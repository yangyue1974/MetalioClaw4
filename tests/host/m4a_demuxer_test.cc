#include "../../main/audio/demuxer/m4a_demuxer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> LoadFixture() {
    FILE* f = fopen("tests/fixtures/audiolib-sample-head.m4a", "rb");
    assert(f && "fixture not found; run from repo root");
    std::vector<uint8_t> buf(65536);
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    buf.resize(n);
    assert(n == 65536);
    return buf;
}

static void TestHeaderParsed() {
    auto data = LoadFixture();
    M4aDemuxer d;
    d.Reset();
    d.Process(data.data(), data.size());
    assert(!d.HasError());
    assert(d.HeaderReady());
    const auto& i = d.info();
    printf("sample_rate=%d channels=%d object_type=%d frames=%u asc_len=%zu\n",
           i.sample_rate, i.channels, i.object_type, i.frame_count, i.asc_len);
    assert(i.sample_rate == 44100);
    assert(i.channels == 2);
    assert(i.object_type == 2);          // AAC-LC
    assert(i.frame_count == 5030);
    assert(i.asc_len == 5);
    const uint8_t expected_asc[5] = {0x12, 0x10, 0x56, 0xe5, 0x00};
    assert(memcmp(i.asc, expected_asc, 5) == 0);
    printf("TestHeaderParsed PASS\n");
}

static void TestFramesEmitted() {
    auto data = LoadFixture();
    M4aDemuxer d;
    std::vector<size_t> sizes;
    std::vector<uint32_t> indices;
    d.OnFrame([&](const uint8_t* p, size_t len, uint32_t idx) {
        assert(p != nullptr);
        sizes.push_back(len);
        indices.push_back(idx);
    });
    d.Reset();
    d.Process(data.data(), data.size());
    assert(!d.HasError());
    // fixture 只有 64KB,mdat 从 20955 开始,所以只能切出开头若干帧
    assert(sizes.size() >= 5);
    printf("emitted %zu frames, first5 = %zu %zu %zu %zu %zu\n",
           sizes.size(), sizes[0], sizes[1], sizes[2], sizes[3], sizes[4]);
    assert(sizes[0] == 411);
    assert(sizes[1] == 402);
    assert(sizes[2] == 452);
    assert(sizes[3] == 413);
    assert(sizes[4] == 494);
    for (size_t i = 0; i < indices.size(); i++) assert(indices[i] == i);
    printf("TestFramesEmitted PASS\n");
}

static void TestChunkedFeedMatches() {
    auto data = LoadFixture();
    // 一次性喂
    std::vector<size_t> whole;
    { M4aDemuxer d; d.OnFrame([&](const uint8_t*, size_t l, uint32_t){ whole.push_back(l); });
      d.Reset(); d.Process(data.data(), data.size()); assert(!d.HasError()); }
    // 按 7 字节的碎块喂(7 是质数,能踩到各种边界)
    std::vector<size_t> chunked;
    { M4aDemuxer d; d.OnFrame([&](const uint8_t*, size_t l, uint32_t){ chunked.push_back(l); });
      d.Reset();
      for (size_t o = 0; o < data.size(); o += 7) {
          size_t n = (data.size() - o < 7) ? data.size() - o : 7;
          d.Process(data.data() + o, n);
      }
      assert(!d.HasError()); }
    printf("whole=%zu chunked=%zu\n", whole.size(), chunked.size());
    assert(whole == chunked);
    printf("TestChunkedFeedMatches PASS\n");
}

static void AppendBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

static std::vector<uint8_t> MakeBox(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> box;
    AppendBe32(box, uint32_t(8 + payload.size()));
    box.insert(box.end(), type, type + 4);
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

// 手搓一段 stco 偏移不连续的 moov(不需要 mdhd/stsd/esds——校验在
// ParseStbl 读完 stsz 之后、还没走到 stsd 之前就会失败并返回),
// 验证 review 点名的"连续性校验路径"真的会被踩到、真的会报错,
// 而不是只靠人工审阅背书。
static void TestNonContiguousChunksRejected() {
    std::vector<uint8_t> stsz_payload;
    AppendBe32(stsz_payload, 0);     // version/flags
    AppendBe32(stsz_payload, 100);   // uniform sample size
    AppendBe32(stsz_payload, 3);     // sample count
    auto stsz = MakeBox("stsz", stsz_payload);

    std::vector<uint8_t> stco_payload;
    AppendBe32(stco_payload, 0);     // version/flags
    AppendBe32(stco_payload, 2);     // entry_count = 2 chunks
    AppendBe32(stco_payload, 1000);  // chunk_offset[0] = base
    // chunk_offset[1] 应该是 1000 + 2*100 = 1200 才连续,这里故意错开
    AppendBe32(stco_payload, 5000);
    auto stco = MakeBox("stco", stco_payload);

    std::vector<uint8_t> stsc_payload;
    AppendBe32(stsc_payload, 0);     // version/flags
    AppendBe32(stsc_payload, 1);     // entry_count = 1
    AppendBe32(stsc_payload, 1);     // first_chunk = 1
    AppendBe32(stsc_payload, 2);     // samples_per_chunk = 2(套用到最后一个 chunk 为止)
    AppendBe32(stsc_payload, 1);     // sample_description_index(未使用)
    auto stsc = MakeBox("stsc", stsc_payload);

    std::vector<uint8_t> stbl_payload;
    stbl_payload.insert(stbl_payload.end(), stsz.begin(), stsz.end());
    stbl_payload.insert(stbl_payload.end(), stco.begin(), stco.end());
    stbl_payload.insert(stbl_payload.end(), stsc.begin(), stsc.end());
    auto stbl = MakeBox("stbl", stbl_payload);
    auto minf = MakeBox("minf", stbl);
    auto mdia = MakeBox("mdia", minf);
    auto trak = MakeBox("trak", mdia);
    auto moov = MakeBox("moov", trak);

    M4aDemuxer d;
    d.Reset();
    d.Process(moov.data(), moov.size());
    assert(d.HasError());
    printf("non-contiguous chunks rejected: %s\n", d.error());
    assert(strcmp(d.error(), "non-contiguous chunks unsupported") == 0);
    printf("TestNonContiguousChunksRejected PASS\n");
}

// moov 声明尺寸远超合理上限(256KB)时,必须在真正尝试为它分配内存
// 之前就拒绝——只喂 8 字节的 atom 头(不喂任何 moov 正文),如果解容器
// 在读到足够字节之前就先 SetError,说明上限检查确实发生在 reserve
// 之前,而不是等 moov body 攒够了才检查。
static void TestOversizedMoovRejected() {
    std::vector<uint8_t> hdr;
    AppendBe32(hdr, 300 * 1024);  // > 256KB 上限
    const uint8_t type[4] = {'m', 'o', 'o', 'v'};
    hdr.insert(hdr.end(), type, type + 4);
    M4aDemuxer d;
    d.Reset();
    d.Process(hdr.data(), hdr.size());
    assert(d.HasError());
    printf("oversized moov rejected: %s\n", d.error());
    printf("TestOversizedMoovRejected PASS\n");
}

// stsz 声明的 sample_count 超过 kMaxFrameCount(100000,约 38.7 分钟音频,
// 是最长实测 Audiolib 曲目的 20 倍)时必须被拒绝——这条检查在 uniform
// 分支的 frame_sizes_.assign(count, uniform) 之前触发,不需要构造
// stco/stsc(检查发生在那之前就已经 return false 了)。断言真的踩到
// 那一行,而不是只靠代码审查背书。
static void TestTooManyFramesRejected() {
    std::vector<uint8_t> stsz_payload;
    AppendBe32(stsz_payload, 0);        // version/flags
    AppendBe32(stsz_payload, 100);      // uniform sample size(非零 = uniform 模式)
    AppendBe32(stsz_payload, 100001);   // sample_count,比 100000 上限多 1
    auto stsz = MakeBox("stsz", stsz_payload);

    auto stbl = MakeBox("stbl", stsz);
    auto minf = MakeBox("minf", stbl);
    auto mdia = MakeBox("mdia", minf);
    auto trak = MakeBox("trak", mdia);
    auto moov = MakeBox("moov", trak);

    M4aDemuxer d;
    d.Reset();
    d.Process(moov.data(), moov.size());
    assert(d.HasError());
    printf("too many frames rejected: %s\n", d.error());
    assert(strcmp(d.error(), "stsz frame_count exceeds 100000-frame limit") == 0);
    printf("TestTooManyFramesRejected PASS\n");
}

static void TestGarbageRejected() {
    // mdat 在 moov 之前(非 faststart)必须报错
    uint8_t bad[16] = {0,0,0,8,'m','d','a','t', 0,0,0,8,'m','o','o','v'};
    M4aDemuxer d;
    d.Reset();
    d.Process(bad, sizeof(bad));
    assert(d.HasError());
    printf("non-faststart rejected: %s\n", d.error());

    // 全零输入(atom size = 0)必须报错,不能死循环
    uint8_t zeros[64] = {};
    M4aDemuxer d2;
    d2.Reset();
    d2.Process(zeros, sizeof(zeros));
    assert(d2.HasError());
    printf("TestGarbageRejected PASS\n");
}

int main() {
    TestHeaderParsed();
    TestFramesEmitted();
    TestChunkedFeedMatches();
    TestGarbageRejected();
    TestNonContiguousChunksRejected();
    TestOversizedMoovRejected();
    TestTooManyFramesRejected();
    printf("ALL PASS\n");
    return 0;
}
