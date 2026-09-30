#ifndef BRIDGE_PROTOCOL_H
#define BRIDGE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_PACKET_MAX 200U
#define BRIDGE_CAN_QUEUE_SIZE 32U

typedef struct {
    uint16_t id;
    uint8_t dlc;
    uint8_t data[8];
} BridgeFrame;

/* No NUL terminator required. Output is unchanged on rejection. */
bool Bridge_Parse(const uint8_t *packet, size_t length, BridgeFrame *frame);
/* Preserve the legacy eight-byte response, padding short CAN frames with zero. */
size_t Bridge_Format(const BridgeFrame *frame, char *packet, size_t capacity);

#endif
