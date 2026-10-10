#pragma once
// Output filter of the sampled sources (SB DMA, SB Direct DAC, Covox, Disney), run at the output
// rate. PMINIT /lpf chooses the mode at run time (bl_set_mode).
//
// BL_MODE_NONE holds each input sample until the next one, with no filter.
//
// BL_MODE_SB16 is the SB16 "brickwall" filter, a band-limited (windowed sinc) resampling. The
// SB16 cuts its output just below half the sample rate, and the cutoff moves with the rate
// (nothing above 5.5KHz at 11KHz, nothing above 11KHz at 22KHz). It is emulated like the DOSBox
// Staging "sb16" filter, see https://www.vogons.org/viewtopic.php?t=49802
// The kernel is defined in input samples, so the same table gives a cutoff at BL_CUTOFF x the
// input rate for every rate, with no recomputation when the rate changes.
//
// BL_MODE_SBPRO2 is the SB Pro 2. The samples are held, go through the CT1345 mixer output
// low-pass filter (3.2KHz, 2nd order), then through the card analog stage. Programs turn the
// 3.2KHz filter on and off with bit 5 of mixer register 0Eh (on by default). It is only on
// the Sound Blaster output.
//
// BL_MODE_ANALOG holds the samples and only applies the card analog stage.
//
// The DACs written by the CPU (SB Direct DAC, Covox, ts_dac.h) are never band-limited. Their
// writes come at irregular times, and the SB16 filter needs evenly spaced samples. They get the
// analog stage in the SB16 mode.
//
// The analog stage is the one of the SB Pro FM output (opl_dbopl.cpp), an 8KHz RC low-pass and
// the coupling capacitor that removes the DC.
//
// Without USE_LPF (build flag, 1 for the PicoMEM 2) there is no SB16 mode and no kernel table in
// RAM, and the default mode is BL_MODE_NONE.

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

#define BL_MODE_NONE   0
#define BL_MODE_SB16   1
#define BL_MODE_SBPRO2 2
#define BL_MODE_ANALOG 3

// IIR filters run at the output rate (SB Pro filter and analog stage)
typedef struct {
    bool     sb;               // Sound Blaster output, where the SB Pro filter applies (kept by bl_post_clear)
    int32_t  bq_x1, bq_x2;     // SB Pro 3.2KHz filter, previous inputs and outputs (Q8)
    int32_t  bq_y1, bq_y2;
    int32_t  dc, dc_x1;        // Analog stage DC blocker (Q8) and its previous input
    int32_t  lp_x1, lp_y1;     // Analog stage RC low-pass, previous input and output
} bl_post_t;

typedef struct {
    int16_t  hist[BL_TAPS*2];  // Last input samples, stored twice : no wrap in the filter loop
    uint32_t w;                // Next write index (0..BL_TAPS-1)
    uint32_t pos;              // Fixed rate sources : output position between two input samples (Q24)
    uint32_t quiet;            // Nb of silent input samples in a row (> BL_TAPS : the filter output is 0)
    bl_post_t post;
} bl_state_t;

extern volatile uint8_t bl_mode;
extern volatile bool    bl_sbpro_filter;   // SB Pro mixer output filter switch, register 0Eh bit 5

void bl_init();                // Compute the kernel (once, can be called several times)
void bl_reset(bl_state_t *s);
bool bl_set_mode(uint8_t mode);   // False if the mode doesn't exist in this build

// Newest input sample (the sample-and-hold output)
static inline int32_t bl_newest(const bl_state_t *s) { return s->hist[s->w + BL_TAPS - 1]; }

// Output of the current mode, at phase (0..BL_PHASES-1) after the newest sample.
// Called once per output sample, in order (the IIR filters keep their state).
int32_t bl_out(bl_state_t *s, uint32_t phase);

// Mode of the DACs written by the CPU, the SB16 mode gives them the analog stage only
static inline uint8_t bl_dac_mode() {
    uint8_t mode = bl_mode;
    return (mode == BL_MODE_SB16) ? BL_MODE_ANALOG : mode;
}

// IIR filters of mode (not BL_MODE_SB16) applied to x, called once per output sample
int32_t bl_post(bl_post_t *p, int32_t x, uint8_t mode);

// True when the IIR filters of mode have nothing left to output
static inline bool bl_post_quiet(const bl_post_t *p, uint8_t mode) {
    if ((mode == BL_MODE_NONE) || (mode == BL_MODE_SB16)) return true;
    return (p->dc < 2048) && (p->dc > -2048) && (p->lp_y1 < 8) && (p->lp_y1 > -8)
        && (p->bq_y1 < 256) && (p->bq_y1 > -256) && (p->bq_y2 < 256) && (p->bq_y2 > -256);
}

// Clear the IIR filters (when the output stops)
void bl_post_clear(bl_post_t *p);

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

// The output is silent, with no input for BL_TAPS samples and nothing left in the IIR filters
static inline bool bl_is_quiet(const bl_state_t *s) { return s->quiet > BL_TAPS && bl_post_quiet(&s->post, bl_mode); }

// 8 bit unsigned sample to the PicoMEM mixing level : (u-128)*64
static inline int16_t bl_sample8u(uint8_t u) { return (int16_t) (((int16_t) u - 128) * 64); }

// Fixed rate source : read the 8 bit unsigned mono FIFO at "rate" and add the filtered
// samples to buff (16 bit stereo interleaved), at the output rate
void bl_render_fifo8(bl_state_t *s, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples);

// Same for an SB Pro stereo FIFO, "rate" is per channel. The SB Pro sends the first byte after
// stereo is switched on to the right channel, then alternates, so each input sample is a right
// then a left byte. Programs written for the real card expect it. Creative tells them to play one
// silent byte first (it goes right, the data then starts on the left), and the Apogee Sound System
// (Duke Nukem 3D) plays no silent byte and swaps its channels on the SB Pro instead.
void bl_render_fifo8s(bl_state_t *l, bl_state_t *r, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples);
