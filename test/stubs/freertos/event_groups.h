#ifndef EVENT_GROUPS_H_STUB
#define EVENT_GROUPS_H_STUB

#include <stdint.h>

typedef uint32_t EventBits_t;
typedef uint32_t TickType_t;
typedef struct event_group_stub* EventGroupHandle_t;

#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define pdFALSE 0

#define pdMS_TO_TICKS(ms) (ms)

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToWaitFor,
                                const int xClearOnExit, const int xWaitForAllBits,
                                TickType_t xTicksToWait);
EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, const EventBits_t uxBitsToSet);
void vEventGroupDelete(EventGroupHandle_t xEventGroup);

#endif
