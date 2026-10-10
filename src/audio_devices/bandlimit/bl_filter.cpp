#include <math.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pm_audio.h"
#include "bl_filter.h"

#if USE_LPF
#define BL_CUTOFF  0.46f       // -6dB point, fraction of the input rate (0.5 = Nyquist)
#define BL_BETA    7.0f        // Kaiser window : ~75dB rejection above half the input rate

static int16_t bl_kernel[BL_PHASES][BL_TAPS];   // Q15, each phase sums to 1.0
static bool    bl_ready = false;

static float bl_bessel_i0(float x) {
    float sum = 1.0f, term = 1.0f;
    for (int k = 1; k < 25; k++) {
        float f = x / (2.0f * k);
        term *= f * f;
        sum += term;
    }
    return sum;
}

void bl_init() {
    if (bl_ready) return;
    const float half = BL_TAPS / 2.0f;
    const float i0_beta = bl_bessel_i0(BL_BETA);
    float c[BL_TAPS];

    for (int p = 0; p < BL_PHASES; p++) {
        float frac = (float) p / BL_PHASES;
        float sum = 0.0f;
        for (int k = 0; k < BL_TAPS; k++) {
            float d = (float) (k - (BL_TAPS/2 - 1)) - frac;   // Distance to the output point, in input samples
            float x = 2.0f * BL_CUTOFF * d;
            float sinc = (x == 0.0f) ? 1.0f : sinf((float) M_PI * x) / ((float) M_PI * x);
            float r = d / half;
            float win = (r * r < 1.0f) ? bl_bessel_i0(BL_BETA * sqrtf(1.0f - r * r)) / i0_beta : 0.0f;
            c[k] = sinc * win;
            sum += c[k];
        }
        for (int k = 0; k < BL_TAPS; k++) bl_kernel[p][k] = (int16_t) lrintf(c[k] / sum * 32767.0f);
    }
    bl_ready = true;
}

#else
void bl_init() {}              // No SB16 mode, so no kernel
#endif

#if USE_LPF
volatile uint8_t bl_mode = BL_MODE_SB16;
#else
volatile uint8_t bl_mode = BL_MODE_NONE;
#endif
volatile bool bl_sbpro_filter = true;

// SB Pro 3.2KHz filter, 2nd order Butterworth at the output rate (49716Hz), Q24
static constexpr int64_t BL_BQ_B0 = 529406, BL_BQ_B1 = 1058811, BL_BQ_A1 = -24132934, BL_BQ_A2 = 9473341;
// Analog stage RC low-pass at 8KHz, the coefficients of the SB Pro FM output (opl_dbopl.cpp), Q15
static constexpr int32_t BL_RC_G = 15773, BL_RC_GB = 5321, BL_RC_P = 11674;

bool bl_set_mode(uint8_t mode) {
    if (mode > BL_MODE_ANALOG) return false;
#if !USE_LPF
    if (mode == BL_MODE_SB16) return false;
#endif
    bl_mode = mode;
    return true;
}

void bl_post_clear(bl_post_t *p) {
    p->bq_x1 = p->bq_x2 = p->bq_y1 = p->bq_y2 = 0;
    p->dc = p->dc_x1 = 0;
    p->lp_x1 = p->lp_y1 = 0;
}

void bl_reset(bl_state_t *s) {
    memset(s->hist, 0, sizeof(s->hist));
    s->w = 0;
    s->pos = 0;
    s->quiet = BL_TAPS + 1;
    bl_post_clear(&s->post);
}

#if USE_LPF
static int32_t __not_in_flash_func(bl_sb16)(const bl_state_t *s, uint32_t phase) {
    const int16_t *h = bl_kernel[phase];
    const int16_t *x = &s->hist[s->w];        // Oldest to newest sample
    int32_t acc = 0;
    for (int k = 0; k < BL_TAPS; k++) acc += h[k] * x[k];
    acc >>= 15;
    if (acc > 32767) acc = 32767;
    else if (acc < -32768) acc = -32768;
    return acc;
}
#endif

int32_t __not_in_flash_func(bl_post)(bl_post_t *p, int32_t x, uint8_t mode) {
    if (mode == BL_MODE_NONE) return x;

    if ((mode == BL_MODE_SBPRO2) && p->sb) {
        // The filter also runs when the program turns it off, so turning it on doesn't click
        int32_t x8 = x << 8;
        int32_t y8 = (int32_t) ((BL_BQ_B0 * (x8 + p->bq_x2) + BL_BQ_B1 * p->bq_x1
                                 - BL_BQ_A1 * p->bq_y1 - BL_BQ_A2 * p->bq_y2) >> 24);
        p->bq_x2 = p->bq_x1;
        p->bq_x1 = x8;
        p->bq_y2 = p->bq_y1;
        p->bq_y1 = y8;
        if (bl_sbpro_filter) x = y8 >> 8;
    }

    // Analog stage, the coupling capacitor (y = x - x1 + (1 - 2^-11) y1) then the RC low-pass
    p->dc += ((x - p->dc_x1) << 8) - (p->dc >> 11);
    p->dc_x1 = x;
    x = p->dc >> 8;
    int32_t y = (BL_RC_G * x + BL_RC_GB * p->lp_x1 + BL_RC_P * p->lp_y1 + (1 << 14)) >> 15;
    p->lp_x1 = x;
    p->lp_y1 = y;
    return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

int32_t __not_in_flash_func(bl_out)(bl_state_t *s, uint32_t phase) {
    const uint8_t mode = bl_mode;
#if USE_LPF
    if (mode == BL_MODE_SB16) return bl_sb16(s, phase);
#else
    (void) phase;
#endif
    return bl_post(&s->post, bl_newest(s), mode);
}

void __not_in_flash_func(bl_render_fifo8)(bl_state_t *s, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples) {
    if (rate > 48000) rate = 48000;
    const uint32_t step = (uint32_t) (((uint64_t) rate << 24) / PM_AUDIO_FREQUENCY);   // Input samples per output sample (Q24)

    for (uint32_t sn = 0; sn < samples * 2; sn += 2) {
        s->pos += step;
        while (s->pos >= (1u << 24)) {
            s->pos -= (1u << 24);
            int8_t raw;
            if (afifo_take_sample8(fifo, &raw)) bl_push(s, bl_sample8u((uint8_t) raw));
            else if (bl_is_quiet(s)) {         // Nothing to play and the filters are empty, no CPU used
                bl_post_clear(&s->post);
                return;
            }
            else bl_push(s, 0);                // FIFO empty : let the filter output fade out
        }
        int16_t v = (int16_t) bl_out(s, s->pos >> (24 - BL_PHASES_BITS));
        buff[sn]   += v;
        buff[sn+1] += v;
    }
}

void __not_in_flash_func(bl_render_fifo8s)(bl_state_t *l, bl_state_t *r, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples) {
    if (rate > 48000) rate = 48000;
    const uint32_t step = (uint32_t) (((uint64_t) rate << 24) / PM_AUDIO_FREQUENCY);   // Input samples per output sample (Q24)

    for (uint32_t sn = 0; sn < samples * 2; sn += 2) {
        l->pos += step;
        while (l->pos >= (1u << 24)) {
            l->pos -= (1u << 24);
            int8_t raw_l, raw_r;
            // Only whole pairs are taken, so a late left byte doesn't swap the channels
            if (((fifo->samples_received - fifo->samples_sent) >= 2) && afifo_take_sample8(fifo, &raw_r)) {
                afifo_take_sample8(fifo, &raw_l);
                bl_push(l, bl_sample8u((uint8_t) raw_l));
                bl_push(r, bl_sample8u((uint8_t) raw_r));
            } else if (bl_is_quiet(l) && bl_is_quiet(r)) {   // Nothing to play and the filters are empty
                bl_post_clear(&l->post);
                bl_post_clear(&r->post);
                return;
            } else {
                bl_push(l, 0);
                bl_push(r, 0);
            }
        }
        const uint32_t phase = l->pos >> (24 - BL_PHASES_BITS);
        buff[sn]   += (int16_t) bl_out(l, phase);
        buff[sn+1] += (int16_t) bl_out(r, phase);
    }
}
