#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pm_audio.h"
#include "ts_dac.h"

#define TSDAC_LATENCY_US     10000    // Must be > the audio output queue (5 x 64 samples = 7.3ms)
#define TSDAC_MAX_LAG_US     (TSDAC_LATENCY_US+25000)  // Resync the replay if later than this
#define TSDAC_MIN_PERIOD_US  20       // Writes faster than 50KHz : only the newest value is kept
#define TSDAC_MAX_PERIOD_US  2000     // Slower than 500Hz : not a sample rate (pause)
#define TSDAC_STOP_US        200000   // Fade to silence and stop 200ms after the last write
#define TSDAC_DEFAULT_PERIOD ((uint32_t) (256 * 1000000ull / 11025))   // Until the rate is measured

void ts_dac_init(ts_dac_t *d) {
    memset(d, 0, sizeof(*d));
    bl_init();
    bl_reset(&d->bl);
}

// Duration of one output sample, from the real I2S rate (same divider computation as audio_i2s.c)
static uint32_t ts_dac_output_step() {
    uint32_t sys4 = clock_get_hz(clk_sys) * 4;
    uint32_t divider = sys4 / PM_AUDIO_FREQUENCY;
    return (uint32_t) (((1000000ull << 16) * divider) / sys4);
}

void __not_in_flash_func(ts_dac_render)(ts_dac_t *d, int16_t *buff, uint32_t samples) {
    uint32_t head = d->head;
    __dmb();
    uint32_t now = time_us_32();
    uint32_t tail = d->tail;

    if (head - tail > TSDAC_RING_SIZE) tail = head - TSDAC_RING_SIZE;   // Overrun (mixer was not running)

    if (!d->active) {
        // Drop the old writes (done when the mixer was not running)
        while (tail != head && (int32_t) (now - d->ts[tail & TSDAC_RING_MASK]) > 2 * TSDAC_LATENCY_US) tail++;
        d->tail = tail;
        if (tail == head) return;
        d->active      = true;
        d->out_step    = ts_dac_output_step();
        d->cursor      = (uint64_t) (now - TSDAC_LATENCY_US) << 16;
        d->anchor      = (now - TSDAC_LATENCY_US) << 8;
        d->period      = TSDAC_DEFAULT_PERIOD;
        d->have_period = false;
        d->last_ts     = now - TSDAC_LATENCY_US;
        d->have_last   = false;
        d->level       = 0x80;
        d->holds       = 0;
        bl_reset(&d->bl);
    }

    // Keep the replay behind the writes : the whole buffer must be in the past
    int32_t lag = (int32_t) (now - (uint32_t) (d->cursor >> 16));
    int32_t buffer_us = (int32_t) (((uint64_t) samples * d->out_step) >> 16);
    if (lag < buffer_us || lag > TSDAC_MAX_LAG_US) {
        uint32_t t_us = now - TSDAC_LATENCY_US;
        d->cursor = (uint64_t) t_us << 16;
        d->anchor = t_us << 8;
        // Writes older than the new replay time are too late to be played : keep the last level only
        while (tail != head && (int32_t) (d->ts[tail & TSDAC_RING_MASK] - t_us) <= 0) {
            d->level = d->val[tail & TSDAC_RING_MASK];
            d->last_ts = d->ts[tail & TSDAC_RING_MASK];
            tail++;
        }
        d->have_last = false;
    }

    bool stop = false;
    for (uint32_t sn = 0; sn < samples * 2; sn += 2) {
        const uint32_t t = (uint32_t) (d->cursor >> 8);   // us << 8
        d->cursor += d->out_step;

        for (;;) {
            if (tail != head) {
                uint32_t ts_raw = d->ts[tail & TSDAC_RING_MASK];
                uint32_t ts = ts_raw << 8;
                if ((int32_t) (ts - t) <= 0) {     // Written before t : it is the next input sample
                    uint32_t dt = ts_raw - d->last_ts;
                    if (d->have_last && dt >= TSDAC_MIN_PERIOD_US && dt <= TSDAC_MAX_PERIOD_US) {   // Measure the write rate
                        if (d->have_period) d->period += (int32_t) ((dt << 8) - d->period) >> 5;
                        else { d->period = dt << 8; d->have_period = true; }
                    }
                    d->last_ts = ts_raw;
                    d->have_last = true;
                    d->level = d->val[tail & TSDAC_RING_MASK];
                    d->holds = 0;
                    tail++;
                    if ((int32_t) (ts - d->anchor) < (TSDAC_MIN_PERIOD_US << 8)) bl_replace_last(&d->bl, bl_sample8u(d->level));
                    else bl_push(&d->bl, bl_sample8u(d->level));
                    d->anchor = ts;
                    continue;
                }
                // The next write is known : just wait for it, unless the game paused
                if ((int32_t) (ts - d->anchor) < (int32_t) (d->period + (d->period >> 1))) break;
            }
            // No write : the DAC keeps its level, one input sample per period
            if ((int32_t) (t - d->anchor) < (int32_t) d->period) break;
            d->anchor += d->period;
            if ((int32_t) (t - (d->last_ts << 8)) > (int32_t) (TSDAC_STOP_US << 8)) {
                // End of the sound : slowly back to 0x80 (no click), then stop
                if ((++d->holds & 3) == 0) {
                    if (d->level > 0x80) d->level--;
                    else if (d->level < 0x80) d->level++;
                }
                if (d->level == 0x80 && bl_is_quiet(&d->bl) && tail == head) stop = true;
            }
            bl_push(&d->bl, bl_sample8u(d->level));
            if ((int32_t) (t - d->anchor) > (int32_t) (TSDAC_MAX_PERIOD_US << 8) * 4) d->anchor = t;   // Safety : never loop long
        }

        uint32_t since = t - d->anchor;
        uint32_t phase = (since < d->period) ? (since << BL_PHASES_BITS) / d->period : BL_PHASES - 1;
        int16_t v = (int16_t) bl_output(&d->bl, phase);
        buff[sn]   += v;
        buff[sn+1] += v;
    }

    d->tail = tail;
    if (stop) d->active = false;
}
