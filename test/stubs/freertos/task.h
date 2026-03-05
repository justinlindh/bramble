#ifndef FREERTOS_TASK_H_STUB
#define FREERTOS_TASK_H_STUB

typedef void* TaskHandle_t;
typedef int BaseType_t;

#ifndef FREERTOS_TASK_CUSTOM_IMPL
static inline void vTaskDelay(int ticks) { (void)ticks; }
static inline int xTaskCreate(void (*task)(void*), const char* name, int stack, void* arg, int pri,
                              TaskHandle_t* out) {
    (void)task;
    (void)name;
    (void)stack;
    (void)arg;
    (void)pri;
    (void)out;
    return 1;
}
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
#else
void vTaskDelay(int ticks);
int xTaskCreate(void (*task)(void*), const char* name, int stack, void* arg, int pri,
                TaskHandle_t* out);
void vTaskDelete(TaskHandle_t t);
#endif

#endif
