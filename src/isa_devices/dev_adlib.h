#pragma once

#include "../dev_audiomix.h"

// The OPL chip has separately enabled AdLib ports (dev_adlib) and Sound Blaster FM ports
// (dev_sbopl). It runs while one of them is enabled.
typedef struct dev_adl_t {            
    bool     active;          // True if configured
    volatile uint8_t delay;   // counter for the Nb of second since last I/O
    uint16_t port;            // AdLib port, 388h by default
} dev_adl_t;

extern dev_adl_t dev_adlib;
extern volatile bool dev_sbopl_active;   // True if the SB FM ports are enabled

__force_inline void dev_adlib_enable_mix() {
  dev_audiomix.dev_active = dev_audiomix.dev_active | AD_ADLIB;
}

__force_inline void dev_adlib_disable_mix() {
  dev_audiomix.dev_active = dev_audiomix.dev_active & ~AD_ADLIB;
}

extern uint8_t dev_adlib_install(uint16_t port);
extern void dev_adlib_remove();
extern bool dev_adlib_installed();
extern void dev_sbopl_install();
extern void dev_sbopl_remove();
extern void dev_adlib_update();
extern bool dev_adlib_ior(uint32_t Addr,uint8_t *Data);
extern void dev_adlib_iow(uint32_t Addr,uint8_t Data);