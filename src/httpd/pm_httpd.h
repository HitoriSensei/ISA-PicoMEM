#pragma once
// Minimal HTTP server giving access to the SD card (browse / download / upload / delete / rename / mkdir).
// Open http://<ip>/ in a browser: the page is embedded in the firmware.
//
// Configuration : "http.txt" at the root of the SD card, one value per line
//   1: IP address    (ex: 192.168.1.50)
//   2: Network mask  (ex: 255.255.255.0)
//   3: Gateway       (ex: 192.168.1.1)
//   4: Password      (optional, if missing anybody can use it)
// Lines starting with # and empty lines are ignored.
// Without http.txt nothing is started. A static IP is used because the NE2000 shares the Wifi MAC.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pm_httpd_init(void);   // Read http.txt and start the server (after the Wifi init)
void pm_httpd_task(void);   // Call from the main loop

#ifdef __cplusplus
}
#endif
