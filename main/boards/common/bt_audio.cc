#include "bt_audio.h"

#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "IOExpander.hpp"
#include "SimpleUart.hpp"

#define TAG "BtAudio"

namespace {

constexpr int kAddrLen = 12;

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void Trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    if (i) s.erase(0, i);
}

// "AT+BT:<12位地址><名称>"
bool ParseDeviceLine(const std::string& line, std::string* addr, std::string* name) {
    static const char kPrefix[] = "AT+BT:";
    if (line.rfind(kPrefix, 0) != 0) return false;
    std::string payload = line.substr(sizeof(kPrefix) - 1);
    if (payload.size() < (size_t)kAddrLen) return false;
    for (int i = 0; i < kAddrLen; i++) if (!IsHex(payload[i])) return false;
    *addr = payload.substr(0, kAddrLen);
    *name = payload.substr(kAddrLen);
    Trim(*name);
    return true;
}

struct SeqArgs {
    char first[32];
    char second[32];
    char first_log[32];    // 去掉 \r\n 的副本,只为日志
    char second_log[32];
    int gap_ms;
};

void StripCrlf(const char* in, char* out, size_t n) {
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++) out[i] = (in[i] == '\r' || in[i] == '\n') ? '\0' : in[i];
    out[i] = '\0';
}

void SeqTask(void* p) {
    auto* a = static_cast<SeqArgs*>(p);
    auto& uart = SimpleUart::getInstance();
    uart.sendString(a->first);
    ESP_LOGI(TAG, "TX: %s", a->first_log);
    if (a->second[0]) {
        vTaskDelay(pdMS_TO_TICKS(a->gap_ms));
        uart.sendString(a->second);
        ESP_LOGI(TAG, "TX: %s", a->second_log);
    }
    delete a;
    vTaskDelete(nullptr);
}

}  // namespace

BtAudio& BtAudio::GetInstance() {
    static BtAudio inst;
    return inst;
}

void BtAudio::Init() {
    if (inited_) return;
    inited_ = true;
    SimpleUart::getInstance().registerCallback(
        [this](const std::vector<uint8_t>& d) { OnUartData(d); });
    ESP_LOGI(TAG, "listening on BT UART");
}

void BtAudio::SendSeq(const char* first, const char* second, int gap_ms) {
    if (!SimpleUart::getInstance().isInitialized()) {
        ESP_LOGE(TAG, "UART not initialized, drop %s", first);
        return;
    }
    auto* a = new SeqArgs{};
    snprintf(a->first, sizeof(a->first), "%s", first);
    snprintf(a->second, sizeof(a->second), "%s", second ? second : "");
    StripCrlf(a->first, a->first_log, sizeof(a->first_log));
    StripCrlf(a->second, a->second_log, sizeof(a->second_log));
    a->gap_ms = gap_ms;
    xTaskCreate(SeqTask, "bt_at", 4096, a, 5, nullptr);
}

void BtAudio::SetMode(Mode mode) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 乐观记下:模块不一定回 "SET MODE n"(开机那次就常常收不到),UI 不能一直等。
        mode_ = mode;
        conn_ = Conn::kIdle;
        connected_name_.clear();
        pending_addr_.clear();
    }
    switch (mode) {
        case Mode::kMode1: SendSeq("AT+RX=2\r\n", "AT+MODE=1\r\n", 700); break;
        case Mode::kMode2: SendSeq("AT+TX=1\r\n", "AT+MODE=2\r\n", 700); break;
        case Mode::kMode3: SendSeq("AT+RX=1\r\n", "AT+MODE=3\r\n", 700); break;
        default: return;
    }
    Emit(Event::kMode, "");
}

void BtAudio::Scan() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        devices_.clear();
        conn_ = Conn::kScanning;
    }
    SendSeq("AT+INQUIRING\r\n", nullptr, 0);
    Emit(Event::kScanStart, "");
}

void BtAudio::Connect(const std::string& addr) {
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s\r\n", addr.c_str());
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_addr_ = addr;
        conn_ = Conn::kConnecting;
    }
    SendSeq(cmd, nullptr, 0);
    Emit(Event::kConnecting, "");
}

void BtAudio::MusicLink() { SendSeq("AT+BTSCO=0\r\n", "AT+PP=1\r\n", 200); }
void BtAudio::CallLink()  { SendSeq("AT+PP=1\r\n", "AT+BTSCO=1\r\n", 200); }

void BtAudio::PowerReset() {
    xTaskCreate([](void* p) {
        auto* self = static_cast<BtAudio*>(p);
        auto& io = IOExpander::getInstance();
        io.setLevel(IOExpander::Pin::BT_POWER, false);
        ESP_LOGI(TAG, "BT_POWER off");
        vTaskDelay(pdMS_TO_TICKS(300));
        io.setLevel(IOExpander::Pin::BT_POWER, true);
        ESP_LOGI(TAG, "BT_POWER on");
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->mode_ = Mode::kNone;
            self->conn_ = Conn::kIdle;
            self->connected_name_.clear();
            self->pending_addr_.clear();
            self->devices_.clear();
        }
        self->Emit(Event::kMode, "");
        vTaskDelete(nullptr);
    }, "bt_reset", 4096, this, 5, nullptr);
}

void BtAudio::ResetToMode1() {
    xTaskCreate([](void* p) {
        auto* self = static_cast<BtAudio*>(p);
        auto& io = IOExpander::getInstance();
        io.setLevel(IOExpander::Pin::BT_POWER, false);
        ESP_LOGI(TAG, "BT_POWER off (reset to mode 1)");
        vTaskDelay(pdMS_TO_TICKS(300));
        io.setLevel(IOExpander::Pin::BT_POWER, true);
        ESP_LOGI(TAG, "BT_POWER on");
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->mode_ = Mode::kNone;
            self->conn_ = Conn::kIdle;
            self->connected_name_.clear();
            self->pending_addr_.clear();
        }
        self->Emit(Event::kMode, "");
        // 开机时板级在上电后 ~1 秒就发模式 1 指令,模块能收。这里留 1.5 秒。
        vTaskDelay(pdMS_TO_TICKS(1500));
        self->SetMode(Mode::kMode1);
        vTaskDelete(nullptr);
    }, "bt_reset1", 4096, this, 5, nullptr);
}

BtAudio::Mode BtAudio::mode() const { std::lock_guard<std::mutex> lk(mu_); return mode_; }
BtAudio::Conn BtAudio::conn() const { std::lock_guard<std::mutex> lk(mu_); return conn_; }
std::string BtAudio::connected_name() const { std::lock_guard<std::mutex> lk(mu_); return connected_name_; }
std::vector<BtAudio::Device> BtAudio::devices() const { std::lock_guard<std::mutex> lk(mu_); return devices_; }
bool BtAudio::OutputIsBluetooth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return mode_ == Mode::kMode2 && conn_ == Conn::kConnected;
}

int BtAudio::AddListener(Listener l) {
    std::lock_guard<std::mutex> lk(mu_);
    int id = next_id_++;
    listeners_.emplace_back(id, std::move(l));
    return id;
}

void BtAudio::RemoveListener(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->first == id) { listeners_.erase(it); return; }
    }
}

void BtAudio::Emit(Event ev, const std::string& line) {
    std::vector<Listener> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : listeners_) copy.push_back(p.second);
    }
    for (auto& l : copy) l(ev, line);
}

void BtAudio::OnUartData(const std::vector<uint8_t>& data) {
    rx_.append(data.begin(), data.end());
    size_t pos = 0;
    while (true) {
        size_t nl = rx_.find('\n', pos);
        if (nl == std::string::npos) break;
        HandleLine(rx_.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (pos) rx_.erase(0, pos);
    if (rx_.size() > 2048) { ESP_LOGW(TAG, "rx overflow, clearing"); rx_.clear(); }
}

void BtAudio::HandleLine(const std::string& raw) {
    std::string line = raw;
    Trim(line);
    if (line.empty()) return;
    ESP_LOGI(TAG, "RX: %s", line.c_str());

    auto has = [&](const char* s) { return line.find(s) != std::string::npos; };

    if (has("SET MODE 1") || has("SET MODE 2") || has("SET MODE 3")) {
        Mode m = has("SET MODE 1") ? Mode::kMode1 : has("SET MODE 2") ? Mode::kMode2 : Mode::kMode3;
        {
            std::lock_guard<std::mutex> lk(mu_);
            mode_ = m;
            conn_ = Conn::kIdle;
            connected_name_.clear();
        }
        Emit(Event::kMode, line);
        return;
    }
    if (has("INQUIRING START")) {
        { std::lock_guard<std::mutex> lk(mu_); devices_.clear(); conn_ = Conn::kScanning; }
        Emit(Event::kScanStart, line);
        return;
    }
    std::string addr, name;
    if (ParseDeviceLine(line, &addr, &name)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            bool dup = false;
            for (auto& d : devices_) if (d.addr == addr) { dup = true; if (d.name.empty()) d.name = name; }
            if (!dup) devices_.push_back({addr, name});
        }
        Emit(Event::kDeviceFound, line);
        return;
    }
    if (has("INQ COMPLETE")) {
        { std::lock_guard<std::mutex> lk(mu_); if (conn_ == Conn::kScanning) conn_ = Conn::kIdle; }
        Emit(Event::kScanDone, line);
        return;
    }
    if (has("CONNECT SUCCESS")) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            conn_ = Conn::kConnected;
            connected_name_.clear();
            for (auto& d : devices_) if (d.addr == pending_addr_) connected_name_ = d.name;
            if (connected_name_.empty()) connected_name_ = pending_addr_.empty() ? "BLUETOOTH" : pending_addr_;
        }
        Emit(Event::kConnected, line);
        return;
    }
    if (has("CONNECT TIMEOUT") || has("CONNECT FAIL")) {
        { std::lock_guard<std::mutex> lk(mu_); conn_ = Conn::kIdle; }
        Emit(Event::kConnectFailed, line);
        return;
    }
    if (has("CONNECTING") || has("RECONNECT")) {
        // RECONNECT:模块自己回连上次的设备,名字不知道,等 CONNECT SUCCESS。
        { std::lock_guard<std::mutex> lk(mu_); conn_ = Conn::kConnecting; }
        Emit(Event::kConnecting, line);
        return;
    }
    if (has("DISCONNECT") || has("DISC LINK")) {
        bool was = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            was = conn_ == Conn::kConnected;
            conn_ = Conn::kIdle;
            connected_name_.clear();
        }
        if (was) Emit(Event::kDisconnected, line);
        else Emit(Event::kLine, line);
        return;
    }
    Emit(Event::kLine, line);
}
