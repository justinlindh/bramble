#ifndef FREERTOS_QUEUE_H_STUB
#define FREERTOS_QUEUE_H_STUB

typedef void* QueueHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0

QueueHandle_t xQueueCreate(unsigned int queue_length, unsigned int item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, unsigned int ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* buffer, unsigned int ticks_to_wait);

#endif
