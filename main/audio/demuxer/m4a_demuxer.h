#ifndef M4A_DEMUXER_H_
#define M4A_DEMUXER_H_

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

struct M4aAudioInfo {
    int      sample_rate  = 0;
    int      channels     = 0;
    int      object_type  = 0;   // 2 = AAC-LC
    uint32_t frame_count  = 0;
    uint8_t  asc[8]       = {};  // AudioSpecificConfig
    size_t   asc_len      = 0;
};

// 流式 MP4/M4A 解容器。只支持 faststart 布局(moov 在 mdat 之前)——
// Audiolib 的文件实测就是这个布局。moov 在后面会 SetError 并停,
// 不会静默产出垃圾。
class M4aDemuxer {
public:
    M4aDemuxer() { Reset(); }

    void Reset();

    // 喂字节。返回本次消费的字节数(总是 size,内部自行缓冲)。
    size_t Process(const uint8_t* data, size_t size);

    bool HeaderReady() const { return header_ready_; }
    const M4aAudioInfo& info() const { return info_; }
    bool HasError() const { return error_ != nullptr; }

    // 非 faststart(moov 在 mdat 之后)时的回退路径:
    // 命中 "mdat before moov" 后,这两个值指出 mdat 负载在文件中的位置,
    // 调用方可用 HTTP Range 去文件尾取回 moov,再走 LoadMoov + StartMdat。
    uint64_t mdat_offset() const { return mdat_offset_; }
    uint64_t mdat_size() const { return mdat_size_; }
    bool LoadMoov(const uint8_t* moov_payload, size_t len);
    void StartMdat(uint64_t payload_size);
    const char* error() const { return error_; }

    // 每解出一个裸 AAC 帧回调一次。data 只在回调期间有效。
    void OnFrame(std::function<void(const uint8_t* data, size_t len, uint32_t frame_index)> cb) {
        on_frame_ = std::move(cb);
    }

private:
    enum class State : uint8_t { kAtomHeader, kMoovBody, kSkip, kMdat, kDone };

    void SetError(const char* msg) { if (!error_) error_ = msg; state_ = State::kDone; }
    bool ParseMoov(const uint8_t* moov, size_t len);
    bool ParseStbl(const uint8_t* stbl, size_t len);
    bool ParseEsds(const uint8_t* esds, size_t len);
    void EmitFrames();

    State    state_        = State::kAtomHeader;
    bool     header_ready_ = false;
    const char* error_     = nullptr;

    M4aAudioInfo info_;
    std::vector<uint32_t> frame_sizes_;   // 来自 stsz
    std::vector<uint8_t>  moov_buf_;      // 完整 moov,约 20 KB
    std::vector<uint8_t>  mdat_buf_;      // 未凑够一帧的余量

    uint8_t  atom_hdr_[8];
    size_t   atom_hdr_len_ = 0;
    uint64_t atom_remaining_ = 0;
    uint32_t next_frame_ = 0;
    uint64_t mdat_offset_ = 0;
    uint64_t mdat_size_ = 0;
    uint64_t file_pos_ = 0;
    size_t   mdat_consumed_ = 0;          // 游标,避免 O(n^2) erase

    std::function<void(const uint8_t*, size_t, uint32_t)> on_frame_;
};

#endif  // M4A_DEMUXER_H_
