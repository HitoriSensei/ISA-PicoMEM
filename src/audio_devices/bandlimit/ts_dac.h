#pragma once
// Time stamped DAC : for the DACs written by the CPU at its own (unknown, possibly irregular)
// rate, like the SB Direct DAC (command 0x10) or the Covox.
// Each write is time stamped by the ISA core. The mixer replays the DAC level at the real write
// times, TSDAC_LATENCY_US behind. Each output sample is the mean level over its duration, then
// goes through the IIR filters of the mode (bl_dac_mode in bl_filter.h, the analog stage in the
// SB16 mode). Nothing runs (no timer) when the DAC is not written.

#include <stdint.h>
#include <stdbool.h>
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "bl_filter.h"

#define TSDAC_RING_SIZE  1024    // Power of 2 : 23ms of samples written at 44KHz
#define TSDAC_RING_MASK  (TSDAC_RING_SIZE-1)

typedef struct {
    uint32_t ts[TSDAC_RING_SIZE];   // Write time (time_us_32)
    uint8_t  val[TSDAC_RING_SIZE];  // 8 bit unsigned sample
    volatile uint32_t head;         // Only changed by the ISA core

    // Mixer (main core) only
    bool     active;
    uint32_t tail;
    uint64_t cursor;      // Replay time, us << 16
    uint32_t out_step;    // Output sample duration, us << 16
    uint32_t last_ts;     // Last write time, us
    uint8_t  level;       // Current DAC value
    uint8_t  holds;       // Output samples counter of the end of sound fade
    bl_post_t post;       // IIR filters
} ts_dac_t;

void ts_dac_init(ts_dac_t *d);

// ISA core : the CPU wrote a sample
static inline void ts_dac_write(ts_dac_t *d, uint8_t value) {
    uint32_t h = d->head;
    d->ts[h & TSDAC_RING_MASK]  = time_us_32();
    d->val[h & TSDAC_RING_MASK] = value;
    __dmb();                    // Sample visible before the new head
    d->head = h + 1;
}

// Mixer : true if ts_dac_render has something to do (only a compare when the DAC is not used)
static inline bool ts_dac_pending(const ts_dac_t *d) {
    return d->active || d->head != d->tail;
}

// Mixer : add the DAC output to buff (16 bit stereo interleaved)
void ts_dac_render(ts_dac_t *d, int16_t *buff, uint32_t samples);
