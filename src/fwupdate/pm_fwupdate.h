#pragma once
// Firmware update from the SD card for the PicoMEM 2.0. It installs the picomem.uf2 from the root
// of the SD card into the free slot of the A/B partition table.

#ifdef __cplusplus
extern "C" {
#endif

// Confirms a newly installed firmware. Must be called FIRST in main().
void pm_fwupdate_boot(void);
// Installs the picomem.uf2 from the SD card and reboots. Must be called AFTER the config is loaded.
void pm_fwupdate_check(void);
// True when the picomem.uf2 was refused.
bool pm_fwupdate_refused(void);

#ifdef __cplusplus
}
#endif
