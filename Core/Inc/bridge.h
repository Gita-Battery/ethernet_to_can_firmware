#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

typedef struct {
    uint32_t task_iterations, last_progress_ms, stack_free_bytes;
    uint32_t parser_rejections, can_queue_overflow, can_rx_errors, can_rx_ignored;
    uint32_t can_tx_ok, can_tx_busy, can_tx_errors, can_rx_forwarded;
    uint32_t can_bus_off_entries, can_bus_off_exits, can_error_transitions, can_esr;
    uint32_t spi_errors, ethernet_timeouts, socket_errors, socket_reopens;
    uint32_t ethernet_resets, ethernet_init_failures, link_down_events;
    uint32_t udp_send_drops, can_offline_drops;
} BridgeDiagnostics;

extern volatile BridgeDiagnostics bridge_diagnostics;
void Bridge_Init(void);
void Bridge_Step(void);

#endif
