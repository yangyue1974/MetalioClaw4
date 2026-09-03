#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// AudiobarScreen —— Audiolib API 的实体样板机,从 ESP32-S3-Touch-LCD-4B(480×480)
// 移植到 Metalio Claw4(720×720)。全英文,配色沿用 audiolib.ai。
//
// 布局(按 480 版 ×1.5):
//   抬头 66px    AUDIOLIB.AI + 右上配额
//   仪表区 252px 198px 圆环进度 + 中心 144px 播放键;右侧曲库名 / 曲名 / 时间 / 音量细轨
//   列表 400px   30 个曲库竖向滚动,当前行整行转橙
//
// 生命周期:
//   LOAD   -> 停系统语音链路(和厂商电台 App 同一套 Application 接口),建音乐任务
//   UNLOAD -> 销毁音乐任务(停播),恢复系统语音链路
// 进度由音频任务写原子量、LVGL 定时器读;曲名/配额/错误经 lv_async_call 切到 LVGL 线程。
// 音频线程绝不碰 LVGL,也不拿显示锁 —— S3 上这条是卡顿的第三层根因。
// ---------------------------------------------------------------------------
class AudiobarScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
