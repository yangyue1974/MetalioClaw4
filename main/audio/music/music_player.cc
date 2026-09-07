#include "music_player.h"

#include <esp_aac_dec.h>
#include <esp_ae_rate_cvt.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "audio/audio_codec.h"
#include "audio/demuxer/m4a_demuxer.h"
#include "board.h"

#define TAG "MusicPlayer"
// 注意:本工程 sdkconfig 开了 newlib nano printf,64 位 / size_t 格式符不支持,
// 会把可变参数错位,直接 Load access fault。这里只用 32 位格式 + 显式强转。

MusicPlayer::MusicPlayer(AudioCodec* codec, std::string api_key)
    : codec_(codec), client_(api_key), api_key_empty_(api_key.empty()) {}

void MusicPlayer::Start() {
    if (task_alive_.load()) return;
    exit_requested_.store(false);
    task_alive_.store(true);
    output_alive_.store(true);
    // TLS 握手 + AAC 解码都在这个任务里,8K 会栈溢出。P4 栈可以放 PSRAM
    // (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY),但 TLS 里有 DMA/中断路径,保守放内部。
    xTaskCreate([](void* arg) {
        auto* self = static_cast<MusicPlayer*>(arg);
        self->Run();
        self->task_alive_.store(false);
        vTaskDelete(nullptr);
    }, "music", 24576, this, 3, nullptr);
    // 输出任务只做一件事:从 PCM 队列取块写 I2S。I2S 从机写是阻塞的,天然节拍。
    xTaskCreate([](void* arg) {
        auto* self = static_cast<MusicPlayer*>(arg);
        self->OutputRun();
        self->output_alive_.store(false);
        vTaskDelete(nullptr);
    }, "music_out", 6144, this, 5, nullptr);
    ESP_LOGI(TAG, "tasks started, out rate %d", codec_->output_sample_rate());
}

void MusicPlayer::Shutdown() {
    if (!task_alive_.load() && !output_alive_.load()) return;
    StopPlayback();
    exit_requested_.store(true);
    pcm_cv_.notify_all();
    while (task_alive_.load() || output_alive_.load()) vTaskDelay(pdMS_TO_TICKS(20));
    ClearPcm();
    ESP_LOGI(TAG, "tasks stopped, sram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void MusicPlayer::PlayLibrary(const std::string& library) {
    // 点下去的瞬间就关 WiFi 省电 —— fetch 和建流是最吃延迟的两步。
    // S3 上实测:modem sleep 下电台只在 DTIM 周期醒,吞吐塌到几 KB/s,是卡顿主因。
    // 这里 WiFi 在 C5 从片上,esp_wifi_remote 把这个调用转过去;4G 模式下返回错误,无害。
    esp_wifi_set_ps(WIFI_PS_NONE);
    {
        std::lock_guard<std::mutex> lock(lib_mutex_);
        current_library_ = library;
    }
    paused_.store(false);
    switch_requested_.store(true);   // 中断当前曲目,立即返回
    pcm_cv_.notify_all();
}

void MusicPlayer::StopPlayback() {
    {
        std::lock_guard<std::mutex> lock(lib_mutex_);
        current_library_.clear();
    }
    paused_.store(false);
    switch_requested_.store(true);
    pcm_cv_.notify_all();
}

void MusicPlayer::TogglePause() {
    if (!playing_.load()) return;
    paused_.store(!paused_.load());
    pcm_cv_.notify_all();
    ESP_LOGI(TAG, "%s", paused_.load() ? "paused" : "resumed");
}

// ---------------------------------------------------------------------------
// PCM 队列
// ---------------------------------------------------------------------------

bool MusicPlayer::PushPcm(std::vector<int16_t>&& chunk) {
    if (chunk.empty()) return true;
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    pcm_cv_.wait(lock, [this] {
        return switch_requested_.load() || exit_requested_.load() || pcm_queue_.size() < kQueueDepth;
    });
    if (switch_requested_.load() || exit_requested_.load()) return false;
    pcm_queue_.push_back(std::move(chunk));
    pcm_cv_.notify_all();
    return true;
}

void MusicPlayer::ClearPcm() {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pcm_queue_.clear();
    pcm_cv_.notify_all();
}

void MusicPlayer::OutputRun() {
    while (!exit_requested_.load()) {
        std::vector<int16_t> chunk;
        {
            std::unique_lock<std::mutex> lock(pcm_mutex_);
            pcm_cv_.wait(lock, [this] {
                return exit_requested_.load() || (!pcm_queue_.empty() && !paused_.load());
            });
            if (exit_requested_.load()) break;
            chunk = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
            pcm_cv_.notify_all();
        }
        codec_->EnableOutput(true);
        codec_->OutputData(chunk);   // I2S 从机写,阻塞到 DMA 吃下为止
    }
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

void MusicPlayer::Run() {
    int consecutive_failures = 0;
    std::string failed_lib;
    while (!exit_requested_.load()) {
        std::string lib;
        {
            std::lock_guard<std::mutex> lock(lib_mutex_);
            lib = current_library_;
        }
        if (lib.empty()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (exit_requested_.load()) break;
        switch_requested_.store(false);
        ClearPcm();
        playing_.store(true);
        bool ok = PlayOneTrack(lib);
        playing_.store(false);

        if (ok) {
            consecutive_failures = 0;
            continue;                 // 正常播完 -> 接着取下一首(连放)
        }
        if (switch_requested_.load()) {
            ClearPcm();
            continue;                 // 是被切库/停止打断的,不算失败
        }
        // 取曲或建流失败。连续失败要停下来 —— 否则会每 2 秒重试一次,
        // 把配额烧光(每次 /v1/audio 都计一次)。
        if (lib != failed_lib) { failed_lib = lib; consecutive_failures = 0; }
        if (++consecutive_failures >= 3) {
            ESP_LOGE(TAG, "%s failed %d times, giving up", lib.c_str(), consecutive_failures);
            if (listener_) listener_->OnError("LIBRARY UNAVAILABLE");
            std::lock_guard<std::mutex> lock(lib_mutex_);
            if (current_library_ == lib) current_library_.clear();
            consecutive_failures = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // 退出时把 WiFi 省电还回去
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

bool MusicPlayer::PlayOneTrack(const std::string& library) {
    const int out_rate = codec_->output_sample_rate() > 0 ? codec_->output_sample_rate() : 16000;
    int64_t t0 = esp_timer_get_time();
    AudiolibTrack track;
    if (!client_.FetchTrack(library, &track)) {
        ESP_LOGE(TAG, "fetch failed: %s", client_.last_error().c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));     // 失败别打满配额
        return false;
    }
    int64_t t_fetch = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "\"%s\" %ds quota %d/%d  (fetch %ldms)", track.title.c_str(),
             track.duration_sec, track.quota_remaining, track.quota_total, (long)t_fetch);
    if (listener_) listener_->OnTrack(library.c_str(), track.title.c_str(),
                                      track.quota_remaining, track.quota_total);

    auto open_stream = [&](const char* range) {
        int64_t ts = esp_timer_get_time();
        auto h = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (range) h->SetHeader("Range", range);
        if (!h->Open("GET", track.url)) {
            ESP_LOGE(TAG, "open failed (range=%s)", range ? range : "-");
            return decltype(h){};
        }
        int st = h->GetStatusCode();
        if (st != 200 && st != 206) {
            ESP_LOGE(TAG, "http %d (range=%s)", st, range ? range : "-");
            h->Close();
            return decltype(h){};
        }
        ESP_LOGI(TAG, "stream open %ldms status=%d len=%u range=%s",
                 (long)((esp_timer_get_time() - ts) / 1000), st, (unsigned)h->GetBodyLength(),
                 range ? range : "-");
        return h;
    };

    auto http = open_stream(nullptr);
    if (!http) return false;

    void* dec = nullptr;
    esp_ae_rate_cvt_handle_t cvt = nullptr;
    M4aDemuxer demuxer;
    uint32_t decoded = 0;
    uint64_t abs_pos = 0;
    int retries = 0;
    std::vector<int16_t> pcm(8192);
    std::vector<int16_t> mono(2048);
    std::vector<int16_t> res(4096);
    // 攒到 0.25 秒再推:块大于 SPIRAM_MALLOC_ALWAYSINTERNAL(4096 字节)才走 PSRAM;
    // 同时队列条目少、缓冲深。16kHz 下 0.25s = 4000 样本 = 8000 字节。
    const size_t chunk_samples = (size_t)out_rate / 4;
    std::vector<int16_t> outbuf;
    outbuf.reserve(chunk_samples + 2048);
    std::vector<uint8_t> buf(16384);
    bool aborted = false;
    int64_t t_net = 0, t_dec = 0, t_push = 0, t_rs = 0;
    uint32_t reads = 0;

    demuxer.OnFrame([&](const uint8_t* data, size_t len, uint32_t idx) {
        if (dec == nullptr) return;
        while (paused_.load() && !switch_requested_.load() && !exit_requested_.load()) vTaskDelay(pdMS_TO_TICKS(50));
        if (switch_requested_.load() || exit_requested_.load()) return;

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(data);
        raw.len = len;
        esp_audio_dec_out_frame_t out = {};
        out.buffer = reinterpret_cast<uint8_t*>(pcm.data());
        out.len = pcm.size() * sizeof(int16_t);
        esp_audio_dec_info_t info = {};
        int64_t ta = esp_timer_get_time();
        if (esp_aac_dec_decode(dec, &raw, &out, &info) != ESP_AUDIO_ERR_OK) return;
        int64_t tb = esp_timer_get_time();

        int ch = info.channel ? info.channel : 2;
        int n = out.decoded_size / sizeof(int16_t) / ch;
        if (n <= 0) return;

        if ((int)mono.size() < n) mono.resize(n);
        if (ch == 2) {
            for (int i = 0; i < n; i++) mono[i] = (pcm[i * 2] + pcm[i * 2 + 1]) / 2;
        } else {
            memcpy(mono.data(), pcm.data(), n * sizeof(int16_t));
        }
        uint32_t cap = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(cvt, n, &cap);
        if (res.size() < cap) res.resize(cap);
        uint32_t got = cap;
        if (esp_ae_rate_cvt_process(cvt, mono.data(), n, res.data(), &got) != ESP_AE_ERR_OK) return;
        int64_t tc = esp_timer_get_time();
        outbuf.insert(outbuf.end(), res.begin(), res.begin() + got);
        if (outbuf.size() >= chunk_samples) {
            PushPcm(std::move(outbuf));
            outbuf.clear();
            outbuf.reserve(chunk_samples + 2048);
        }
        int64_t td = esp_timer_get_time();
        t_dec += tb - ta; t_rs += tc - tb; t_push += td - tc;

        ++decoded;
        uint32_t total = demuxer.info().frame_count;
        int rate = demuxer.info().sample_rate ? demuxer.info().sample_rate : 44100;
        if (total > 0 && decoded % 40 == 0 && listener_) {
            listener_->OnProgress((int)((int64_t)decoded * 1024 / rate),
                                  (int)((int64_t)total * 1024 / rate));
        }
        if (decoded % 200 == 0) {
            // 四段计时:net / dec / rs / push。卡顿先看这行,别猜。
            ESP_LOGI(TAG, "f%u/%u net=%ldms dec=%ldms rs=%ldms push=%ldms reads=%u sram=%u",
                     (unsigned)idx, (unsigned)total, (long)(t_net/1000), (long)(t_dec/1000),
                     (long)(t_rs/1000), (long)(t_push/1000),
                     (unsigned)reads, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    });
    demuxer.Reset();

    auto open_decoder = [&]() -> bool {
        const auto& a = demuxer.info();
        ESP_LOGI(TAG, "audio %d Hz %d ch AOT %d, %u frames -> out %d Hz mono",
                 a.sample_rate, a.channels, a.object_type, a.frame_count, out_rate);
        esp_aac_dec_cfg_t dc = {};
        dc.sample_rate = a.sample_rate;
        dc.channel = (uint8_t)a.channels;
        dc.bits_per_sample = 16;
        dc.no_adts_header = true;
        if (esp_aac_dec_open(&dc, sizeof(dc), &dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "aac open failed");
            return false;
        }
        esp_ae_rate_cvt_cfg_t rc = {};
        rc.src_rate = (uint32_t)a.sample_rate;
        rc.dest_rate = (uint32_t)out_rate;
        rc.channel = 1;
        rc.bits_per_sample = 16;
        rc.complexity = 2;
        if (esp_ae_rate_cvt_open(&rc, &cvt) != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "resampler open failed");
            return false;
        }
        return true;
    };

    auto pump = [&](decltype(http)& h) {
        while (!switch_requested_.load() && !exit_requested_.load()) {
            int64_t tn = esp_timer_get_time();
            int n = h->Read(reinterpret_cast<char*>(buf.data()), buf.size());
            t_net += esp_timer_get_time() - tn;
            if (n > 0) reads++;
            if (n <= 0) {
                // 读到 0/负数不一定是播完 —— 网络抖一下也是这个表现。
                if (retries >= 3) {
                    ESP_LOGW(TAG, "stream ended at %lu", (unsigned long)abs_pos);
                    return;
                }
                retries++;
                char rg[48];
                snprintf(rg, sizeof(rg), "bytes=%lu-", (unsigned long)abs_pos);
                ESP_LOGW(TAG, "read=%d, resume at %lu (%d/3)", n,
                         (unsigned long)abs_pos, retries);
                auto nh = open_stream(rg);
                if (!nh) return;
                h->Close();
                h = std::move(nh);
                continue;
            }
            retries = 0;
            abs_pos += n;
            demuxer.Process(buf.data(), (size_t)n);
            if (demuxer.HasError()) return;
            if (dec == nullptr && demuxer.HeaderReady() && !open_decoder()) return;
        }
        aborted = true;
    };

    pump(http);

    // 非 faststart(moov 在 mdat 之后):Range 取文件尾拿回 moov 再续播。
    // Audiolib 的文件不都是 faststart,实测有 moov 在尾的。不要用一个样本下结论。
    if (!aborted && demuxer.HasError() &&
        strstr(demuxer.error(), "not faststart") != nullptr) {
        uint64_t mdat_off = demuxer.mdat_offset(), mdat_sz = demuxer.mdat_size();
        ESP_LOGW(TAG, "not faststart; mdat at %lu size %lu",
                 (unsigned long)mdat_off, (unsigned long)mdat_sz);
        http->Close();
        auto tail = open_stream("bytes=-131072");
        if (tail) {
            std::vector<uint8_t> tb;
            tb.reserve(131072);
            std::vector<uint8_t> tmp(4096);
            while (!switch_requested_.load() && !exit_requested_.load()) {
                int n = tail->Read(reinterpret_cast<char*>(tmp.data()), tmp.size());
                if (n <= 0) break;
                tb.insert(tb.end(), tmp.begin(), tmp.begin() + n);
            }
            tail->Close();
            size_t mo = SIZE_MAX;
            for (size_t i = 4; i + 4 <= tb.size(); i++) {
                if (memcmp(&tb[i], "moov", 4) == 0) { mo = i - 4; break; }
            }
            ESP_LOGI(TAG, "tail %u bytes, moov at %d", (unsigned)tb.size(), mo == SIZE_MAX ? -1 : (int)mo);
            if (mo != SIZE_MAX) {
                uint32_t msz = ((uint32_t)tb[mo] << 24) | ((uint32_t)tb[mo + 1] << 16) |
                               ((uint32_t)tb[mo + 2] << 8) | tb[mo + 3];
                if (msz >= 8 && mo + msz <= tb.size()) {
                    demuxer.Reset();
                    if (demuxer.LoadMoov(&tb[mo + 8], msz - 8) && open_decoder()) {
                        char rg[48];
                        snprintf(rg, sizeof(rg), "bytes=%lu-", (unsigned long)mdat_off);
                        auto body = open_stream(rg);
                        if (body) {
                            demuxer.StartMdat(mdat_sz);
                            abs_pos = mdat_off;
                            retries = 0;
                            pump(body);
                            body->Close();
                        }
                    } else {
                        ESP_LOGE(TAG, "tail moov unusable: %s",
                                 demuxer.HasError() ? demuxer.error() : "decoder open failed");
                    }
                }
            }
        }
    }

    if (!outbuf.empty() && !switch_requested_.load() && !exit_requested_.load()) {
        PushPcm(std::move(outbuf));
    }
    // 正常播完:等队列排空再取下一首,不然下一首的 CONNECTING 会把这首的尾巴吞掉
    if (!aborted) {
        for (int i = 0; i < 200 && !switch_requested_.load() && !exit_requested_.load(); ++i) {
            { std::lock_guard<std::mutex> lock(pcm_mutex_); if (pcm_queue_.empty()) break; }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    ESP_LOGI(TAG, "track done, %u frames", decoded);
    if (dec) esp_aac_dec_close(dec);
    if (cvt) esp_ae_rate_cvt_close(cvt);
    http->Close();
    return !aborted;
}
