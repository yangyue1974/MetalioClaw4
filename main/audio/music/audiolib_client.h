#ifndef AUDIOLIB_CLIENT_H_
#define AUDIOLIB_CLIENT_H_

#include <string>

struct AudiolibTrack {
    std::string title;
    std::string url;
    int duration_sec    = 0;
    int quota_remaining = 0;
    int quota_total     = 0;
};

// audiolib.ai 的 API 客户端。
// 只有一个端点:POST /v1/audio,从指定曲库里随机取一首。
// 返回的 url 是预签名的,约 40 分钟过期,不得跨重启缓存。
class AudiolibClient {
public:
    // api_key 是 audiolib.ai 签发的 bearer token,来自 gitignore 的 audiolib_secret.h。
    explicit AudiolibClient(std::string api_key) : api_key_(std::move(api_key)) {}

    // library 形如 "audio.lo-fi"。成功返回 true 并填充 out。
    bool FetchTrack(const std::string& library, AudiolibTrack* out);

    const std::string& last_error() const { return last_error_; }

private:
    std::string api_key_;
    std::string last_error_;
};

#endif  // AUDIOLIB_CLIENT_H_
