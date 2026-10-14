#pragma once
// Output filter of the sampled sources (SB DMA, SB Direct DAC, Covox, Disney), run at the output rate.

#include <stdint.h>
#include "audio_fifo.h"

// Without USE_LPF there is no SB16 mode and no kernel table in RAM
#ifndef USE_LPF
#define USE_LPF 0
#endif

#if USE_LPF
#define BL_TAPS        48      // Kernel length, in input samples (adds BL_TAPS/2 samples of latency)
#else
#define BL_TAPS        1       // Sample-and-hold keeps only the newest sample
#endif
#define BL_PHASES_BITS 7       // 128 sub-sample positions of the output (nearest one is used)
#define BL_PHASES      (1 << BL_PHASES_BITS)

#define BL_MODE_NONE   0       // Each input sample is held until the next one
#define BL_MODE_SB16   1       // SB16 "brickwall" filter, a windowed sinc cutting just below half the sample rate
#define BL_MODE_SBPRO2 2       // SB Pro 2 mixer low-pass (3.2kHz, 2nd order) and the analog stage
#define BL_MODE_ANALOG 3       // Analog stage of the SB Pro FM output, an 8kHz RC low-pass and DC blocking

// IIR filters run at the output rate (SB Pro filter and analog stage)
typedef struct {
    bool     sb;               // Set for the Sound Blaster output, where the SB Pro filter applies
    int32_t  bq_x1, bq_x2;     // SB Pro 3.2kHz filter, previous inputs and outputs (Q8)
    int32_t  bq_y1, bq_y2;
    int32_t  dc, dc_x1;        // Analog stage DC blocker (Q8) and its previous input
    int32_t  lp_x1, lp_y1;     // Analog stage RC low-pass, previous input and output
} bl_post_t;

typedef struct {
    int16_t  hist[BL_TAPS*2];  // Last input samples, stored twice to avoid a wrap in the filter loop
    uint32_t w;                // Next write index (0..BL_TAPS-1)
    uint32_t pos;              // Output position between two input samples for fixed rate sources (Q24)
    uint32_t quiet;            // Number of silent input samples in a row
    bl_post_t post;
} bl_state_t;

extern volatile uint8_t bl_mode;
extern volatile bool    bl_sbpro_filter;   // SB Pro mixer output filter switch, register 0Eh bit 5

void bl_init();                // Computes the kernel once. Can be called several times.
void bl_reset(bl_state_t *s);
bool bl_set_mode(uint8_t mode);   // Sets the filter mode. False if the mode doesn't exist in this build.

// Newest input sample (the sample-and-hold output)
static inline int32_t bl_newest(const bl_state_t *s) { return s->hist[s->w + BL_TAPS - 1]; }

// Returns the output of the current mode at a phase (0..BL_PHASES-1) after the newest sample.
// Must be called once per output sample, in order.
int32_t bl_out(bl_state_t *s, uint32_t phase);

// Returns the mode for the DACs written by the CPU. They get the analog stage in the SB16 mode
// because their irregular writes can't be band-limited.
static inline uint8_t bl_dac_mode() {
    uint8_t mode = bl_mode;
    return (mode == BL_MODE_SB16) ? BL_MODE_ANALOG : mode;
}

// Applies the IIR filters of the mode to x. Must be called once per output sample.
int32_t bl_post(bl_post_t *p, int32_t x, uint8_t mode);

// True when the IIR filters of mode have nothing left to output
static inline bool bl_post_quiet(const bl_post_t *p, uint8_t mode) {
    if ((mode == BL_MODE_NONE) || (mode == BL_MODE_SB16)) return true;
    return (p->dc < 2048) && (p->dc > -2048) && (p->lp_y1 < 8) && (p->lp_y1 > -8)
        && (p->bq_y1 < 256) && (p->bq_y1 > -256) && (p->bq_y2 < 256) && (p->bq_y2 > -256);
}

// Clears the IIR filters
void bl_post_clear(bl_post_t *p);

static inline void bl_push(bl_state_t *s, int16_t sample) {
    s->hist[s->w] = sample;
    s->hist[s->w + BL_TAPS] = sample;
    if (++s->w == BL_TAPS) s->w = 0;
    if (sample) s->quiet = 0;
    else if (s->quiet <= BL_TAPS) s->quiet++;
}

// Replaces the newest sample when the input is faster than the filter can follow
static inline void bl_replace_last(bl_state_t *s, int16_t sample) {
    uint32_t i = s->w ? s->w - 1 : BL_TAPS - 1;
    s->hist[i] = sample;
    s->hist[i + BL_TAPS] = sample;
    if (sample) s->quiet = 0;
}

// True when the output is silent
static inline bool bl_is_quiet(const bl_state_t *s) { return s->quiet > BL_TAPS && bl_post_quiet(&s->post, bl_mode); }

// Converts an 8-bit unsigned sample to the PicoMEM mixing level
static inline int16_t bl_sample8u(uint8_t u) { return (int16_t) (((int16_t) u - 128) * 64); }

// Reads the 8-bit unsigned mono FIFO at the rate and adds the filtered samples to the buff
// (16-bit stereo interleaved)
void bl_render_fifo8(bl_state_t *s, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples);

// Same for an SB Pro stereo FIFO. The rate is per channel.
void bl_render_fifo8s(bl_state_t *l, bl_state_t *r, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples);
