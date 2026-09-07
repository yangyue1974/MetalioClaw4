// BtAudio 的回显解析和状态机,主机上跑(stubs/ 里是 UART / IO 扩展 / FreeRTOS 的桩)。
// 回显行是按 README §12.1 和实机日志写的;模块换固件、回显变了,先改这里的样本。
#include "bt_audio.h"
#include "SimpleUart.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static void feed(const char* s) {
    std::string x(s);
    std::vector<uint8_t> v(x.begin(), x.end());
    SimpleUart::getInstance().cb(v);
}

int main() {
    auto& bt = BtAudio::GetInstance();
    bt.Init();
    int events = 0;
    bt.AddListener([&](BtAudio::Event, const std::string&) { events++; });

    // 乐观置模式,回显确认
    bt.SetMode(BtAudio::Mode::kMode2);
    assert(bt.mode() == BtAudio::Mode::kMode2);
    feed("SET MODE 2\r\n");
    assert(bt.mode() == BtAudio::Mode::kMode2 && bt.conn() == BtAudio::Conn::kIdle);

    // 扫描:设备行、拆包、去重、无名设备
    feed("INQUIRING START\r\n");
    assert(bt.conn() == BtAudio::Conn::kScanning);
    feed("AT+BT:AABBCCDDEEFFJBL Flip 6\r\n");
    feed("AT+BT:AABBCCDDEEFFJBL Flip 6\r\n");
    feed("AT+BT:1122334455");
    feed("66\r\nINQ COMP");
    feed("LETE\r\n");
    auto devs = bt.devices();
    assert(devs.size() == 2);
    assert(devs[0].addr == "AABBCCDDEEFF" && devs[0].name == "JBL Flip 6");
    assert(devs[1].addr == "112233445566" && devs[1].name.empty());
    assert(bt.conn() == BtAudio::Conn::kIdle);

    // 连接:名字从扫描结果里找
    bt.Connect("AABBCCDDEEFF");
    assert(bt.conn() == BtAudio::Conn::kConnecting);
    feed("CONNECTING\r\nCONNECT SUCCESS\r\n");
    assert(bt.OutputIsBluetooth());
    assert(bt.connected_name() == "JBL Flip 6");

    // SCO 断开不是链路断开
    feed("DISC SCO\r\n");
    assert(bt.OutputIsBluetooth());
    feed("DISCONNECT\r\n");
    assert(!bt.OutputIsBluetooth() && bt.conn() == BtAudio::Conn::kIdle);

    // 超时
    bt.Connect("112233445566");
    feed("CONNECT TIMEOUT\r\n");
    assert(bt.conn() == BtAudio::Conn::kIdle);

    // 模块自己回连,名字未知
    feed("RECONNECT\r\n");
    assert(bt.conn() == BtAudio::Conn::kConnecting);
    feed("CONNECT SUCCESS\r\n");
    assert(bt.OutputIsBluetooth() && !bt.connected_name().empty());

    // 切回模式 1 立刻不算蓝牙输出
    bt.SetMode(BtAudio::Mode::kMode1);
    assert(!bt.OutputIsBluetooth() && bt.mode() == BtAudio::Mode::kMode1);

    // 垃圾行不崩,超长不崩
    feed("\r\n\r\n   \r\n");
    feed(std::string(5000, 'x').c_str());
    feed("\r\nSET MODE 3\r\n");
    assert(bt.mode() == BtAudio::Mode::kMode3);

    assert(events > 10);
    printf("bt_audio_test: ALL PASS (%d events)\n", events);
    return 0;
}
