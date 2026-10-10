#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pm_audio.h"
#include "ts_dac.h"

#define TSDAC_LATENCY_US     10000    // Must be > the audio output queue (5 x 64 samples = 7.3ms)
#define TSDAC_MAX_LAG_US     (TSDAC_LATENCY_US+25000)  // Resync the replay if later than this
#define TSDAC_STOP_US        200000   // Fade to silence and stop 200ms after the last write

void ts_dac_init(ts_dac_t *d) {
    memset(d, 0, sizeof(*d));
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
        d->active   = true;
        d->out_step = ts_dac_output_step();
        d->cursor   = (uint64_t) (now - TSDAC_LATENCY_US) << 16;
        d->last_ts  = now - TSDAC_LATENCY_US;
        d->level    = 0x80;
        d->holds    = 0;
        bl_post_clear(&d->post);
    }

    // Keep the replay behind the writes, the whole buffer must be in the past
    int32_t lag = (int32_t) (now - (uint32_t) (d->cursor >> 16));
    int32_t buffer_us = (int32_t) (((uint64_t) samples * d->out_step) >> 16);
    if (lag < buffer_us || lag > TSDAC_MAX_LAG_US) {
        uint32_t t_us = now - TSDAC_LATENCY_US;
        d->cursor = (uint64_t) t_us << 16;
        // Writes older than the new replay time are too late to be played, only the last level is kept
        while (tail != head && (int32_t) (d->ts[tail & TSDAC_RING_MASK] - t_us) <= 0) {
            d->level = d->val[tail & TSDAC_RING_MASK];
            d->last_ts = d->ts[tail & TSDAC_RING_MASK];
            tail++;
        }
    }

    const uint8_t mode = bl_dac_mode();
    bool stop = false;
    for (uint32_t sn = 0; sn < samples * 2; sn += 2) {
        const uint32_t t0 = (uint32_t) (d->cursor >> 8);   // Output sample start and end, us << 8 (wraps)
        d->cursor += d->out_step;
        const uint32_t t1 = (uint32_t) (d->cursor >> 8);

        // Mean DAC level over the output sample, each value weighted by the time it is held
        int32_t x = bl_sample8u(d->level);
        if (tail != head && (int32_t) ((d->ts[tail & TSDAC_RING_MASK] << 8) - t1) < 0) {
            int32_t acc = 0;
            uint32_t tc = t0;
            do {
                const uint32_t ts = d->ts[tail & TSDAC_RING_MASK] << 8;
                if ((int32_t) (ts - tc) > 0) {
                    acc += x * (int32_t) (ts - tc);
                    tc = ts;
                }
                d->level = d->val[tail & TSDAC_RING_MASK];
                d->last_ts = d->ts[tail & TSDAC_RING_MASK];
                d->holds = 0;
                x = bl_sample8u(d->level);
                tail++;
            } while (tail != head && (int32_t) ((d->ts[tail & TSDAC_RING_MASK] << 8) - t1) < 0);
            acc += x * (int32_t) (t1 - tc);
            x = acc / (int32_t) (t1 - t0);
        } else if ((int32_t) (t0 - (d->last_ts << 8)) > (int32_t) (TSDAC_STOP_US << 8)) {
            // End of the sound, slowly back to 0x80 (no click) in about 40ms, then stop
            if ((++d->holds & 15) == 0) {
                if (d->level > 0x80) d->level--;
                else if (d->level < 0x80) d->level++;
            }
            if (d->level == 0x80 && tail == head && bl_post_quiet(&d->post, mode)) stop = true;
        }

        int16_t v = (int16_t) bl_post(&d->post, x, mode);
        buff[sn]   += v;
        buff[sn+1] += v;
    }

    d->tail = tail;
    if (stop) {
        d->active = false;
        bl_post_clear(&d->post);
    }
}
