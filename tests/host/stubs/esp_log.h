#pragma once
#include <cstdio>
#define ESP_LOGI(t, f, ...) printf(f "\n", ##__VA_ARGS__)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
