#pragma once
// Minimal HTTP server for the SD card. The page is embedded in the firmware.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the http.txt and starts the server. Must be called AFTER the Wifi init.
bool pm_httpd_init(void);
// Serves the pending requests. Must be called from the main loop.
void pm_httpd_task(void);

#ifdef __cplusplus
}
#endif
