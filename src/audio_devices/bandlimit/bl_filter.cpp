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
void bl_init() {}              // Sample-and-hold : no kernel
#endif

void bl_reset(bl_state_t *s) {
    memset(s->hist, 0, sizeof(s->hist));
    s->w = 0;
    s->pos = 0;
    s->quiet = BL_TAPS + 1;
}

#if USE_LPF
int32_t __not_in_flash_func(bl_output)(const bl_state_t *s, uint32_t phase) {
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

void __not_in_flash_func(bl_render_fifo8)(bl_state_t *s, audio_fifo_8_t *fifo, uint32_t rate, int16_t *buff, uint32_t samples) {
    if (rate > 48000) rate = 48000;
    const uint32_t step = (uint32_t) (((uint64_t) rate << 24) / PM_AUDIO_FREQUENCY);   // Input samples per output sample (Q24)

    for (uint32_t sn = 0; sn < samples * 2; sn += 2) {
        s->pos += step;
        while (s->pos >= (1u << 24)) {
            s->pos -= (1u << 24);
            int8_t raw;
            if (afifo_take_sample8(fifo, &raw)) bl_push(s, bl_sample8u((uint8_t) raw));
            else if (bl_is_quiet(s)) return;   // Nothing to play and the filter is empty : no CPU used
            else bl_push(s, 0);                // FIFO empty : let the filter output fade out
        }
        int16_t v = (int16_t) bl_output(s, s->pos >> (24 - BL_PHASES_BITS));
        buff[sn]   += v;
        buff[sn+1] += v;
    }
}
