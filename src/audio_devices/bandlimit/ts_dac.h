#pragma once
// Time stamped DAC for the DACs written by the CPU at its own rate, like the SB Direct DAC or the
// Covox. The mixer replays the DAC level at the real write times.

#include <stdint.h>
#include <stdbool.h>
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "bl_filter.h"

#define TSDAC_RING_SIZE  1024    // Power of 2, 23ms of writes at 44kHz
#define TSDAC_RING_MASK  (TSDAC_RING_SIZE-1)

typedef struct {
    uint32_t ts[TSDAC_RING_SIZE];   // Write time (time_us_32)
    uint8_t  val[TSDAC_RING_SIZE];  // 8-bit unsigned sample
    volatile uint32_t head;         // Only changed by the ISA core

    // Used only by the mixer on the main core
    bool     active;
    uint32_t tail;
    uint64_t cursor;      // Replay time, us << 16
    uint32_t out_step;    // Output sample duration, us << 16
    uint32_t last_ts;     // Last write time, us
    uint8_t  level;       // Current DAC value
    uint8_t  holds;       // Output sample counter of the end fade
    bl_post_t post;       // IIR filters
} ts_dac_t;

void ts_dac_init(ts_dac_t *d);

// Stores a sample written by the CPU. Must be called from the ISA core.
static inline void ts_dac_write(ts_dac_t *d, uint8_t value) {
    uint32_t h = d->head;
    d->ts[h & TSDAC_RING_MASK]  = time_us_32();
    d->val[h & TSDAC_RING_MASK] = value;
    __dmb();                    // Sample visible before the new head
    d->head = h + 1;
}

// True if the ts_dac_render() has something to do
static inline bool ts_dac_pending(const ts_dac_t *d) {
    return d->active || d->head != d->tail;
}

// Adds the DAC output to the buff (16-bit stereo interleaved)
void ts_dac_render(ts_dac_t *d, int16_t *buff, uint32_t samples);
