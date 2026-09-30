#include "bridge_protocol.h"
#include <string.h>

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool whitespace(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(const uint8_t **p, size_t *n)
{
    while (*n && whitespace(**p)) { ++*p; --*n; }
    while (*n && whitespace((*p)[*n - 1])) --*n;
}

static bool number(const uint8_t *p, size_t n, unsigned base,
                   unsigned maximum, unsigned *value)
{
    unsigned result = 0;
    if (base == 16 && n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        n -= 2;
    }
    if (!n) return false;
    for (size_t i = 0; i < n; ++i) {
        int digit = hex_digit(p[i]);
        if (digit < 0 || (unsigned)digit >= base ||
            result > maximum / base || result * base + (unsigned)digit > maximum)
            return false;
        result = result * base + (unsigned)digit;
    }
    *value = result;
    return true;
}

bool Bridge_Parse(const uint8_t *packet, size_t length, BridgeFrame *frame)
{
    const uint8_t *fields[6];
    size_t lengths[6];
    BridgeFrame parsed = {0};
    unsigned id, dlc;
    if (!packet || !frame || length > BRIDGE_PACKET_MAX) return false;
    trim(&packet, &length);
    if (length < 2 || packet[0] != '{' || packet[length - 1] != '}') return false;
    ++packet;
    length -= 2;
    for (unsigned i = 0; i < 6; ++i) {
        size_t n = 0;
        while (n < length && packet[n] != ',') ++n;
        if ((i < 5 && n == length) || (i == 5 && n != length)) return false;
        fields[i] = packet;
        lengths[i] = n;
        trim(&fields[i], &lengths[i]);
        if (i < 5) { packet += n + 1; length -= n + 1; }
    }
    if (lengths[0] != 3 || memcmp(fields[0], "007", 3) ||
        lengths[1] != 3 || memcmp(fields[1], "232", 3) ||
        lengths[2] != 3 || memcmp(fields[2], "001", 3) ||
        !number(fields[3], lengths[3], 16, 0x7FF, &id) ||
        !number(fields[4], lengths[4], 10, 8, &dlc))
        return false;
    unsigned payload_bytes = dlc;
    if (lengths[5] != 2 * dlc) {
        /* Released analyzers declare DLC 5 for the four-byte enable command.
         * Preserve that DLC, with the unused fifth byte zeroed by parsed's
         * initializer. Do not accept other incomplete commands/settings. */
        if (dlc != 5 || lengths[5] != 8 ||
            memcmp(fields[5], "1445650", 7) || hex_digit(fields[5][7]) != 10)
            return false;
        payload_bytes = 4;
    }
    parsed.id = (uint16_t)id;
    parsed.dlc = (uint8_t)dlc;
    for (unsigned i = 0; i < payload_bytes; ++i) {
        int hi = hex_digit(fields[5][2 * i]);
        int lo = hex_digit(fields[5][2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        parsed.data[i] = (uint8_t)((hi << 4) | lo);
    }
    *frame = parsed;
    return true;
}

size_t Bridge_Format(const BridgeFrame *frame, char *packet, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    const char prefix[] = "{507,232,4,";
    size_t n = sizeof(prefix) - 1;
    /* Maximum is 41 bytes plus terminator. Avoid libc formatting on the task stack. */
    if (!frame || !packet || capacity < 42 || frame->id > 0x7FF || frame->dlc > 8)
        return 0;
    memcpy(packet, prefix, n);
    if (frame->id > 0xFF) packet[n++] = hex[(frame->id >> 8) & 15];
    packet[n++] = hex[(frame->id >> 4) & 15];
    packet[n++] = hex[frame->id & 15];
    packet[n++] = ',';
    packet[n++] = '8';
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t byte = i < frame->dlc ? frame->data[i] : 0;
        packet[n++] = ',';
        packet[n++] = hex[byte >> 4];
        packet[n++] = hex[byte & 15];
    }
    packet[n++] = '}';
    packet[n] = '\0';
    return n;
}
