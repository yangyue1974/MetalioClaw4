#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "audiolib_client.h"

class AudioCodec;

// Audiobar 的播放引擎,从 S3-4B 版移植到 Metalio Claw4。
//
// 链路:Audiolib /v1/audio 取曲 → HTTP Range 拉流 → M4A 解容器 → AAC 软解 →
//       下混单声道 + 重采样到 codec 的输出采样率 → 本地 PCM 队列 → codec->OutputData()。
//
// 和 S3 版的差别:
//   - 不再改 AudioService 的播放队列。Metalio 的 codec 是蓝牙音频 SoC,P4 作 I2S 从机,
//     厂商自己的电台 App 也是绕开 AudioService 直接 codec->OutputData()。这里照做,
//     自带一个 4 秒深的 PCM 队列(kQueueDepth × 0.25s)吸收网络抖动。
//   - 输出采样率读 codec->output_sample_rate()(现在 16000,由蓝牙芯片的 I2S 时钟决定),不写死。
//   - UI 通知走 Listener 接口,实现方负责线程安全(LVGL 线程之外不许碰 LVGL)。
//
// 常驻任务模型:任务建一次就不退出,循环播放。UI 只投递「要播哪个库」然后立刻返回 ——
// 绝不能在触摸回调里等任务退出,那会把 LVGL 线程冻住十几秒(任务可能正卡在 http Read 里)。
class MusicPlayer {
public:
    struct Listener {
        virtual ~Listener() = default;
        // 以下都可能从音频任务调用
        virtual void OnTrack(const char* library_id, const char* title, int quota_remaining, int quota_total) = 0;
        virtual void OnProgress(int elapsed_sec, int total_sec) = 0;
        virtual void OnError(const char* msg) = 0;
    };

    MusicPlayer(AudioCodec* codec, std::string api_key);

    void SetListener(Listener* l) { listener_ = l; }

    // 任务按需启停:进 app 才建,退出就销毁。
    void Start();
    void Shutdown();

    // 立即返回。切库/开播都走它。
    void PlayLibrary(const std::string& library);
    void StopPlayback();          // 停止并保持静默
    void TogglePause();

    bool is_playing() const { return playing_.load(); }
    bool is_paused() const { return paused_.load(); }
    bool has_api_key() const { return !api_key_empty_; }

private:
    void Run();
    // 播一首。返回 false 表示应当停止外层循环(被要求停/切库)。
    bool PlayOneTrack(const std::string& library);

    // PCM 队列:解码任务推,输出任务取。满了推方阻塞;停止/切库时清空并唤醒。
    bool PushPcm(std::vector<int16_t>&& chunk);
    void ClearPcm();
    void OutputRun();

    AudioCodec*    codec_;
    AudiolibClient client_;
    bool           api_key_empty_ = false;
    Listener*      listener_ = nullptr;

    std::mutex  lib_mutex_;
    std::string current_library_;      // 空 = 静默
    std::atomic<bool> switch_requested_{false};   // 要求中断当前曲目
    std::atomic<bool> exit_requested_{false};
    std::atomic<bool> task_alive_{false};
    std::atomic<bool> output_alive_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};

    static constexpr size_t kQueueDepth = 16;   // × 0.25s = 4 秒缓冲垫
    std::mutex pcm_mutex_;
    std::condition_variable pcm_cv_;
    std::deque<std::vector<int16_t>> pcm_queue_;
    // 卡顿排查计数(输出任务写,解码任务在计时行里读):
    std::atomic<uint32_t> underruns_{0};      // 输出任务来取块时队列是空的 → 供不上,听感就是卡
    std::atomic<uint32_t> starve_max_ms_{0};  // 空队列等了最久多少毫秒
    std::atomic<uint32_t> out_max_ms_{0};     // 单次 OutputData 最长阻塞(正常 ≈ 250ms,一块的时长)
};

#endif  // MUSIC_PLAYER_H_
