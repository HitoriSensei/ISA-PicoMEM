// Firmware update from the SD card for the RP2350 (PicoMEM 2.0), using the bootrom A/B partitions.
//
// pm_fwupdate_check()  (SD card mounted, core 1 already running the ISA bus)
//   1. Read picomem.uf2 a first time to check it: RP2350 Arm family, all the blocks present,
//      fits in the slot, contains the PicoMEM 2.0 tag. If refused, the PicoMEM starts normally.
//   2. Stop core 1 (the PC should be restarted after the update), go back to the boot clock, erase
//      the free slot and write the image. The first sector (with the IMAGE_DEF) is written last:
//      an interrupted update leaves no valid image in the slot.
//   3. Read the slot back, then reboot with a "flash update" boot of the new slot.
// pm_fwupdate_boot()   (start of main, before the overclock and core 1)
//   After a flash update boot, "buy" the new image (TBYB) and erase the start of the other slot:
//   with no version in the images, the bootrom could prefer the old one on the next normal boot.

#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "boot/uf2.h"
#include "ff.h"
#include "../pm_debug.h"
#include "pm_fwupdate.h"

#if BOARD_PM20
#define FW_BOARD "PICOMEM_20"
#else
#error "Firmware update from the SD card is only supported on the PicoMEM 2.0"
#endif

#define FW_FILE       "picomem.uf2"
#define FW_FILE_DONE  "picomem.uf2.done"
#define FW_FILE_BAD   "picomem.uf2.bad"
#define FW_LOG        "fwupdate.txt"     // Result of the last update (no screen at this point)

#define UF2_PAYLOAD   256

bool PM_EnableSD();

// Present in every firmware built with this code: an UF2 without it is refused
static const char fw_tag[] = "PicoMEM firmware for " FW_BOARD;

typedef struct {
  uint32_t start;               // Flash storage offset
  uint32_t size;
  uint32_t flags;
} part_t;

typedef struct {
  struct uf2_block b;
  uint8_t sector0[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));   // Also the bootrom work area
  uint8_t tx[4 + UF2_PAYLOAD];
  uint8_t rx[4 + UF2_PAYLOAD];
  uint8_t win[sizeof(fw_tag) - 1 + UF2_PAYLOAD];
} fw_work_t;

// ---------------------------------------------------------------------------
//  Flash access (core 1 stopped or not started, the IRQs are disabled)
// ---------------------------------------------------------------------------

static void flash_erase(uint32_t offs, uint32_t len)
{
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(offs, len);
  restore_interrupts(irq);
}

static void flash_program(uint32_t offs, const uint8_t *data, uint32_t len)
{
  uint32_t irq = save_and_disable_interrupts();
  flash_range_program(offs, data, len);
  restore_interrupts(irq);
}

static void flash_cmd(const uint8_t *tx, uint8_t *rx, size_t len)
{
  uint32_t irq = save_and_disable_interrupts();
  flash_do_cmd(tx, rx, len);
  restore_interrupts(irq);
}

// The bootrom may leave the flash in its slow 03h mode: restart the XIP before going back to flash code
static int __no_inline_not_in_flash_func(fw_buy)(uint8_t *buf, uint32_t size)
{
  rom_explicit_buy_fn buy = (rom_explicit_buy_fn)rom_func_lookup_inline(ROM_FUNC_EXPLICIT_BUY);
  uint32_t irq = save_and_disable_interrupts();
  int rc = buy(buf, size);
  flash_start_xip();
  restore_interrupts(irq);
  return rc;
}

// ---------------------------------------------------------------------------
//  Partitions
// ---------------------------------------------------------------------------

static bool part_get(uint num, part_t *p)
{
  uint32_t info[3];
  int n = rom_get_partition_table_info(info, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (num << 24));
  if (n != 3 || !(info[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS)) return false;
  uint32_t first = (info[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
  uint32_t last  = (info[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
  p->start = first * FLASH_SECTOR_SIZE;
  p->size  = (last + 1 - first) * FLASH_SECTOR_SIZE;
  p->flags = info[2];
  return true;
}

// The other slot of the A/B pair the running image was started from
static bool other_slot(uint8_t *work, part_t *other)
{
  boot_info_t bi;
  part_t me;

  if (!rom_get_boot_info(&bi) || bi.partition < 0) return false;    // Not started from a partition
  if (rom_load_partition_table(work, FLASH_SECTOR_SIZE, false) != 0) return false;
  if (!part_get(bi.partition, &me)) return false;

  int o;
  if (((me.flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION)
     o = (me.flags & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB;   // We are B
     else o = rom_get_b_partition(bi.partition);                                                            // We are A
  return o >= 0 && o != bi.partition && part_get(o, other);
}

// ---------------------------------------------------------------------------
//  UF2 file
// ---------------------------------------------------------------------------

static void fw_log(const char *msg)
{
  FIL l;
  UINT bw;
  if (f_open(&l, FW_LOG, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
  f_write(&l, msg, strlen(msg), &bw);
  f_write(&l, "\r\n", 2, &bw);
  f_close(&l);
}

// Deletes the result file of a previous update (picomem.uf2.done / .bad): before the update, so
// that picomem.uf2 can be renamed after it. Returns NULL if it is gone (or was not there).
static const char *fw_delete_old(const char *name, char *msg, size_t size)
{
  FRESULT fr = f_unlink(name);
  if (fr == FR_OK || fr == FR_NO_FILE) return NULL;
  snprintf(msg, size, "Failed: can't delete %s (FatFS error %d), nothing written", name, (int)fr);
  return msg;
}

// picomem.uf2 -> picomem.uf2.done / .bad (the old one was deleted before the update). If it
// fails, the result logged says so: picomem.uf2 is still there and is tried again at the next boot.
static void fw_rename(const char *to, char *msg, size_t size)
{
  FRESULT fr = f_rename(FW_FILE, to);
  size_t n = strlen(msg);
  if (fr != FR_OK && n < size) snprintf(msg + n, size - n, " (rename to %s failed: FatFS error %d)", to, (int)fr);
}

// Next 512 bytes block: 1 ok, 0 end of file, -1 error
static int fw_read(FIL *f, struct uf2_block *b)
{
  UINT br;
  if (f_read(f, b, sizeof(*b), &br) != FR_OK) return -1;
  if (br == 0) return 0;
  return (br == sizeof(*b)) ? 1 : -1;
}

// Block to write in the slot (the others, like the RP2350-E10 absolute block, are ignored)
static bool fw_block(const struct uf2_block *b)
{
  return !(b->flags & UF2_FLAG_NOT_MAIN_FLASH) && (b->flags & UF2_FLAG_FAMILY_ID_PRESENT) &&
         b->file_size == RP2350_ARM_S_FAMILY_ID;
}

static bool tag_in(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i + sizeof(fw_tag) <= n; i++)
      if (p[i] == (uint8_t)fw_tag[0] && !memcmp(p + i, fw_tag, sizeof(fw_tag))) return true;
  return false;
}

// Pass 1: check the whole file before touching the flash. Returns NULL if ok
static const char *check_file(FIL *f, fw_work_t *w, uint32_t slot_size, uint32_t *img_end)
{
  const size_t carry_max = sizeof(fw_tag) - 1;
  uint32_t max_blocks = slot_size / UF2_PAYLOAD;
  uint32_t total = 0, count = 0, end = 0, prev = 0;
  size_t carry = 0;
  bool start = false, tag = false;
  const char *err = NULL;
  int r;

  uint8_t *seen = calloc((max_blocks + 7) / 8, 1);
  if (seen == NULL) return "out of memory";

  while ((r = fw_read(f, &w->b)) > 0) {
      struct uf2_block *b = &w->b;
      if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END) { err = "not an UF2 file"; break; }
      if ((b->flags & UF2_FLAG_FAMILY_ID_PRESENT) && b->file_size == RP2040_FAMILY_ID) { err = "RP2040 firmware"; break; }
      if (!fw_block(b)) continue;

      uint32_t off = b->target_addr - XIP_BASE;
      if (b->payload_size != UF2_PAYLOAD || (b->target_addr % UF2_PAYLOAD) || b->target_addr < XIP_BASE || off >= slot_size) { err = "bad block address"; break; }
      if (total == 0) {
          total = b->num_blocks;
          if (total == 0 || total > max_blocks) { err = "firmware too big"; break; }
         }
      if (b->num_blocks != total || b->block_no >= total) { err = "bad block number"; break; }
      if (seen[b->block_no / 8] & (1 << (b->block_no % 8))) { err = "duplicated block"; break; }
      seen[b->block_no / 8] |= 1 << (b->block_no % 8);
      count++;
      if (off == 0) start = true;
      if (off + UF2_PAYLOAD > end) end = off + UF2_PAYLOAD;

      // Look for the tag, also across two consecutive blocks
      if (!tag) {
          if (off != prev + UF2_PAYLOAD) carry = 0;
          memcpy(w->win + carry, b->data, UF2_PAYLOAD);
          size_t n = carry + UF2_PAYLOAD;
          tag = tag_in(w->win, n);
          memmove(w->win, w->win + n - carry_max, carry_max);
          carry = carry_max;
         }
      prev = off;
     }
  free(seen);

  if (err) return err;
  if (r < 0) return "SD read error or truncated file";
  if (count == 0 || count != total) return "missing blocks";
  if (!start) return "no image start";
  if (!tag) return "not a " FW_BOARD " firmware";
  *img_end = end;
  return NULL;
}

// Pass 2: write the slot, the first sector last
static const char *write_image(FIL *f, fw_work_t *w, const part_t *slot, uint32_t img_end)
{
  int r;

  flash_erase(slot->start, (img_end + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1));
  memset(w->sector0, 0xFF, sizeof(w->sector0));
  if (f_lseek(f, 0) != FR_OK) return "SD read error";

  while ((r = fw_read(f, &w->b)) > 0) {
      if (!fw_block(&w->b)) continue;
      uint32_t off = w->b.target_addr - XIP_BASE;
      if (off < FLASH_SECTOR_SIZE) memcpy(w->sector0 + off, w->b.data, UF2_PAYLOAD);
         else flash_program(slot->start + off, w->b.data, UF2_PAYLOAD);
     }
  if (r < 0) return "SD read error";

  flash_program(slot->start, w->sector0, FLASH_SECTOR_SIZE);
  return NULL;
}

// Pass 3: read the slot back (03h read command, the slot is not mapped in the XIP window)
static const char *verify_image(FIL *f, fw_work_t *w, const part_t *slot)
{
  int r;

  memset(w->tx, 0, sizeof(w->tx));
  w->tx[0] = 0x03;
  if (f_lseek(f, 0) != FR_OK) return "SD read error";

  while ((r = fw_read(f, &w->b)) > 0) {
      if (!fw_block(&w->b)) continue;
      uint32_t a = slot->start + (w->b.target_addr - XIP_BASE);
      w->tx[1] = a >> 16;
      w->tx[2] = a >> 8;
      w->tx[3] = a;
      flash_cmd(w->tx, w->rx, sizeof(w->rx));
      if (memcmp(w->rx + 4, w->b.data, UF2_PAYLOAD)) return "flash verify error";
     }
  return (r < 0) ? "SD read error" : NULL;
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------

void pm_fwupdate_boot(void)
{
  boot_info_t bi;
  part_t other;

  if (!rom_get_boot_info(&bi) || (bi.boot_type & ~BOOT_TYPE_CHAINED_FLAG) != BOOT_TYPE_FLASH_UPDATE) return;

  uint8_t *work = malloc(FLASH_SECTOR_SIZE);
  if (work == NULL) return;

  // Also needed without partition table (UF2 copied in BOOTSEL mode): a TBYB image not bought
  // is restarted by the bootrom watchdog, then no more started at all
  if (!(bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) || fw_buy(work, FLASH_SECTOR_SIZE) == 0) {
      if (!(bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_OTHER_ERASED) && other_slot(work, &other))
          flash_erase(other.start, FLASH_SECTOR_SIZE);
     }
  free(work);
}

void pm_fwupdate_check(void)
{
  FIL f;
  part_t slot;
  uint32_t end = 0;
  const char *err;
  char msg[160];

  if (!PM_EnableSD() || f_open(&f, FW_FILE, FA_READ | FA_OPEN_EXISTING) != FR_OK) return;
  PM_INFO("FW update: " FW_FILE " found\n");

  // The results of a previous update must go first: if one can't be deleted (read only, a
  // folder with that name, SD error), picomem.uf2 could not be renamed after the update
  if ((err = fw_delete_old(FW_FILE_DONE, msg, sizeof(msg))) != NULL ||
      (err = fw_delete_old(FW_FILE_BAD, msg, sizeof(msg))) != NULL) {
      PM_ERROR("FW update: %s\n", err);
      f_close(&f);
      fw_log(err);
      return;
     }

  fw_work_t *w = malloc(sizeof(fw_work_t));
  if (w == NULL) err = "out of memory";
     else if (!other_slot(w->sector0, &slot)) err = "no A/B partition table (see README)";
     else err = check_file(&f, w, slot.size, &end);

  if (err) {    // Nothing written: normal start
      snprintf(msg, sizeof(msg), "Refused: %s", err);
      PM_ERROR("FW update: %s\n", msg);
      f_close(&f);
      fw_rename(FW_FILE_BAD, msg, sizeof(msg));
      fw_log(msg);
      free(w);
      return;
     }

  // No way back for this boot: the ISA bus is no more served (the PC must be restarted)
  multicore_reset_core1();
  set_sys_clock_khz(150000, true);   // The flash functions put back the boot flash timing (see BOARD_Init)
  PM_INFO("FW update: writing %lu KB at 0x%lx\n", (unsigned long)(end / 1024), (unsigned long)slot.start);

  err = write_image(&f, w, &slot, end);
  if (err == NULL) err = verify_image(&f, w, &slot);
  f_close(&f);

  if (err) {
      flash_erase(slot.start, FLASH_SECTOR_SIZE);   // Leave no valid image in the slot
      snprintf(msg, sizeof(msg), "Failed: %s", err);
      PM_ERROR("FW update: %s\n", msg);
      fw_rename(FW_FILE_BAD, msg, sizeof(msg));
      fw_log(msg);
      rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
     }
     else {
      snprintf(msg, sizeof(msg), "Installed: %lu KB at 0x%lx", (unsigned long)(end / 1024), (unsigned long)slot.start);
      PM_INFO("FW update: %s\n", msg);
      fw_rename(FW_FILE_DONE, msg, sizeof(msg));
      fw_log(msg);
      rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, XIP_BASE + slot.start, 0);
     }
  for (;;) tight_loop_contents();
}
