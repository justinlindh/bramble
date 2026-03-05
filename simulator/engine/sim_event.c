#include "sim_event.h"
#include <string.h>

static void heap_bubble_up(event_queue_t* q, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (q->events[idx].timestamp_us >= q->events[parent].timestamp_us)
            break;
        sim_event_t tmp = q->events[idx];
        q->events[idx] = q->events[parent];
        q->events[parent] = tmp;
        idx = parent;
    }
}

static void heap_bubble_down(event_queue_t* q, int idx) {
    int size = q->count;
    while (true) {
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        int smallest = idx;

        if (left < size && q->events[left].timestamp_us < q->events[smallest].timestamp_us)
            smallest = left;
        if (right < size && q->events[right].timestamp_us < q->events[smallest].timestamp_us)
            smallest = right;

        if (smallest == idx)
            break;

        sim_event_t tmp = q->events[idx];
        q->events[idx] = q->events[smallest];
        q->events[smallest] = tmp;
        idx = smallest;
    }
}

void event_queue_init(event_queue_t* queue) { memset(queue, 0, sizeof(*queue)); }

bool event_queue_push(event_queue_t* queue, const sim_event_t* event) {
    if (queue->count >= MAX_EVENT_QUEUE)
        return false;
    queue->events[queue->count] = *event;
    heap_bubble_up(queue, queue->count);
    queue->count++;
    return true;
}

bool event_queue_pop(event_queue_t* queue, sim_event_t* out) {
    if (queue->count == 0)
        return false;
    *out = queue->events[0];
    queue->count--;
    if (queue->count > 0) {
        queue->events[0] = queue->events[queue->count];
        heap_bubble_down(queue, 0);
    }
    return true;
}

sim_event_t* event_queue_peek(event_queue_t* queue) {
    return (queue->count > 0) ? &queue->events[0] : NULL;
}

int event_queue_count(const event_queue_t* queue) { return queue->count; }
