#pragma once
inline void vTaskDelay(int) {}
inline void vTaskDelete(void*) {}
inline int xTaskCreate(void (*f)(void*), const char*, int, void* a, int, void*) { f(a); return 1; }
