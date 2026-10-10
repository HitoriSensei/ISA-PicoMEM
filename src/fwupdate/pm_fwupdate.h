#pragma once
// Firmware update from the SD card (PicoMEM 2.0 / RP2350 only).
//
// Put "picomem.uf2" (the normal build output) at the root of the SD card and power on:
// it is written into the free slot of the A/B partition table and the PicoMEM restarts on it.
// The file is then renamed "picomem.uf2.done" ("picomem.uf2.bad" if it was refused). The result
// files of a previous update are deleted first: if one can't be deleted, nothing is done and
// fwupdate.txt says why.
// The new firmware is "Try Before You Buy": if it does not reach pm_fwupdate_boot(), the
// bootrom restarts the previous one.
//
// Needs the partition table installed once: partition.uf2, built next to picomem.uf2 from
// fwupdate/pm_partitions.json, copied in BOOTSEL mode.
// Without it nothing is done. A new table can then also be installed from the SD card: put
// partition.uf2 next to picomem.uf2 (renamed .done / .bad as well).

#ifdef __cplusplus
extern "C" {
#endif

void pm_fwupdate_boot(void);    // Call first in main(): confirms a newly installed firmware
void pm_fwupdate_check(void);   // Call after the config is loaded: installs picomem.uf2 (reboots)
bool pm_fwupdate_refused(void); // The update was refused (the PicoMEM starts normally)

#ifdef __cplusplus
}
#endif
