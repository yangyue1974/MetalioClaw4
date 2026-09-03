#include "audiobar_screen.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "application.h"
#include "audio/audio_codec.h"
#include "audio/music/music_player.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"

#if __has_include("audio/music/audiolib_secret.h")
#include "audio/music/audiolib_secret.h"
#else
// 没有 key 也要能编:界面照常,点曲库时提示 NO API KEY。
#define AUDIOLIB_API_KEY ""
#endif

namespace {

constexpr const char* TAG = "Audiobar";

// audiolib.ai palette
constexpr uint32_t kBg     = 0x0A0A0A;
constexpr uint32_t kHair   = 0x1F1F1F;
constexpr uint32_t kText   = 0xF5F5F5;
constexpr uint32_t kDim    = 0x4B4B4B;
constexpr uint32_t kMuted  = 0x9CA3AF;
constexpr uint32_t kAccent = 0xF97316;
constexpr uint32_t kRowOn  = 0x141110;
constexpr uint32_t kBtnBg  = 0x121212;

constexpr int kPanel   = 720;
constexpr int kHeaderH = 66;
constexpr int kRingY   = 84;
constexpr int kRingD   = 198;
constexpr int kBtnD    = 144;
constexpr int kColX    = 276;   // 右侧文字列
constexpr int kRuleY   = 330;
constexpr int kRowH    = 96;

struct Lib { const char* name; const char* id; };
const Lib kLibs[] = {
    {"AMBIENT",    "audio.ambient"},        {"BACKGROUND", "audio.background"},
    {"CHINESE POP","audio.chinese-pop"},    {"CINEMATIC",  "audio.cinematic"},
    {"CLASSICAL",  "audio.classical"},      {"DEFAULT",    "audio.default"},
    {"DISCO",      "audio.disco"},          {"DUBSTEP",    "audio.dubstep"},
    {"ELECTRONIC", "audio.electronic"},     {"EMOTIONAL",  "audio.emotional"},
    {"ENERGY",     "audio.energy"},         {"FLOW",       "audio.flow"},
    {"FOCUS",      "audio.focus"},          {"HEALING",    "audio.healing"},
    {"HOUSE",      "audio.house"},          {"JAZZ",       "audio.jazz"},
    {"LO-FI",      "audio.lo-fi"},          {"BALLAD",     "audio.mandarin-ballad"},
    {"MEDITATION", "audio.meditation"},     {"NEW AGE",    "audio.new-age"},
    {"POP",        "audio.pop"},            {"RELAX",      "audio.relax"},
    {"ROCK",       "audio.rock"},           {"RUNNING",    "audio.running"},
    {"SLEEP",      "audio.sleep"},          {"STUDY",      "audio.study"},
    {"TECHNO",     "audio.techno"},         {"TRANCE",     "audio.trance"},
    {"WORKOUT",    "audio.workout"},        {"WORLD",      "audio.world-music"},
};
constexpr int kLibCount = sizeof(kLibs) / sizeof(kLibs[0]);

struct Ui {
    lv_obj_t* screen = nullptr;
    lv_obj_t* quota = nullptr;
    lv_obj_t* now = nullptr;
    lv_obj_t* arc = nullptr;
    lv_obj_t* play_icon = nullptr;
    lv_obj_t* cur_lib = nullptr;
    lv_obj_t* time = nullptr;
    lv_obj_t* vol_pct = nullptr;
    lv_obj_t* rows[kLibCount] = {};
    lv_obj_t* index[kLibCount] = {};
    lv_obj_t* names[kLibCount] = {};
    lv_timer_t* progress_timer = nullptr;
    int active = -1;
};
Ui s_ui;
// 屏幕每建一次加一;lv_async_call 的回调拿它判断屏幕还是不是当初那一个。
std::atomic<uint32_t> s_gen{0};

MusicPlayer* s_player = nullptr;

// 进度由音频任务写、LVGL 定时器读。绝不让音频任务碰 LVGL。
std::atomic<int> s_prog_elapsed{0};
std::atomic<int> s_prog_total{0};

bool Alive() { return s_ui.screen != nullptr; }

lv_color_t C(uint32_t v) { return lv_color_hex(v); }

void SetRowState(int i, bool on) {
    if (i < 0 || i >= kLibCount || !Alive()) return;
    lv_obj_set_style_text_color(s_ui.index[i], C(on ? kAccent : kDim), 0);
    lv_obj_set_style_text_color(s_ui.names[i], C(on ? kAccent : kText), 0);
    lv_obj_set_style_bg_opa(s_ui.rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void SetPlayIcon(bool playing, bool active) {
    if (!s_ui.play_icon) return;
    lv_label_set_text(s_ui.play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_ui.play_icon, C(active ? kAccent : kDim), 0);
}

// ---- 从音频任务来的通知,切到 LVGL 线程 ----
struct TrackMsg {
    uint32_t gen;
    char lib[48];
    char title[96];
    int quota_remaining;
    int quota_total;
};
void ApplyTrack(void* p) {
    auto* m = static_cast<TrackMsg*>(p);
    if (m->gen == s_gen.load() && Alive()) {
        lv_label_set_text_fmt(s_ui.quota, "%d/%d", m->quota_remaining, m->quota_total);
        lv_obj_set_style_text_color(s_ui.quota, C(kMuted), 0);
        lv_label_set_text(s_ui.now, m->title);
        lv_obj_set_style_text_color(s_ui.now, C(kText), 0);
        SetPlayIcon(true, true);
    }
    delete m;
}
struct ErrMsg { uint32_t gen; char msg[64]; };
void ApplyError(void* p) {
    auto* m = static_cast<ErrMsg*>(p);
    if (m->gen == s_gen.load() && Alive()) {
        lv_label_set_text(s_ui.now, m->msg);
        lv_obj_set_style_text_color(s_ui.now, C(kAccent), 0);
        lv_obj_set_style_text_color(s_ui.cur_lib, C(kDim), 0);
        if (s_ui.active >= 0) { SetRowState(s_ui.active, false); s_ui.active = -1; }
        SetPlayIcon(false, false);
    }
    delete m;
}

class Listener : public MusicPlayer::Listener {
public:
    void OnTrack(const char* lib, const char* title, int rem, int tot) override {
        auto* m = new TrackMsg{};
        m->gen = s_gen.load();
        snprintf(m->lib, sizeof(m->lib), "%s", lib ? lib : "");
        snprintf(m->title, sizeof(m->title), "%s", title ? title : "");
        m->quota_remaining = rem;
        m->quota_total = tot;
        lv_async_call(ApplyTrack, m);
    }
    void OnProgress(int el, int tot) override {
        s_prog_elapsed.store(el);
        s_prog_total.store(tot);
    }
    void OnError(const char* msg) override {
        auto* m = new ErrMsg{};
        m->gen = s_gen.load();
        snprintf(m->msg, sizeof(m->msg), "%s", msg ? msg : "ERROR");
        lv_async_call(ApplyError, m);
    }
};
Listener s_listener;

// ---- 事件 ----
void OnProgressTimer(lv_timer_t*) {
    if (!Alive()) return;
    int el = s_prog_elapsed.load(), tot = s_prog_total.load();
    if (tot <= 0) return;
    int v = (int)((int64_t)el * 1000 / tot);
    lv_arc_set_value(s_ui.arc, v > 1000 ? 1000 : v);
    lv_label_set_text_fmt(s_ui.time, "%d:%02d / %d:%02d", el / 60, el % 60, tot / 60, tot % 60);
}

void OnVolume(lv_event_t* e) {
    auto* sl = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int v = (int)lv_slider_get_value(sl);
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(v);
    if (s_ui.vol_pct) lv_label_set_text_fmt(s_ui.vol_pct, "%d", v);
}

void OnPlayPause(lv_event_t*) {
    if (s_player == nullptr || !s_player->is_playing()) return;
    s_player->TogglePause();
    SetPlayIcon(!s_player->is_paused(), true);
}

void OnRow(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kLibCount || s_player == nullptr) return;
    if (!s_player->has_api_key()) {
        lv_label_set_text(s_ui.now, "NO API KEY  (audiolib_secret.h)");
        lv_obj_set_style_text_color(s_ui.now, C(kAccent), 0);
        return;
    }
    if (s_ui.active >= 0) SetRowState(s_ui.active, false);
    s_ui.active = i;
    SetRowState(i, true);
    lv_label_set_text(s_ui.now, "CONNECTING");
    lv_obj_set_style_text_color(s_ui.now, C(kMuted), 0);
    lv_label_set_text(s_ui.cur_lib, kLibs[i].name);
    lv_obj_set_style_text_color(s_ui.cur_lib, C(kAccent), 0);
    lv_arc_set_value(s_ui.arc, 0);
    lv_label_set_text(s_ui.time, "0:00 / 0:00");
    s_prog_elapsed.store(0);
    s_prog_total.store(0);
    ESP_LOGI(TAG, "tap %s", kLibs[i].id);
    s_player->PlayLibrary(kLibs[i].id);
}

void GoHome() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    lv_obj_t* old = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old != nullptr && old != home) lv_obj_delete_async(old);
}

void OnDelete(lv_event_t*) {
    if (s_ui.progress_timer) { lv_timer_delete(s_ui.progress_timer); }
    s_ui = Ui{};
    s_gen.fetch_add(1);
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int ls = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    if (ls) lv_obj_set_style_text_letter_space(l, ls, 0);
    return l;
}

// ---- 进出 app 时的系统音频切换,放 worker 里,不堵 LVGL 线程 ----
void EnterTask(void*) {
    Application::GetInstance().StopSystemAudioForStressTest();
    if (s_player) s_player->Start();
    vTaskDelete(nullptr);
}
void LeaveTask(void*) {
    if (s_player) s_player->Shutdown();
    Application::GetInstance().RestoreSystemAudioAfterStressTest();
    vTaskDelete(nullptr);
}

}  // namespace

lv_obj_t* AudiobarScreen::Create() {
    s_gen.fetch_add(1);
    s_ui = Ui{};
    s_prog_elapsed.store(0);
    s_prog_total.store(0);

    if (s_player == nullptr) {
        s_player = new MusicPlayer(Board::GetInstance().GetAudioCodec(), AUDIOLIB_API_KEY);
        s_player->SetListener(&s_listener);
    }

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    lv_obj_set_style_bg_color(scr, C(kBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDelete, LV_EVENT_DELETE, nullptr);
    screen_attach_swipe_back(scr, GoHome);

    // ---- 抬头 ----
    lv_obj_t* back = lv_button_create(scr);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_set_pos(back, 10, (kHeaderH - 72) / 2 + 6);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back, C(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) { GoHome(); }, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* brand = Label(scr, "AUDIOLIB", &lv_font_montserrat_20, kText, 8);
    lv_obj_set_pos(brand, 96, 24);
    lv_obj_t* tld = Label(scr, ".AI", &lv_font_montserrat_20, kAccent, 8);
    lv_obj_align_to(tld, brand, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    s_ui.quota = Label(scr, "--/--", &lv_font_montserrat_16, kDim, 2);
    lv_obj_align(s_ui.quota, LV_ALIGN_TOP_RIGHT, -40, 26);

    // ---- 仪表区 ----
    s_ui.arc = lv_arc_create(scr);
    lv_obj_set_size(s_ui.arc, kRingD, kRingD);
    lv_obj_set_pos(s_ui.arc, 42, kRingY);
    lv_arc_set_rotation(s_ui.arc, 270);
    lv_arc_set_bg_angles(s_ui.arc, 0, 360);
    lv_arc_set_range(s_ui.arc, 0, 1000);
    lv_arc_set_value(s_ui.arc, 0);
    lv_obj_remove_style(s_ui.arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(s_ui.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_ui.arc, C(kHair), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ui.arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ui.arc, C(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_ui.arc, 4, LV_PART_INDICATOR);

    lv_obj_t* btn = lv_obj_create(scr);
    lv_obj_set_size(btn, kBtnD, kBtnD);
    lv_obj_set_pos(btn, 42 + (kRingD - kBtnD) / 2, kRingY + (kRingD - kBtnD) / 2);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, C(kBtnBg), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, OnPlayPause, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);
    s_ui.play_icon = Label(btn, LV_SYMBOL_PLAY, &lv_font_montserrat_48, kDim);
    lv_obj_center(s_ui.play_icon);

    s_ui.cur_lib = Label(scr, "NO LIBRARY", &lv_font_montserrat_28, kDim, 3);
    lv_label_set_long_mode(s_ui.cur_lib, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.cur_lib, kPanel - kColX - 40);
    lv_obj_set_pos(s_ui.cur_lib, kColX, 104);

    s_ui.now = Label(scr, "SELECT A LIBRARY BELOW", &lv_font_montserrat_16, kMuted);
    lv_label_set_long_mode(s_ui.now, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.now, kPanel - kColX - 40);
    lv_obj_set_pos(s_ui.now, kColX, 156);

    s_ui.time = Label(scr, "0:00 / 0:00", &lv_font_montserrat_16, kDim, 2);
    lv_obj_set_pos(s_ui.time, kColX, 196);

    lv_obj_t* vol_tag = Label(scr, "VOL", &lv_font_montserrat_16, kDim, 2);
    lv_obj_set_pos(vol_tag, kColX, 246);

    int vol = Board::GetInstance().GetAudioCodec()->output_volume();
    lv_obj_t* slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 300, 4);            // 真·细轨:对象本身就是 4px
    lv_obj_set_pos(slider, kColX + 62, 254);
    lv_obj_set_ext_click_area(slider, 24);      // 触摸区单独放大,不影响观感
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, C(kHair), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, C(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, OnVolume, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(slider, true);

    char volbuf[8];
    snprintf(volbuf, sizeof(volbuf), "%d", vol);
    s_ui.vol_pct = Label(scr, volbuf, &lv_font_montserrat_16, kMuted);
    lv_obj_set_pos(s_ui.vol_pct, kColX + 62 + 300 + 16, 246);

    lv_obj_t* rule = lv_obj_create(scr);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, kPanel - 84, 1);
    lv_obj_set_pos(rule, 42, kRuleY);
    lv_obj_set_style_bg_color(rule, C(kHair), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    // ---- 列表 ----
    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, kPanel, kPanel - kRuleY - 2);
    lv_obj_set_pos(list, 0, kRuleY + 2);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < kLibCount; i++) {
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, kPanel, kRowH);
        lv_obj_set_style_bg_color(row, C(kRowOn), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, C(kHair), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnRow, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_ui.rows[i] = row;

        char num[4];
        snprintf(num, sizeof(num), "%02d", i + 1);
        lv_obj_t* idx = Label(row, num, &lv_font_montserrat_16, kDim);
        lv_obj_align(idx, LV_ALIGN_LEFT_MID, 42, 0);
        s_ui.index[i] = idx;

        lv_obj_t* name = Label(row, kLibs[i].name, &lv_font_montserrat_28, kText, 3);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 114, -13);
        s_ui.names[i] = name;

        lv_obj_t* id = Label(row, kLibs[i].id, &lv_font_montserrat_16, kDim);
        lv_obj_align(id, LV_ALIGN_LEFT_MID, 114, 21);
    }

    s_ui.progress_timer = lv_timer_create(OnProgressTimer, 500, nullptr);
    ESP_LOGI(TAG, "ui ready, %d libraries, api key %s", kLibCount,
             s_player->has_api_key() ? "present" : "MISSING");
    return scr;
}

void AudiobarScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        xTaskCreate(EnterTask, "audiobar_on", 4096, nullptr, 5, nullptr);
    } else {
        xTaskCreate(LeaveTask, "audiobar_off", 4096, nullptr, 5, nullptr);
    }
}
