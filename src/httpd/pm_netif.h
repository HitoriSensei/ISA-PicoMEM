#pragma once
// lwIP network interface on top of the CYW43 station interface.
// The CYW43 is also used raw by the NE2000 emulation, so only the frames
// addressed to the Pico's own IP are given to lwIP (see pm_netif_rx).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start lwIP and the netif (static IP). Returns false on failure.
bool pm_netif_init(const uint8_t mac[6], const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw);

// Called from the CYW43 receive callback: returns true if the frame is for lwIP
// (it is queued and must NOT be given to the NE2000)
bool pm_netif_rx(const uint8_t *buf, size_t len);

// Call from the main loop: process received frames and lwIP timers (rate limited when idle)
void pm_netif_poll(void);

bool pm_netif_link_is_up(void);
const ip4_addr_t *pm_netif_ip(void);

#ifdef __cplusplus
}
#endif
