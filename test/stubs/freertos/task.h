#ifndef FREERTOS_TASK_H_STUB
#define FREERTOS_TASK_H_STUB
typedef void* TaskHandle_t;
static inline void vTaskDelay(int ticks){(void)ticks;}
static inline int xTaskCreate(void (*task)(void*), const char* name, int stack, void* arg, int pri, TaskHandle_t* out){(void)task;(void)name;(void)stack;(void)arg;(void)pri;(void)out;return 1;}
static inline void vTaskDelete(TaskHandle_t t){(void)t;}
#endif
