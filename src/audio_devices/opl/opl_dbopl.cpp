// DOSBox OPL core adaptation for PicoGUS
// mostly just copied from opl_ymfm.cpp

#include "opl.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "dbopl/dbopl.h"

#if OPL_CMD_BUFFER
#include "include/cmd_buffers.h"
extern cms_buffer_t opl_cmd_buffer;
#endif

#ifdef BOARD_PICOMEM
#include "pm_audio.h"
#endif

// ---------------------------------------------------------------------------
// Chip selection
// ---------------------------------------------------------------------------
#ifdef USE_YMF3812
static constexpr unsigned int OPL_STATUS_BASE = 0x06; // OPL2 detection
#else
static constexpr unsigned int OPL_STATUS_BASE = 0x00; // OPL3 detection
#endif

// ---------------------------------------------------------------------------
// Timer state — Core 0 only
// ---------------------------------------------------------------------------
typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} opl_timer_t;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125,  0, 0, 0 };

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    if (timer->enabled)
    {
        int tics = 0x100 - timer->value;
        timer->expire_time = time_us_64()
                           + ((uint64_t)tics * OPL_SECOND) / timer->rate;
    }
}

static DBOPL::Chip dbopl3(true);

// Temporary buffer as dbopl3 generates 32-bit samples, but we need to return 16-bit samples to the audio mixer.
static int32_t opl_buf32s[2*PM_SAMPLES_PER_BUFFER];   // interleaved stereo

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int OPL_Pico_Init(unsigned int port_base)
{
    dbopl3.Setup(49716);    // DBOPL renders at the chip rate. The mixer must run at the same rate.
#ifdef BOARD_PICOMEM
    static_assert(PM_AUDIO_FREQUENCY == 49716, "The OPL output is not resampled: PM_AUDIO_FREQUENCY must be 49716");
#endif
#if !BOARD_PICOMEM
    s_prebuf_head = PREBUF_SIZE;
#endif
    return 1;
}

#ifdef BOARD_PICOMEM
void OPL_Pico_Shutdown(void)
{

}
#endif

unsigned int OPL_Pico_PortRead(opl_port_t port)
{
    unsigned int result = OPL_STATUS_BASE;
    __dmb();
    uint64_t pico_time = time_us_64();

    if (timer1.enabled && pico_time > timer1.expire_time)
    {
        result |= 0x80 | 0x40;
    }
    if (timer2.enabled && pico_time > timer2.expire_time)
    {
        result |= 0x80 | 0x20;
    }

    return result;
}

void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value)
{
    // Writes to the second register array go to the first array while the OPL3 mode is off, as on
    // the chip. The 0x105 register itself is excluded.
    if ((reg_num & 0x100) && !dbopl3.opl3Active && (reg_num != OPL_REG_NEW))
        reg_num &= 0xFF;

    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }
                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }
            break;

        default:
            dbopl3.WriteReg(reg_num, value);
            break;
    }
}

// ---------------------------------------------------------------------------
// Analog output stage
// ---------------------------------------------------------------------------
// The DAC holds each sample, an RC low-pass filters it and a coupling capacitor removes the DC.
// OPL_FILTER selects the RC cutoff measured on real cards by DOSBox Staging : 0 none (SB16),
// 1 SB 1.x / 2.0 (12kHz), 2 SB Pro (8kHz).
#ifndef OPL_FILTER
#define OPL_FILTER 1
#endif
// 1 pole and 1 zero fitted to the RC and the sample-and-hold at 49716Hz

#if OPL_FILTER == 1
static constexpr int32_t OPL_LPF_G = 19476, OPL_LPF_GB = 6163, OPL_LPF_P = 7128;    // Q15
#elif OPL_FILTER == 2
static constexpr int32_t OPL_LPF_G = 15773, OPL_LPF_GB = 5321, OPL_LPF_P = 11674;   // Q15
#endif

typedef struct {
    int32_t x1;     // Previous input (clipped chip output)
    int32_t dc;     // DC blocker state, Q8
    int32_t lp_x1;  // Low-pass previous input
    int32_t lp_y1;  // Low-pass previous output
} opl_analog_t;

static opl_analog_t opl_analog[2];  // Left, Right

// Output gain, left and right (Q15). Set by the SB Pro mixer MIDI and Master volumes (sbdsp.cpp).
volatile int32_t opl_gain[2] = { 32768, 32768 };

static inline int32_t opl_clip16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static inline int32_t opl_analog_step(opl_analog_t *a, int32_t chip)
{
    int32_t x = opl_clip16(chip);                   // 16-bit chip output (DAC input)
#if OPL_FILTER
    a->dc += ((x - a->x1) << 8) - (a->dc >> 11);    // Coupling capacitor, y = x - x1 + (1 - 2^-11) y1
    a->x1 = x;
    x = a->dc >> 8;
    int32_t y = (OPL_LPF_G * x + OPL_LPF_GB * a->lp_x1 + OPL_LPF_P * a->lp_y1 + (1 << 14)) >> 15;
    a->lp_x1 = x;
    a->lp_y1 = y;
    return y;
#else
    return x;
#endif
}

// Mono interface — kept for compatibility with non-ymfm OPL backends.
// Returns the average of L+R so mono output is at the same level as stereo.
void OPL_Pico_simple16(int16_t *buffer, uint32_t nsamples)
{
    int32_t *b =opl_buf32s;
    dbopl3.GenerateBlock3(nsamples,b);

    for (uint32_t i = 0; i < nsamples; i++)
    {
        buffer[i] = (int16_t) opl_clip16((b[2*i] + b[2*i+1]) >> 1);
    }
}

// Adds to the stereo buffer because other devices (MPU) may already be in it
void OPL_Pico_stereo_int16(int16_t  *buffer, uint32_t nsamples)
{
    int32_t *b =opl_buf32s;
    dbopl3.GenerateBlock3(nsamples,b);
    const int32_t gl = opl_gain[0], gr = opl_gain[1];

    for (uint32_t i = 0; i < nsamples*2; i+=2)
    {
      buffer[i]   = (int16_t) opl_clip16(buffer[i]   + ((opl_analog_step(&opl_analog[0], b[i])   * gl) >> 15));
      buffer[i+1] = (int16_t) opl_clip16(buffer[i+1] + ((opl_analog_step(&opl_analog[1], b[i+1]) * gr) >> 15));
    }
}
