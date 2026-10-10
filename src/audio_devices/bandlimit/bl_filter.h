#pragma once
// SB16 style "brickwall" filter : band-limited (windowed sinc) resampling to the output rate.
// The SB16 band-limits its output just below half the sample rate, and the cutoff moves
// with the rate (11KHz : nothing above 5.5KHz, 22KHz : nothing above 11KHz).
// See https://www.vogons.org/viewtopic.php?t=49802 (emulated like the DOSBox Staging "sb16" filter)
// The kernel is defined in input samples : the same table gives a cutoff at BL_CUTOFF x the
// input rate for every rate, no recomputation when the rate changes.

// USE_LPF (build flag, default 0, 1 for the PicoMEM 2) : 0 replaces the filter by a simple
// sample-and-hold (each input sample is repeated until the next one), no kernel table in RAM.

#include <stdint.h>
#include "audio_fifo.h"

#ifndef USE_LPF
#define USE_LPF 0
#endif

#if USE_LPF
#define BL_TAPS        48      // Kernel length, in input samples (adds BL_TAPS/2 samples of latency)
#else
#define BL_TAPS        1       // Sample-and-hold : only the newest sample
#endif
#define BL_PHASES_BITS 7       // 128 sub-sample positions of the output (nearest one is used)
#define BL_PHASES      (1 << BL_PHASES_BITS)

typedef struct {
    int16_t  hist[BL_TAPS*2];  // Last input samples, stored twice : no wrap in the filter loop
    uint32_t w;                // Next write index (0..BL_TAPS-1)
    uint32_t pos;              // Fixed rate sources : output position between two input samples (Q24)
    uint32_t quiet;            // Nb of silent input samples in a row (> BL_TAPS : the filter output is 0)
} bl_state_t;

void bl_init();                // Compute the kernel (once, can be called several times)
void bl_reset(bl_state_t *s);
#if USE_LPF
int32_t bl_output(const bl_state_t *s, uint32_t phase);   // Filter output at phase (0..BL_PHASES-1) after the newest sample
#else
static inline int32_t bl_output(const bl_state_t *s, uint32_t phase) { (void) phase; return s->hist[s->w]; }  // Newest sample
#endif

static inline void bl_push(bl_state_t *s, int16_t sample) {
    s->hist[s->w] = sample;
    s->hist[s->w + BL_TAPS] = sample;
    if (++s->w == BL_TAPS) s->w = 0;
    if (sample) s->quiet = 0;
    else if (s->quiet <= BL_TAPS) s->quiet++;
}

// Change the newest sample (used when the input is faster than the filter can follow)
static inline void bl_replace_last(bl_state_t *s, int16_t sample) {
    uint32_t i = s->w ? s->w - 1 : BL_TAPS - 1;
    s->hist[i] = sample;
    s->hist[i + BL_TAPS] = sample;
    if (sample) s->quiet = 0;
}

static inline bool bl_is_quiet(const bl_state_t *s) { return s->quiet > BL_TAPS; }

// 8 bit unsigned sample to the PicoMEM mixing level : (u-128)*64
static inline int16_t bl_sample8u(uint8_t u) { return (int16_t) (((int16_t) u - 128) * 64); }

// Fixed rate source : read the 8 bit unsigned mono FIFO at "rate" and add the filtered
// samples to buff (16 bit stereo interleaved), at the output rate
void bl_render_fifo8(bl_state_t *s, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples);
