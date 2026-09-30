#ifndef ETH_IO_H
#define ETH_IO_H

#include <stdbool.h>
#include <stdint.h>

#define ETH_COMMAND_TIMEOUT_MS 100U
#define ETH_SEND_TIMEOUT_MS 2000U

/* Platform hooks for the local W5500 driver; all calls belong to the bridge task. */
uint32_t EthIo_Now(void);
bool EthIo_Failed(void);
void EthIo_LatchFault(void);
/* Yield while polling. False means transport failure or an expired deadline. */
bool EthIo_Wait(uint32_t started, uint32_t timeout_ms);

#endif
