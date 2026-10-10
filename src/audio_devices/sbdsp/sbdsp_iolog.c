// I/O trace to the SD card (see sbdsp_iolog.h)
// Producers: core 1 (I/O handlers) and core 0 (audio timers), protected by a spin lock.
// Consumer: sbdsp_iolog_task() in the core 0 main loop. The SD card is only written after
// IOLOG_IDLE_US without new entry, when the buffer is half full, or IOLOG_MAXWAIT_US after
// the last write (a program stuck polling a port never gets idle and its identical reads are
// merged, so the buffer never fills), IOLOG_CHUNK entries per call, as the main loop also
// does the audio mix.

#if SBDSP_IOLOG

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "ff.h"
#include "sbdsp_iolog.h"

#define IOLOG_FILE     "sbiolog.txt"
#define IOLOG_SIZE     2048          // Entries (12 bytes each)
#define IOLOG_IDLE_US    500000
#define IOLOG_MAXWAIT_US 1000000
#define IOLOG_CHUNK      32

bool PM_EnableSD();
bool PM_PICO_EnablePSRAM();

typedef struct {
  uint32_t time;
  uint32_t count;                    // Consecutive identical reads
  uint16_t port;
  uint8_t value;
  uint8_t type;
} iolog_t;

static iolog_t log_buf[IOLOG_SIZE];
static volatile uint32_t log_head, log_tail;   // Free running indexes
static volatile uint32_t log_lost, log_last;
static spin_lock_t *log_lock;

void sbdsp_iolog_init(void)
{
  log_lock = spin_lock_instance(next_striped_spin_lock_num());
}

void __not_in_flash_func(sbdsp_iolog_add)(uint8_t type, uint16_t port, uint8_t value)
{
  if (log_lock == NULL) return;
  uint32_t now = time_us_32();
  uint32_t irq = spin_lock_blocking(log_lock);
  uint32_t h = log_head;
  iolog_t *p = &log_buf[(h - 1) % IOLOG_SIZE];

  if (type == IOLOG_R && h != log_tail && p->type == IOLOG_R && p->port == port && p->value == value) p->count++;
     else if (h - log_tail >= IOLOG_SIZE) log_lost++;
     else {
      p = &log_buf[h % IOLOG_SIZE];
      p->time = now;
      p->count = 1;
      p->port = port;
      p->value = value;
      p->type = type;
      log_head = h + 1;
     }
  log_last = now;
  spin_unlock(log_lock, irq);
}

// Take the oldest entry (under the lock: the last one can still be merged)
static bool iolog_get(iolog_t *e)
{
  uint32_t irq = spin_lock_blocking(log_lock);
  bool ok = (log_tail != log_head);
  if (ok) {
      *e = log_buf[log_tail % IOLOG_SIZE];
      log_tail = log_tail + 1;
     }
  spin_unlock(log_lock, irq);
  return ok;
}

static int iolog_format(char *s, size_t n, const iolog_t *e)
{
  unsigned long t = e->time;
  switch (e->type) {
    case IOLOG_IRQ: return snprintf(s, n, "%10lu I irq %u %s\n", t, e->value, e->port ? "(IRQ line)" : "(through the PicoMEM IRQ)");
    case IOLOG_DSP: return snprintf(s, n, "%10lu S dma size=%u mode=%u auto=%u\n", t, e->port, e->value & 0x7F, e->value >> 7);
    case IOLOG_E2:
      if (e->port == 0xFFFF) return snprintf(s, n, "%10lu E E2 answer not written: PicoMEM IRQ %u (none or same as the SB)\n", t, e->value);
      return snprintf(s, n, "%10lu E %04X=%02X\n", t, e->port, e->value);
    default:
      if (e->count > 1) return snprintf(s, n, "%10lu %c %02X=%02X x%lu\n", t, e->type, e->port, e->value, (unsigned long)e->count);
      return snprintf(s, n, "%10lu %c %02X=%02X\n", t, e->type, e->port, e->value);
   }
}

void sbdsp_iolog_task(void)
{
  static FIL f;
  static bool open = false, failed = false, dirty = false;
  static char buf[IOLOG_CHUNK * 48 + 64];
  static uint32_t last_write;

  if (failed || log_lock == NULL) return;
  uint32_t now = time_us_32();
  uint32_t used = log_head - log_tail;
  bool idle = (now - log_last) >= IOLOG_IDLE_US;
  bool overdue = (now - last_write) >= IOLOG_MAXWAIT_US;
  if (!idle && !overdue && used < IOLOG_SIZE / 2) return;
  if (used == 0 && log_lost == 0 && !dirty) return;
  last_write = now;

  if (!PM_EnableSD()) return;
  if (!open) {
      // New file at the first write after power on, then append (if reopened after an error)
      static bool created = false;
      if (f_open(&f, IOLOG_FILE, created ? (FA_WRITE | FA_OPEN_APPEND) : (FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) {
          failed = true;
          PM_PICO_EnablePSRAM();
          return;
         }
      if (!created) f_puts("# PicoMEM I/O log: us, W/R SB port write/read, D 8237 write, I SB IRQ, S DSP DMA start\n", &f);
      created = true;
      open = true;
     }

  int len = 0;
  uint32_t lost;
  uint32_t irq = spin_lock_blocking(log_lock);
  lost = log_lost;
  log_lost = 0;
  spin_unlock(log_lock, irq);
  if (lost) len += snprintf(buf + len, sizeof(buf) - len, "# %lu entries lost (buffer full)\n", (unsigned long)lost);

  iolog_t e;
  for (int i = 0; i < IOLOG_CHUNK && iolog_get(&e); i++)
      len += iolog_format(buf + len, sizeof(buf) - len, &e);

  UINT bw;
  FRESULT fr = FR_OK;
  if (len) {
      fr = f_write(&f, buf, len, &bw);
      dirty = true;
     }
  if (fr == FR_OK && (log_head == log_tail || overdue)) {  // Everything written (or a stuck program still polling): make it visible on the card
      fr = f_sync(&f);
      dirty = false;
     }
  if (fr != FR_OK) {        // Card remounted/removed: reopen in append mode next time
      f_close(&f);
      open = false;
     }
  PM_PICO_EnablePSRAM();
}

#endif // SBDSP_IOLOG
