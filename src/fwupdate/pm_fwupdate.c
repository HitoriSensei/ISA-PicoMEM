// Firmware update from the SD card for the RP2350 (PicoMEM 2.0), using the bootrom A/B partitions.
//
// pm_fwupdate_check()  (SD card mounted and config loaded, core 1 already running the ISA bus)
//   1. Read picomem.uf2 a first time to check it: RP2350 Arm family, all the blocks present,
//      fits in the slot, contains the PicoMEM 2.0 tag. If refused, the PicoMEM starts normally.
//   2. Stop core 1 (the PC should be restarted after the update), go back to the boot clock, show
//      "Updating..." on the OLED screen (if any), erase the free slot and write the image. The first
//      sector (with the IMAGE_DEF) is written last: an interrupted update leaves no valid image in
//      the slot.
//   3. Read the slot back, then reboot with a "flash update" boot of the new slot.
// With partition.uf2 next to picomem.uf2 (a new partition table, with its hash):
//   1. The firmware slot is taken from the new table: an A/B partition outside the running one.
//   2. The firmware is written and checked as above, then the table is installed. The bootrom has
//      two partition table slots (flash 0 and 4K): the new table is written in the free one, then
//      the old one is erased and the bootrom reloads the table. It ends in slot 0 (as with a
//      partition.uf2 copied in BOOTSEL mode). A power cut leaves at least one valid table.
// pm_fwupdate_boot()   (start of main, before the overclock and core 1)
//   After a flash update boot, "buy" the new image (TBYB) and erase the start of the other slot:
//   with no version in the images, the bootrom could prefer the old one on the next normal boot.

#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/sha256.h"
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
#define PT_FILE       "partition.uf2"
#define PT_FILE_DONE  "partition.uf2.done"
#define PT_FILE_BAD   "partition.uf2.bad"
#define FW_LOG        "fwupdate.txt"     // Result of the last update

#define UF2_PAYLOAD   256
#define PT_SLOT_SIZE  0x1000             // Partition table slots 0 and 1 (OTP default)
#define PT_LINK_BITS  (PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS | PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS)

bool PM_EnableSD();

#if USE_OLED
extern uint8_t PM_OLED_Type;           // Read from picomem.cfg
void pm_display_fwupdate(uint8_t oled_type, const char *msg);
#endif

// Present in every firmware built with this code: an UF2 without it is refused
static const char fw_tag[] = "PicoMEM firmware for " FW_BOARD;

static bool fw_refused;                // Update refused, the PicoMEM starts normally

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

typedef struct {
  uint8_t blk[PT_SLOT_SIZE] __attribute__((aligned(4)));   // New partition table, as written in a slot
  uint8_t old[PT_SLOT_SIZE] __attribute__((aligned(4)));   // Installed one, put back if the new one is not taken
  part_t  p[PARTITION_TABLE_MAX_PARTITIONS];               // Partitions of the new table
  part_t  q[PARTITION_TABLE_MAX_PARTITIONS];
  uint32_t n;
} pt_work_t;

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

// 03h read command: the XIP window only shows the running partition. len: multiple of 256
static void flash_read(fw_work_t *w, uint32_t offs, uint8_t *dst, uint32_t len)
{
  memset(w->tx, 0, sizeof(w->tx));
  w->tx[0] = 0x03;
  for (uint32_t i = 0; i < len; i += UF2_PAYLOAD) {
      uint32_t a = offs + i;
      w->tx[1] = a >> 16;
      w->tx[2] = a >> 8;
      w->tx[3] = a;
      flash_cmd(w->tx, w->rx, sizeof(w->rx));
      memcpy(dst + i, w->rx + 4, UF2_PAYLOAD);
     }
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

// Same for a forced reload of the partition table from the flash
static int __no_inline_not_in_flash_func(pt_reload_rom)(uint8_t *buf, uint32_t size)
{
  rom_load_partition_table_fn load = (rom_load_partition_table_fn)rom_func_lookup_inline(ROM_FUNC_LOAD_PARTITION_TABLE);
  uint32_t irq = save_and_disable_interrupts();
  int rc = load(buf, size, true);
  flash_start_xip();
  restore_interrupts(irq);
  return rc;
}

// The bootrom lock checking is on (SDK runtime init): loading the table needs the SHA-256 lock
static int pt_reload(uint8_t *buf, uint32_t size)
{
  if (!bootrom_try_acquire_lock(BOOTROM_LOCK_SHA_256)) return BOOTROM_ERROR_LOCK_REQUIRED;
  int rc = pt_reload_rom(buf, size);
  bootrom_release_lock(BOOTROM_LOCK_SHA_256);
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

// The partition the running image was started from: its number, -1 if none
static int running_part(uint8_t *work, part_t *me)
{
  boot_info_t bi;

  if (!rom_get_boot_info(&bi) || bi.partition < 0) return -1;    // Not started from a partition
  if (rom_load_partition_table(work, FLASH_SECTOR_SIZE, false) != 0) return -1;
  return part_get(bi.partition, me) ? bi.partition : -1;
}

// The other slot of the A/B pair the running image was started from
static bool other_slot(uint8_t *work, part_t *other)
{
  part_t me;
  int num = running_part(work, &me);
  if (num < 0) return false;

  int o;
  if (((me.flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION)
     o = (me.flags & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB;   // We are B
     else o = rom_get_b_partition(num);                                                                     // We are A
  return o >= 0 && o != num && part_get(o, other);
}

// ---------------------------------------------------------------------------
//  Partition table (partition.uf2)
// ---------------------------------------------------------------------------

// PARTITION_TABLE item at w[i]: the partition count in its top byte, the unpartitioned space, then
// for each partition: location, flags, [id: 2 words], [extra families], [name]
static const char *pt_items(const uint8_t *blk, uint32_t i, uint32_t size, part_t *p, uint32_t *n)
{
  const uint32_t *w = (const uint32_t *)blk;
  uint32_t end = i + size, j = i + 2;
  uint32_t count = w[i] >> 24;

  if (count == 0 || count > PARTITION_TABLE_MAX_PARTITIONS) return "bad partition count";
  for (uint32_t k = 0; k < count; k++) {
      if (j + 2 > end) return "bad partition table";
      uint32_t loc = w[j], flags = w[j + 1];
      j += 2;
      if (flags & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS) j += 2;
      j += (flags & PICOBIN_PARTITION_FLAGS_ACCEPTS_NUM_EXTRA_FAMILIES_BITS) >> PICOBIN_PARTITION_FLAGS_ACCEPTS_NUM_EXTRA_FAMILIES_LSB;
      if (flags & PICOBIN_PARTITION_FLAGS_HAS_NAME_BITS) {
          if (j >= end) return "bad partition table";
          j += (1 + (blk[j * 4] & 0x7f) + 3) / 4;      // Length byte, then the characters
         }
      uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
      uint32_t last  = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
      if (last < first) return "bad partition location";
      p[k].start = first * FLASH_SECTOR_SIZE;
      p[k].size  = (last + 1 - first) * FLASH_SECTOR_SIZE;
      p[k].flags = flags;
     }
  if (j != end) return "bad partition table";
  *n = count;
  return NULL;
}

// Checks a partition table block (markers, items, SHA-256 hash) and reads its partitions.
// Returns NULL if ok
static const char *pt_parse(const uint8_t *blk, part_t *p, uint32_t *n)
{
  const uint32_t *w = (const uint32_t *)blk;
  const uint32_t max = PICOBIN_MAX_PARTITION_TABLE_BLOCK_SIZE / 4;
  uint32_t i = 1, hashed = 0, hash = 0, hash_len = 0;
  bool pt = false;
  const char *err;

  if (w[0] != PICOBIN_BLOCK_MARKER_START) return "no partition table";
  for (;;) {
      if (i >= max) return "bad block";
      uint32_t h = w[i];
      uint32_t type = h & 0xff;
      uint32_t size = (type & 0x80) ? (h >> 8) & 0xffff : (h >> 8) & 0xff;
      if (type == PICOBIN_BLOCK_ITEM_2BS_LAST) {     // Size of all the items, link to the next block, end marker
          if (size != i - 1 || i + 2 >= max || w[i + 2] != PICOBIN_BLOCK_MARKER_END) return "bad block";
          break;
         }
      if (size == 0 || i + size > max) return "bad block";
      if (type == PICOBIN_BLOCK_ITEM_PARTITION_TABLE) {
          if (pt) return "two partition tables";
          if ((err = pt_items(blk, i, size, p, n)) != NULL) return err;
          pt = true;
         }
         else if (type == PICOBIN_BLOCK_ITEM_1BS_HASH_DEF) {
          if (size != 2 || (h >> 24) != PICOBIN_HASH_SHA256) return "unsupported hash";
          hashed = w[i + 1] & 0xffff;                 // Words hashed from the block start
         }
         else if (type == PICOBIN_BLOCK_ITEM_HASH_VALUE) {
          hash = i + 1;
          hash_len = size - 1;
         }
      i += size;
     }
  if (!pt) return "no partition table";
  if (hashed == 0 || hash == 0 || hash_len == 0 || hash_len > 8 || hashed >= hash) return "no hash";

  pico_sha256_state_t st;
  sha256_result_t res;
  if (pico_sha256_try_start(&st, SHA256_BIG_ENDIAN, false) != PICO_OK) return "SHA-256 busy";
  pico_sha256_update_blocking(&st, blk, hashed * 4);
  pico_sha256_finish(&st, &res);
  return memcmp(res.words, &w[hash], hash_len * 4) ? "bad hash" : NULL;
}

static int fw_read(FIL *f, struct uf2_block *b);

// The table the bootrom has loaded has the partitions p[0..n-1]
static bool pt_same(const part_t *p, uint32_t n)
{
  uint32_t info[4];
  part_t q;

  if (rom_get_partition_table_info(info, 4, PT_INFO_PT_INFO) != 4 || !(info[0] & PT_INFO_PT_INFO) || (info[1] & 0xff) != n) return false;
  for (uint32_t k = 0; k < n; k++)
      if (!part_get(k, &q) || q.start != p[k].start || q.size != p[k].size ||
          (q.flags & PT_LINK_BITS) != (p[k].flags & PT_LINK_BITS)) return false;
  return true;
}

// partition.uf2: the table block at the start of the flash. Returns NULL if ok
static const char *pt_read(FIL *f, fw_work_t *w, pt_work_t *t)
{
  bool start = false;
  int r;

  memset(t->blk, 0xFF, sizeof(t->blk));
  while ((r = fw_read(f, &w->b)) > 0) {
      struct uf2_block *b = &w->b;
      if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END) return "not an UF2 file";
      if (!(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) || b->file_size != ABSOLUTE_FAMILY_ID) return "not a partition table";
      if (b->flags & UF2_FLAG_NOT_MAIN_FLASH) continue;

      uint32_t off = b->target_addr - XIP_BASE;
      if (b->target_addr < XIP_BASE || off >= PT_SLOT_SIZE) continue;   // RP2350-E10 absolute block (end of the flash)
      if (b->payload_size != UF2_PAYLOAD || (off % UF2_PAYLOAD)) return "bad block address";
      memcpy(t->blk + off, b->data, UF2_PAYLOAD);
      if (off == 0) start = true;
     }
  if (r < 0) return "SD read error or truncated file";
  if (!start) return "no partition table";
  return pt_parse(t->blk, t->p, &t->n);
}

// The firmware slot in the new table: a partition of an A/B pair for Arm images, after the
// partition table slots and outside the running partition
static bool pt_target(const pt_work_t *t, const part_t *me, part_t *slot)
{
  for (uint32_t k = 0; k < t->n; k++) {
      const part_t *p = &t->p[k];
      bool ab = ((p->flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION;
      for (uint32_t j = 0; j < t->n && !ab; j++)      // An A partition: one links to it
          ab = ((t->p[j].flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION &&
               ((t->p[j].flags & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB) == k;
      if (!ab || !(p->flags & PICOBIN_PARTITION_FLAGS_ACCEPTS_DEFAULT_FAMILY_RP2350_ARM_S_BITS)) continue;
      if (p->start < 2 * PT_SLOT_SIZE) continue;
      if (p->start < me->start + me->size && me->start < p->start + p->size) continue;
      *slot = *p;
      return true;
     }
  return false;
}

// The partition table slot (0 / 1) the loaded table comes from (copied in t->old), -1 if none
static int pt_active(fw_work_t *w, pt_work_t *t)
{
  uint32_t n;
  for (int s = 0; s < 2; s++) {
      flash_read(w, s * PT_SLOT_SIZE, t->old, PT_SLOT_SIZE);
      if (pt_parse(t->old, t->q, &n) == NULL && pt_same(t->q, n)) return s;
     }
  return -1;
}

static bool pt_program(fw_work_t *w, int s, const uint8_t *blk)
{
  flash_erase(s * PT_SLOT_SIZE, PT_SLOT_SIZE);
  flash_program(s * PT_SLOT_SIZE, blk, PT_SLOT_SIZE);
  flash_read(w, s * PT_SLOT_SIZE, w->sector0, PT_SLOT_SIZE);
  return !memcmp(w->sector0, blk, PT_SLOT_SIZE);
}

// Installs the new table, the loaded one being in slot a. Returns NULL if the bootrom took it,
// else the old table is put back
static const char *pt_install(fw_work_t *w, pt_work_t *t, int a)
{
  static char err[80];
  int o = 1 - a, rc;

  if (!pt_program(w, o, t->blk)) {
      flash_erase(o * PT_SLOT_SIZE, PT_SLOT_SIZE);
      return "partition table verify error";
     }
  flash_erase(a * PT_SLOT_SIZE, PT_SLOT_SIZE);       // Only the new table is valid now
  if ((rc = pt_reload(w->sector0, sizeof(w->sector0))) != 0 || !pt_same(t->p, t->n)) {
      if (rc) snprintf(err, sizeof(err), "new partition table not taken by the bootrom (load error %d)", rc);
         else snprintf(err, sizeof(err), "new partition table not taken by the bootrom (other layout loaded)");
      pt_program(w, a, t->old);
      flash_erase(o * PT_SLOT_SIZE, PT_SLOT_SIZE);
      pt_reload(w->sector0, sizeof(w->sector0));
      return err;
     }

  // Move it to slot 0: a partition.uf2 copied in BOOTSEL mode goes there, it must not have an older
  // table in slot 1 next to it
  if (o != 0) {
      if (pt_program(w, 0, t->blk)) flash_erase(PT_SLOT_SIZE, PT_SLOT_SIZE);
         else flash_erase(0, PT_SLOT_SIZE);          // Stays in slot 1
      pt_reload(w->sector0, sizeof(w->sector0));
     }
  return NULL;
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

// picomem.uf2 / partition.uf2 -> .done / .bad (the old one was deleted before the update). If it
// fails, the result logged says so: the file is still there and is tried again at the next boot.
static void fw_rename(const char *from, const char *to, char *msg, size_t size)
{
  FRESULT fr = f_rename(from, to);
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

bool pm_fwupdate_refused(void)
{
  return fw_refused;
}

// partition.uf2: reads the new table and the firmware slot in it. *pt_new is false if this table
// is already installed (the firmware then goes in the other A/B slot, as without partition.uf2)
static const char *pt_prepare(FIL *pf, fw_work_t *w, pt_work_t *t, part_t *slot, bool *pt_new)
{
  part_t me;
  const char *err;

  if (running_part(w->sector0, &me) < 0) return "no A/B partition table (see README)";
  if ((err = pt_read(pf, w, t)) != NULL) return err;
  *pt_new = !pt_same(t->p, t->n);
  if (!*pt_new) return other_slot(w->sector0, slot) ? NULL : "no A/B partition table (see README)";
  return pt_target(t, &me, slot) ? NULL : "no firmware slot outside the running one";
}

void pm_fwupdate_check(void)
{
  FIL f, pf;
  part_t slot;
  uint32_t end = 0;
  const char *err = NULL, *what = "";
  char msg[200];
  bool pt_file, pt_new = false;
  pt_work_t *t = NULL;

  if (!PM_EnableSD()) return;
  pt_file = f_open(&pf, PT_FILE, FA_READ | FA_OPEN_EXISTING) == FR_OK;
  if (f_open(&f, FW_FILE, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
      if (!pt_file) return;
      f_close(&pf);
      snprintf(msg, sizeof(msg), "Refused: " PT_FILE " without " FW_FILE " (both are needed)");
      PM_ERROR("FW update: %s\n", msg);
      if (fw_delete_old(PT_FILE_BAD, msg, sizeof(msg)) == NULL) fw_rename(PT_FILE, PT_FILE_BAD, msg, sizeof(msg));
      fw_log(msg);
      fw_refused = true;
      return;
     }
  PM_INFO("FW update: " FW_FILE "%s found\n", pt_file ? " and " PT_FILE : "");

  // The results of a previous update must go first: if one can't be deleted (read only, a
  // folder with that name, SD error), picomem.uf2 could not be renamed after the update
  if ((err = fw_delete_old(FW_FILE_DONE, msg, sizeof(msg))) != NULL ||
      (err = fw_delete_old(FW_FILE_BAD, msg, sizeof(msg))) != NULL ||
      (pt_file && ((err = fw_delete_old(PT_FILE_DONE, msg, sizeof(msg))) != NULL ||
                   (err = fw_delete_old(PT_FILE_BAD, msg, sizeof(msg))) != NULL))) {
      PM_ERROR("FW update: %s\n", err);
      f_close(&f);
      if (pt_file) f_close(&pf);
      fw_log(err);
      fw_refused = true;
      return;
     }

  fw_work_t *w = malloc(sizeof(fw_work_t));
  if (pt_file) t = malloc(sizeof(pt_work_t));
  if (w == NULL || (pt_file && t == NULL)) err = "out of memory";
     else if (pt_file) {
      if ((err = pt_prepare(&pf, w, t, &slot, &pt_new)) != NULL) what = PT_FILE ": ";
     }
     else if (!other_slot(w->sector0, &slot)) err = "no A/B partition table (see README)";
  if (err == NULL) err = check_file(&f, w, slot.size, &end);
  if (pt_file) f_close(&pf);

  if (err) {    // Nothing written: normal start
      snprintf(msg, sizeof(msg), "Refused: %s%s", what, err);
      PM_ERROR("FW update: %s\n", msg);
      f_close(&f);
      fw_rename(FW_FILE, FW_FILE_BAD, msg, sizeof(msg));
      if (pt_file) fw_rename(PT_FILE, PT_FILE_BAD, msg, sizeof(msg));
      fw_log(msg);
      free(t);
      free(w);
      fw_refused = true;
      return;
     }

  // No way back for this boot: the ISA bus is no more served (the PC must be restarted)
  multicore_reset_core1();
  set_sys_clock_khz(150000, true);   // The flash functions put back the boot flash timing (see BOARD_Init)
#if USE_OLED
  pm_display_fwupdate(PM_OLED_Type, "Updating...");  // After the clock change: the I2C baud rate depends on it
#endif
  PM_INFO("FW update: writing %lu KB at 0x%lx%s\n", (unsigned long)(end / 1024), (unsigned long)slot.start, pt_new ? " (new partition table)" : "");

  // Flash reads: only with core 1 stopped. Nothing is written if the loaded table is not found
  int pt_slot = pt_new ? pt_active(w, t) : -1;
  bool written = false;
  if (pt_new && pt_slot < 0) err = "installed partition table not found in the flash";
     else {
      written = true;
      err = write_image(&f, w, &slot, end);
      if (err == NULL) err = verify_image(&f, w, &slot);
      if (err == NULL && pt_new) err = pt_install(w, t, pt_slot);
     }
  f_close(&f);

  if (err) {
      if (written) flash_erase(slot.start, FLASH_SECTOR_SIZE);   // Leave no valid image in the slot
      snprintf(msg, sizeof(msg), "Failed: %s", err);
      PM_ERROR("FW update: %s\n", msg);
      fw_rename(FW_FILE, FW_FILE_BAD, msg, sizeof(msg));
      if (pt_file) fw_rename(PT_FILE, PT_FILE_BAD, msg, sizeof(msg));
      fw_log(msg);
#if USE_OLED
      pm_display_fwupdate(PM_OLED_Type, "UPD Failed!");
      sleep_ms(5000);
#endif
      rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
     }
     else {
      snprintf(msg, sizeof(msg), "Installed: %lu KB at 0x%lx%s", (unsigned long)(end / 1024), (unsigned long)slot.start,
               pt_new ? ", new partition table" : (pt_file ? ", partition table unchanged" : ""));
      PM_INFO("FW update: %s\n", msg);
      fw_rename(FW_FILE, FW_FILE_DONE, msg, sizeof(msg));
      if (pt_file) fw_rename(PT_FILE, PT_FILE_DONE, msg, sizeof(msg));
      fw_log(msg);
      rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, XIP_BASE + slot.start, 0);
     }
  for (;;) tight_loop_contents();
}
