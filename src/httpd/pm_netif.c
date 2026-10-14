#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#include "pm_netif.h"

#define RXQ_LEN      6
#define FRAME_MAX    1536

#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806

static struct netif pm_nif;
static bool netif_ready = false;
static uint32_t last_poll_us = 0;
static uint8_t nif_mac[6];
static uint32_t my_ip;              // Own IP in network byte order

// Received frames, written by the CYW43 callback and read by the main loop
static struct { uint16_t len; uint8_t data[FRAME_MAX]; } rxq[RXQ_LEN];
static volatile uint8_t rxq_head = 0;
static volatile uint8_t rxq_tail = 0;

// Time source in ms for lwIP
u32_t sys_now(void)
{
  return to_ms_since_boot(get_absolute_time());
}

// Protect functions needed by lwIP with NO_SYS
sys_prot_t sys_arch_protect(void)
{
  return 0;   // lwIP is only used from the main loop
}

void sys_arch_unprotect(sys_prot_t pval)
{
  (void)pval;
}

static err_t pm_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
  static uint8_t txbuf[FRAME_MAX];
  const uint8_t *data;

  if (p->tot_len > FRAME_MAX) return ERR_BUF;
  if (p->next == NULL) data = (const uint8_t *)p->payload;
  else {
        pbuf_copy_partial(p, txbuf, p->tot_len, 0);
        data = txbuf;
       }

  cyw43_arch_lwip_begin();
  int r = cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, p->tot_len, data, false);
  cyw43_arch_lwip_end();
  return (r == 0) ? ERR_OK : ERR_IF;
}

static err_t pm_netif_setup(struct netif *netif)
{
  netif->name[0] = 'p';
  netif->name[1] = 'm';
  netif->output = etharp_output;
  netif->linkoutput = pm_netif_linkoutput;
  memcpy(netif->hwaddr, nif_mac, 6);
  netif->hwaddr_len = 6;
  netif->mtu = 1500;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

bool pm_netif_init(const uint8_t mac[6], const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw)
{
  lwip_init();

  memcpy(nif_mac, mac, 6);
  if (netif_add(&pm_nif, ip, mask, gw, NULL, pm_netif_setup, ethernet_input) == NULL) return false;

  netif_set_default(&pm_nif);
  netif_set_up(&pm_nif);
  my_ip = ip4_addr_get_u32(ip);
  __dmb();
  netif_ready = true;
  return true;
}

// Runs in the CYW43 async context. Must stay short.
bool pm_netif_rx(const uint8_t *buf, size_t len)
{
  if (!netif_ready || len < 42 || len > FRAME_MAX) return false;

  uint16_t type = ((uint16_t)buf[12] << 8) | buf[13];
  if (type == ETH_TYPE_ARP) {
      if (memcmp(buf + 38, &my_ip, 4) != 0) return false;   // ARP target IP
     }
  else if (type == ETH_TYPE_IP) {
      if (memcmp(buf + 30, &my_ip, 4) != 0) return false;   // IPv4 destination
     }
  else return false;

  uint8_t head = rxq_head;
  uint8_t next = (head + 1) % RXQ_LEN;
  if (next == rxq_tail) return true;   // Drops the frame when the queue is full. TCP retransmits it.

  memcpy(rxq[head].data, buf, len);
  rxq[head].len = len;
  __dmb();
  rxq_head = next;
  return true;
}

bool pm_netif_link_is_up(void)
{
  return netif_ready && netif_is_link_up(&pm_nif);
}

const ip4_addr_t *pm_netif_ip(void)
{
  return netif_ip4_addr(&pm_nif);
}

void pm_netif_poll(void)
{
  if (!netif_ready) return;

  bool pending = (rxq_tail != rxq_head);
  uint32_t now = time_us_32();
  if (!pending && (now - last_poll_us) < 50000) return;   // At most every 50ms without received frames
  last_poll_us = now;

  // Follows the Wifi link state
  bool up = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) >= CYW43_LINK_JOIN;
  if (up && !netif_is_link_up(&pm_nif)) netif_set_link_up(&pm_nif);
  else if (!up && netif_is_link_up(&pm_nif)) netif_set_link_down(&pm_nif);

  while (rxq_tail != rxq_head) {
      uint8_t tail = rxq_tail;
      struct pbuf *p = pbuf_alloc(PBUF_RAW, rxq[tail].len, PBUF_POOL);
      if (p != NULL) {
          pbuf_take(p, rxq[tail].data, rxq[tail].len);
          if (pm_nif.input(p, &pm_nif) != ERR_OK) pbuf_free(p);
         }
      __dmb();
      rxq_tail = (tail + 1) % RXQ_LEN;
     }

  sys_check_timeouts();
}
