#include "include/radio_mock.h"
#include <string.h>

static bool mock_radio_valid_node(const mock_radio_t* radio, int idx) {
    return idx >= 0 && idx < radio->num_nodes && idx < MOCK_MAX_NODES;
}

void mock_radio_init(mock_radio_t* radio, int num_nodes) {
    memset(radio, 0, sizeof(*radio));
    if (num_nodes < 0) {
        num_nodes = 0;
    }
    radio->num_nodes = (num_nodes > MOCK_MAX_NODES) ? MOCK_MAX_NODES : num_nodes;
}

void mock_radio_connect(mock_radio_t* radio, int a, int b, int8_t rssi, int8_t snr) {
    if (!mock_radio_valid_node(radio, a) || !mock_radio_valid_node(radio, b))
        return;
    radio->connected[a][b] = true;
    radio->connected[b][a] = true;
    radio->rssi_matrix[a][b] = rssi;
    radio->rssi_matrix[b][a] = rssi;
    radio->snr_matrix[a][b] = snr;
    radio->snr_matrix[b][a] = snr;
}

void mock_radio_send(mock_radio_t* radio, int from_node, const uint8_t* data, size_t len) {
    if (!mock_radio_valid_node(radio, from_node))
        return;
    if (len > MOCK_MAX_PACKET_SIZE)
        len = MOCK_MAX_PACKET_SIZE;

    for (int i = 0; i < radio->num_nodes; i++) {
        if (i == from_node)
            continue;
        if (!radio->connected[from_node][i])
            continue;

        mock_rx_queue_t* q = &radio->rx_queues[i];
        if (q->count >= MOCK_MAX_QUEUED)
            continue;

        mock_packet_t* pkt = &q->queue[q->tail];
        memcpy(pkt->data, data, len);
        pkt->len = len;
        pkt->from_node = from_node;
        pkt->rssi = radio->rssi_matrix[from_node][i];
        pkt->snr = radio->snr_matrix[from_node][i];

        q->tail = (q->tail + 1) % MOCK_MAX_QUEUED;
        q->count++;
    }
}

bool mock_radio_has_packet(mock_radio_t* radio, int node) {
    if (!mock_radio_valid_node(radio, node))
        return false;
    return radio->rx_queues[node].count > 0;
}

bool mock_radio_recv(mock_radio_t* radio, int node, mock_packet_t* pkt) {
    if (!mock_radio_valid_node(radio, node))
        return false;
    mock_rx_queue_t* q = &radio->rx_queues[node];
    if (q->count <= 0)
        return false;

    *pkt = q->queue[q->head];
    q->head = (q->head + 1) % MOCK_MAX_QUEUED;
    q->count--;
    return true;
}
