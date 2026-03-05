#ifndef BRAMBLE_RADIO_MOCK_H
#define BRAMBLE_RADIO_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MOCK_MAX_NODES 8
#define MOCK_MAX_QUEUED 32
#define MOCK_MAX_PACKET_SIZE 222

typedef struct {
    uint8_t data[MOCK_MAX_PACKET_SIZE];
    size_t len;
    int from_node;
    int8_t rssi;
    int8_t snr;
} mock_packet_t;

typedef struct {
    mock_packet_t queue[MOCK_MAX_QUEUED];
    int head, tail, count;
} mock_rx_queue_t;

typedef struct {
    int num_nodes;
    mock_rx_queue_t rx_queues[MOCK_MAX_NODES];
    bool connected[MOCK_MAX_NODES][MOCK_MAX_NODES];
    int8_t rssi_matrix[MOCK_MAX_NODES][MOCK_MAX_NODES];
    int8_t snr_matrix[MOCK_MAX_NODES][MOCK_MAX_NODES];
} mock_radio_t;

void mock_radio_init(mock_radio_t* radio, int num_nodes);
void mock_radio_connect(mock_radio_t* radio, int a, int b, int8_t rssi, int8_t snr);
void mock_radio_send(mock_radio_t* radio, int from_node, const uint8_t* data, size_t len);
bool mock_radio_has_packet(mock_radio_t* radio, int node);
bool mock_radio_recv(mock_radio_t* radio, int node, mock_packet_t* pkt);

#endif
