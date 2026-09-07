#pragma once

// ---------------------------------------------------------------------------
// BtAudio —— 板上蓝牙音频 SoC 的常驻控制器(UART2 AT 指令)。
//
// 厂商原来把模式 / 连接状态放在设置页的蓝牙 Tab 里,只有那个 Tab 打开时才收
// UART 回显;一离开设置页,模块回什么都没人听,别的 App(Audiobar)也拿不到
// "现在出声的是内置扬声器还是蓝牙音箱"。这里把状态和 AT 序列抽出来常驻,
// 板级初始化时 Init() 一次,之后设置页和 Audiobar 都只是它的听众。
//
// 模式(README §12.1):
//   1  日常小智对话,开机默认,模块自己是 codec,声音走本机喇叭 / 麦
//   2  本机作主机连外接蓝牙耳机 / 音箱(AT+INQUIRING 扫描,AT+CONNECT 连接),
//      连上后 MusicLink() 走 A2DP 放歌,CallLink() 走 SCO 对话(外设要带麦)
//   3  本机作蓝牙音箱,手机连过来推歌
// 实测(2026-09-07):模式 2 + A2DP 下 P4 的 I2S 输出照常 16kHz,Audiobar 不用改采样率。
//
// 线程:AT 序列在自己的小任务里发(中间要等 700ms);回显在 SimpleUart 的
// 接收任务里解析;听众回调也在那个任务里,想碰 LVGL 自己 lv_async_call。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class BtAudio {
public:
    enum class Mode : uint8_t { kNone = 0, kMode1, kMode2, kMode3 };
    enum class Conn : uint8_t { kIdle, kScanning, kConnecting, kConnected };
    enum class Event : uint8_t {
        kMode,          // 模式变了(含乐观设置)
        kScanStart,
        kDeviceFound,
        kScanDone,
        kConnecting,
        kConnected,
        kConnectFailed,
        kDisconnected,
        kLine,          // 任何一行回显(设置页拿它显示原文)
    };
    struct Device {
        std::string addr;   // 12 位十六进制
        std::string name;
    };
    using Listener = std::function<void(Event, const std::string& line)>;

    static BtAudio& GetInstance();

    // 板级在 SimpleUart::begin 之后调一次。之后 SimpleUart 的回调归这里所有,
    // 别的地方不要再 registerCallback。
    void Init();

    // 发 AT 序列(异步)。模式先乐观记下,回显 "SET MODE n" 再确认。
    void SetMode(Mode mode);
    void Scan();                              // 只在模式 2 有意义
    void Connect(const std::string& addr);    // 只在模式 2 有意义
    void MusicLink();                         // AT+BTSCO=0 → AT+PP=1(A2DP)
    void CallLink();                          // AT+PP=1 → AT+BTSCO=1(SCO)
    void PowerReset();                        // BT_POWER 断电再上电,状态清零
    // 断电重启后再设模式 1。从模式 2 且 A2DP 链路还活着的状态直接发 AT+MODE=1,
    // 模块不放手,声音还在蓝牙音箱上出(2026-09-07 实测)。断电是唯一确定能把链路
    // 掐掉的办法,开机就是这条路,所以结果和刚开机一样。
    void ResetToMode1();

    Mode mode() const;
    Conn conn() const;
    std::string connected_name() const;       // 连上的设备名,没名字给地址
    std::vector<Device> devices() const;      // 最近一次扫描结果
    // 声音现在是不是走蓝牙外设:模式 2 且已连接。
    bool OutputIsBluetooth() const;

    int  AddListener(Listener l);
    void RemoveListener(int id);

private:
    BtAudio() = default;
    void OnUartData(const std::vector<uint8_t>& data);
    void HandleLine(const std::string& raw);
    void Emit(Event ev, const std::string& line);
    void SendSeq(const char* first, const char* second, int gap_ms);

    mutable std::mutex mu_;
    Mode mode_ = Mode::kNone;
    Conn conn_ = Conn::kIdle;
    std::string pending_addr_;
    std::string connected_name_;
    std::vector<Device> devices_;
    std::string rx_;
    std::vector<std::pair<int, Listener>> listeners_;
    int next_id_ = 1;
    bool inited_ = false;
};
