#include "m4a_demuxer.h"
#include <cstring>

namespace {

// moov 一次性整块缓冲,但必须设上限——声明尺寸来自不可信的文件头,
// 畸形文件可以把它写成接近 4GB。真实 moov 大约 20KB,256KB 已经是
// 十倍以上的余量。超过就报错,绝不能在读到那么多字节之前先按声明
// 尺寸 reserve:S3 只有 320KB 内部 SRAM,reserve 本身就可能先崩,
// 而不是等真正越界读写的时候崩。
constexpr size_t kMaxMoovSize = 256 * 1024;

// frame_sizes_(每帧字节数表)最多能有多少帧——跟 kMaxMoovSize 是同一类
// 防护:文件声明一个巨大的帧数,代码不能无条件照着分配。这个具体数值
// 是产品决策(不是解容器自己能推导出来的边界),推导过程:
//   - AAC-LC 每帧固定 1024 个样本;Audiolib 的音频实测 44100 Hz,
//     所以每帧 1024 / 44100 ≈ 23.22 毫秒。
//   - 100000 帧 = 100000 * 0.02322 秒 ≈ 2322 秒 ≈ 38.7 分钟。
//   - 实测的 Audiolib 曲目:117 秒(5030 帧)、240 秒——上限是最长
//     实测值的约 20 倍,对"背景音乐 API"这个产品形态足够宽裕。
//   - 内存上界:100000 * 4 字节(frame_sizes_ 是 vector<uint32_t>)
//     = 400KB。设备有 8MB PSRAM,且 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
//     = 2048,超过 2KB 的分配会走 PSRAM,所以这个上界是安全的。
// 对 uniform(所有帧同大小)和非 uniform(逐帧记录)两条路径都套用同一
// 个上限——非 uniform 路径本身已经被 stsz box 长度自洽性约束住了,套
// 用这条上限只是让"帧表最多 400KB"成为一条不看路径都成立的不变量,
// 而不是"某条路径碰巧安全"。
constexpr uint32_t kMaxFrameCount = 100000;

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// 在 [buf, buf+len) 里找顶层 box。返回负载起点与长度。
//
// 边界检查全部写成减法/除法形式,不写 "a + b > limit" 这种加法/乘法
// 形式——size 来自文件里 4 字节的 box size 字段,是不可信输入,理论
// 上限接近 4G-1。这份代码要跑在 ESP32-S3 上,size_t 是 32 位:
// off + size 这种写法一旦发生整数回绕,会把一个越界的 size 判定成
// "合法",让调用方对着一个巨大的、看似合法的长度去读——SRAM 上就是
// 越界读。主机侧 size_t 是 64 位,回绕不了,ASan/UBSan 测不出这个问题,
// 所以必须靠审查而不是靠跑测试来堵。
bool FindBox(const uint8_t* buf, size_t len, const char* type, size_t* out_off, size_t* out_len) {
    size_t off = 0;
    while (len - off >= 8) {  // 等价于 off + 8 <= len,但不会在 off 接近上限时回绕
        uint32_t size = Be32(buf + off);
        if (size < 8 || len - off < size) return false;  // 不写 off + size > len
        if (memcmp(buf + off + 4, type, 4) == 0) {
            *out_off = off + 8;
            *out_len = size - 8;
            return true;
        }
        off += size;
    }
    return false;
}

// 顺着路径逐层下钻,例如 {"trak","mdia","minf","stbl"}
bool FindPath(const uint8_t* buf, size_t len, const char* const* path, int depth,
              size_t* out_off, size_t* out_len) {
    size_t off = 0, sz = len;
    for (int i = 0; i < depth; i++) {
        size_t o, l;
        if (!FindBox(buf + off, sz, path[i], &o, &l)) return false;
        off += o;
        sz = l;
    }
    *out_off = off;
    *out_len = sz;
    return true;
}

}  // namespace

void M4aDemuxer::Reset() {
    state_ = State::kAtomHeader;
    header_ready_ = false;
    error_ = nullptr;
    info_ = M4aAudioInfo{};
    frame_sizes_.clear();
    moov_buf_.clear();
    mdat_buf_.clear();
    atom_hdr_len_ = 0;
    atom_remaining_ = 0;
    next_frame_ = 0;
    mdat_consumed_ = 0;
    mdat_offset_ = 0;
    mdat_size_ = 0;
    file_pos_ = 0;
}

bool M4aDemuxer::ParseEsds(const uint8_t* esds, size_t len) {
    // esds 是 full box:4 字节 version/flags,后面是 MPEG-4 描述符链
    size_t o = 4;
    auto read_desc = [&](uint8_t* tag, size_t* size) -> bool {
        if (o >= len) return false;
        *tag = esds[o++];
        size_t s = 0;
        for (int i = 0; i < 4; i++) {
            if (o >= len) return false;
            uint8_t c = esds[o++];
            s = (s << 7) | (c & 0x7f);
            if (!(c & 0x80)) break;
        }
        *size = s;
        return true;
    };
    uint8_t tag; size_t sz;
    if (!read_desc(&tag, &sz) || tag != 0x03) return false;  // ES_Descriptor
    if (len - o < 3) return false;
    o += 3;                                                  // ES_ID(2) + flags(1)
    if (!read_desc(&tag, &sz) || tag != 0x04) return false;  // DecoderConfigDescriptor
    if (len - o < 13) return false;
    o += 13;                                                 // 固定字段
    if (!read_desc(&tag, &sz) || tag != 0x05) return false;  // DecoderSpecificInfo
    if (sz < 2 || sz > sizeof(info_.asc) || len - o < sz) return false;
    memcpy(info_.asc, esds + o, sz);
    info_.asc_len = sz;

    static const int kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000,
                                   24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint8_t b0 = info_.asc[0], b1 = info_.asc[1];
    info_.object_type = (b0 >> 3) & 0x1f;
    int fi = ((b0 & 0x07) << 1) | (b1 >> 7);
    info_.channels = (b1 >> 3) & 0x0f;
    if (fi < 13 && info_.sample_rate == 0) info_.sample_rate = kRates[fi];
    return true;
}

bool M4aDemuxer::ParseStbl(const uint8_t* stbl, size_t len) {
    size_t o, l;

    // stsz:每帧字节数。这是切帧的唯一依据。
    if (!FindBox(stbl, len, "stsz", &o, &l) || l < 12) { SetError("stsz missing"); return false; }
    uint32_t uniform = Be32(stbl + o + 4);
    uint32_t count   = Be32(stbl + o + 8);
    if (count == 0) { SetError("stsz empty"); return false; }
    if (count > kMaxFrameCount) { SetError("stsz frame_count exceeds 100000-frame limit"); return false; }
    if (uniform != 0) {
        frame_sizes_.assign(count, uniform);
    } else {
        // 溢出安全:用除法而不是 "12 + count*4" 这种乘法比较——count 是
        // 文件里的 32 位字段、不可信,在 32 位 size_t 上 count*4 可能回绕,
        // 让本该拒绝的"声明帧数超过box实际能装下的量"被判定成合法,
        // 之后的循环就会越界读 stbl。同理,这里也把 resize 放到检查
        // 通过之后,不在校验前就先按不可信的 count 去分配。
        if (count > (l - 12) / 4) { SetError("stsz truncated"); return false; }
        frame_sizes_.resize(count);
        for (uint32_t i = 0; i < count; i++) frame_sizes_[i] = Be32(stbl + o + 12 + i * 4);
    }
    info_.frame_count = count;

    // stco + stsc:本解容器假设所有 chunk 在 mdat 里连续排列(Audiolib 实测如此),
    // 因此顺序切帧即可,不需要 chunk 偏移。但必须校验这个假设——
    // 不连续,或者 stco/stsc 表头声明的尺寸跟盒子实际数据自相矛盾,
    // 都要报错,不能因为"校验不了"就默默当成连续处理。
    size_t co, cl, sc, sl;
    bool have_stco = FindBox(stbl, len, "stco", &co, &cl);
    bool have_stsc = FindBox(stbl, len, "stsc", &sc, &sl);
    if (!have_stco || !have_stsc) { SetError("stco/stsc missing"); return false; }
    if (cl < 8 || sl < 8) { SetError("stco/stsc truncated"); return false; }
    uint32_t nchunk = Be32(stbl + co + 4);
    uint32_t nentry = Be32(stbl + sc + 4);
    // 溢出安全的容量校验,原理同 stsz:除法代替乘法比较。
    if (nchunk > (cl - 8) / 4)  { SetError("stco truncated"); return false; }
    if (nentry > (sl - 8) / 12) { SetError("stsc truncated"); return false; }
    if (nchunk == 0) { SetError("stco has no chunks"); return false; }
    if (nchunk > 1) {
        if (nentry == 0) { SetError("stsc empty but multiple chunks"); return false; }
        // 展开 stsc 得到每个 chunk 的样本数
        std::vector<uint32_t> spc;
        spc.reserve(nchunk);
        for (uint32_t e = 0; e < nentry; e++) {
            uint32_t first   = Be32(stbl + sc + 8 + e * 12);
            uint32_t samples = Be32(stbl + sc + 8 + e * 12 + 4);
            uint32_t last;
            if (e + 1 < nentry) {
                uint32_t next_first = Be32(stbl + sc + 8 + (e + 1) * 12);
                if (next_first == 0) { SetError("stsc malformed"); return false; }  // 避免 -1 回绕
                last = next_first - 1;
            } else {
                last = nchunk;
            }
            if (first == 0 || first > last) { SetError("stsc malformed"); return false; }
            for (uint32_t c = first; c <= last && spc.size() < nchunk; c++) spc.push_back(samples);
        }
        if (spc.size() != nchunk) { SetError("stsc does not cover all chunks"); return false; }
        uint32_t base = Be32(stbl + co + 8);
        uint64_t cur = base;
        uint32_t idx = 0;
        for (uint32_t c = 0; c < nchunk; c++) {
            if (Be32(stbl + co + 8 + c * 4) != cur) {
                SetError("non-contiguous chunks unsupported");
                return false;
            }
            for (uint32_t k = 0; k < spc[c] && idx < count; k++) cur += frame_sizes_[idx++];
        }
    }
    // nchunk == 1 时单 chunk 天然连续,无需进一步校验。

    // mdhd 的 timescale 才是权威采样率,优先于 esds 里的索引
    // (mdhd 在 mdia 层,不在 stbl,由 ParseMoov 负责)

    // esds:AudioSpecificConfig。stsd 直接在 stbl 下,不需要多层路径。
    if (!FindBox(stbl, len, "stsd", &o, &l)) { SetError("stsd missing"); return false; }
    // stsd 是 full box:version/flags(4) + entry_count(4)
    if (l < 8) { SetError("stsd truncated"); return false; }
    const uint8_t* entries = stbl + o + 8;
    size_t entries_len = l - 8;
    size_t mo, ml;
    if (!FindBox(entries, entries_len, "mp4a", &mo, &ml)) { SetError("mp4a missing"); return false; }
    // mp4a sample entry:28 字节固定头,之后是子 box
    if (ml < 28) { SetError("mp4a truncated"); return false; }
    const uint8_t* sub = entries + mo + 28;
    size_t sub_len = ml - 28;
    size_t eo, el;
    if (!FindBox(sub, sub_len, "esds", &eo, &el)) { SetError("esds missing"); return false; }
    if (!ParseEsds(sub + eo, el)) { SetError("esds parse failed"); return false; }
    return true;
}

bool M4aDemuxer::ParseMoov(const uint8_t* moov, size_t len) {
    // mdhd 的 timescale 是权威采样率
    static const char* kMdhdPath[] = {"trak", "mdia"};
    size_t o, l;
    if (FindPath(moov, len, kMdhdPath, 2, &o, &l)) {
        size_t mo, ml;
        if (FindBox(moov + o, l, "mdhd", &mo, &ml) && ml >= 20) {
            const uint8_t* p = moov + o + mo;
            uint8_t ver = p[0];
            // full box: version(1)+flags(3) 之后:
            // v0: creation_time(4) + modification_time(4) + timescale(4)  -> timescale @ 12
            // v1: creation_time(8) + modification_time(8) + timescale(4) -> timescale @ 20
            if (ver == 0) {
                info_.sample_rate = int(Be32(p + 12));
            } else if (ml >= 24) {
                info_.sample_rate = int(Be32(p + 20));
            }
        }
    }
    static const char* kStblPath[] = {"trak", "mdia", "minf", "stbl"};
    if (!FindPath(moov, len, kStblPath, 4, &o, &l)) { SetError("stbl missing"); return false; }
    if (!ParseStbl(moov + o, l)) return false;
    if (info_.sample_rate <= 0 || info_.channels <= 0) { SetError("bad audio params"); return false; }
    header_ready_ = true;
    return true;
}

void M4aDemuxer::EmitFrames() {
    // mdat_consumed_ 是游标,标记 mdat_buf_ 里已经切出去的字节数。
    // 只在游标越过缓冲一半时才整体压缩一次,避免每帧都 erase 导致 O(n^2)。
    while (next_frame_ < frame_sizes_.size()) {
        size_t need = frame_sizes_[next_frame_];
        if (mdat_buf_.size() - mdat_consumed_ < need) break;
        if (on_frame_) on_frame_(mdat_buf_.data() + mdat_consumed_, need, next_frame_);
        mdat_consumed_ += need;
        next_frame_++;
    }
    // mdat_consumed_ >= mdat_buf_.size() - mdat_consumed_ 等价于 "游标过半",
    // 但不用 mdat_consumed_ * 2 这种乘法形式。
    if (mdat_consumed_ > 0 && mdat_consumed_ >= mdat_buf_.size() - mdat_consumed_) {
        mdat_buf_.erase(mdat_buf_.begin(), mdat_buf_.begin() + mdat_consumed_);
        mdat_consumed_ = 0;
    }
}

size_t M4aDemuxer::Process(const uint8_t* data, size_t size) {
    size_t off = 0;
    const uint64_t base_pos = file_pos_;
    file_pos_ += size;
    while (off < size && state_ != State::kDone) {
        switch (state_) {
        case State::kAtomHeader: {
            size_t want = 8 - atom_hdr_len_;
            size_t take = (size - off < want) ? size - off : want;
            memcpy(atom_hdr_ + atom_hdr_len_, data + off, take);
            atom_hdr_len_ += take;
            off += take;
            if (atom_hdr_len_ < 8) break;
            uint32_t asize = Be32(atom_hdr_);
            char type[5] = {};
            memcpy(type, atom_hdr_ + 4, 4);
            atom_hdr_len_ = 0;
            if (asize < 8) { SetError("bad atom size"); break; }
            atom_remaining_ = asize - 8;
            if (memcmp(type, "moov", 4) == 0) {
                if (header_ready_) { SetError("duplicate moov"); break; }
                if (atom_remaining_ > kMaxMoovSize) { SetError("moov too large"); break; }
                moov_buf_.clear();
                moov_buf_.reserve(atom_remaining_);
                state_ = State::kMoovBody;
            } else if (memcmp(type, "mdat", 4) == 0) {
                mdat_offset_ = base_pos + off;   // 紧跟 mdat box 头之后
                mdat_size_ = atom_remaining_;
                if (!header_ready_) { SetError("mdat before moov (not faststart)"); break; }
                state_ = State::kMdat;
            } else {
                state_ = State::kSkip;   // ftyp / free / 其它
            }
            break;
        }
        case State::kMoovBody: {
            size_t take = (size - off < atom_remaining_) ? size - off : size_t(atom_remaining_);
            moov_buf_.insert(moov_buf_.end(), data + off, data + off + take);
            off += take;
            atom_remaining_ -= take;
            if (atom_remaining_ == 0) {
                if (!ParseMoov(moov_buf_.data(), moov_buf_.size())) break;
                moov_buf_.clear();
                moov_buf_.shrink_to_fit();
                state_ = State::kAtomHeader;
            }
            break;
        }
        case State::kSkip: {
            size_t take = (size - off < atom_remaining_) ? size - off : size_t(atom_remaining_);
            off += take;
            atom_remaining_ -= take;
            if (atom_remaining_ == 0) state_ = State::kAtomHeader;
            break;
        }
        case State::kMdat: {
            size_t take = (size - off < atom_remaining_) ? size - off : size_t(atom_remaining_);
            mdat_buf_.insert(mdat_buf_.end(), data + off, data + off + take);
            off += take;
            atom_remaining_ -= take;
            EmitFrames();
            if (atom_remaining_ == 0) state_ = State::kAtomHeader;
            break;
        }
        case State::kDone:
            break;
        }
    }
    return size;
}

bool M4aDemuxer::LoadMoov(const uint8_t* moov_payload, size_t len) {
    error_ = nullptr;
    state_ = State::kAtomHeader;
    return ParseMoov(moov_payload, len);
}

void M4aDemuxer::StartMdat(uint64_t payload_size) {
    mdat_buf_.clear();
    mdat_consumed_ = 0;
    next_frame_ = 0;
    atom_remaining_ = payload_size;
    atom_hdr_len_ = 0;
    error_ = nullptr;
    state_ = State::kMdat;
}
