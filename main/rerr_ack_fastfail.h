#ifndef RERR_ACK_FASTFAIL_H
#define RERR_ACK_FASTFAIL_H

#include <stddef.h>
#include <stdint.h>
#include "reliability.h"

typedef void (*rerr_ack_fail_notify_fn)(uint32_t packet_id, const char* reason);

size_t rerr_ack_failfast_for_dest(pending_ack_table_t* table, uint32_t broken_dest,
                                  const char* reason, rerr_ack_fail_notify_fn notify);

#endif /* RERR_ACK_FASTFAIL_H */
