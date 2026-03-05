#ifndef BRAMBLE_TX_QUEUE_H
#define BRAMBLE_TX_QUEUE_H
#include <stdint.h>
#include <stdbool.h>

#define TX_QUEUE_SIZE 16

typedef struct {
    uint8_t data[256];
    uint16_t len;
    uint8_t priority;
    uint32_t enqueue_time;
    bool active;
} tx_entry_t;

typedef struct {
    tx_entry_t entries[TX_QUEUE_SIZE];
    int count;
} tx_queue_t;

void tx_queue_init(tx_queue_t* q);
int tx_queue_enqueue(tx_queue_t* q, const uint8_t* data, uint16_t len, uint8_t priority,
                     uint32_t now_ms);
bool tx_queue_dequeue(tx_queue_t* q, uint8_t* data, uint16_t* len);
int tx_queue_count(const tx_queue_t* q);
bool tx_queue_is_full(const tx_queue_t* q);
#endif
