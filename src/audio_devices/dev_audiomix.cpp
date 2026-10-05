/* PicoMEM Sound Cards mixer used to mix all the different sources.  */

#include <stdio.h>
#include <math.h>
#include <string.h>  // For memset

#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "pico_pic.h"
#endif

#include "pico/stdlib.h"
#include "pm_audio.h"
#include "dev_audiomix.h"

#include "opl.h"

#define A_DEBUG_PIN 0

#include "square/square.h"
extern cms_t *cms;

extern tandysound_t *tandysound;   // Tandy Emulation object

extern void* tsfrenderer;

extern void dev_mpu_getbuffer(int32_t *buffer, int samples);
extern void sbdsp_updatebuffer(int16_t* buff,uint32_t samples);
extern void dev_psg_getbuffer(int32_t* buff,uint8_t samples);
void cvx_updatebuffer(int16_t* buff,uint32_t samples);
extern void gus_updatebuffer(int16_t* buff,uint32_t samples);

//extern "C" void OPL_Pico_Mix_callback(audio_buffer_t *);

dev_audiomix_t dev_audiomix;
uint8_t *buffer;

typedef struct pma_status
 {
   bool active;
   bool mix_inprogress;
 } pma_status_t;

pma_status_t PM_Audio;

int64_t opl_TotalTime=0;
uint32_t opl_bufferCount=0;
int64_t starttime;

// Gentle one-pole (RC-style) low-pass "de-harshening" filter applied to the
// Covox output only. Covox resamples onto the output rate with simple
// nearest-sample (zero-order-hold) conversion, which is cheap but leaves harsh,
// spiky aliasing. (The SB DSP has its own SB16 style band-limited resampling,
// and like on a real SB16 the other sources, OPL included, are not filtered.)
// A single-pole filter has a gentle -6dB/octave slope, so picking a cutoff
// above the of game music/SFX (~10kHz) just takes the edge off without muffling
// the mix.
#define AUDIO_LPF_CUTOFF_HZ 10000

static int32_t audio_lpf_state_l = 0;
static int32_t audio_lpf_state_r = 0;
static int32_t audio_lpf_alpha_q15 = 32768;  // Q15 fixed point, 1.0 = no filtering (set properly in dev_audiomix_start)

static void audio_lpf_init() {
    double rc = 1.0 / (2.0 * M_PI * (double) AUDIO_LPF_CUTOFF_HZ);
    double dt = 1.0 / (double) PM_AUDIO_FREQUENCY;
    double alpha = dt / (rc + dt);
    audio_lpf_alpha_q15 = (int32_t) (alpha * 32768.0 + 0.5);
    audio_lpf_state_l = 0;
    audio_lpf_state_r = 0;
}

static __force_inline void audio_lpf_apply(int16_t *buff, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        audio_lpf_state_l += ((buff[i*2]   - audio_lpf_state_l) * audio_lpf_alpha_q15) >> 15;
        audio_lpf_state_r += ((buff[i*2+1] - audio_lpf_state_r) * audio_lpf_alpha_q15) >> 15;
        buff[i*2]   = (int16_t) audio_lpf_state_l;
        buff[i*2+1] = (int16_t) audio_lpf_state_r;
    }
}

void oplperf_start()
{
  starttime = time_us_64();
}

void oplperf_end()
{
  opl_bufferCount++;
  int64_t delta= time_us_64()-starttime;
  opl_TotalTime += delta;
  if (opl_bufferCount==1000)
   {
     PM_INFO("OPL- 1/1k: %d/%duS CPU %%: %d\n",(int)delta, (int)opl_TotalTime,(int)(opl_TotalTime/12873));  // calculated for a 49KHz mixing
     opl_TotalTime=0;
     opl_bufferCount=0;
   }
}


void pm_audio_domix() {

if (!dev_audiomix.enabled) return;
DBG_ON_MIX();
dev_audiomix.mix_inprogress=true;
for (int i=0;i<4;i++)   // Try to mix 4 Buffers before giving some time for the rest
    {
       buffer = take_empty_buffer();
       if (buffer!=NULL)
       {
         memset(buffer,0,PM_SAMPLES_PER_BUFFER*PM_SAMPLES_STRIDE);  // Clean the buffer in advanced 2*16bit*PM_SAMPLES_PER_BUFFER

#if USE_MPU
         if (dev_audiomix.dev_active & AD_MPU) {
             dev_mpu_getbuffer((int32_t*) buffer, PM_SAMPLES_PER_BUFFER);
             }  // MPU (Various rendering)
#endif

         if (dev_audiomix.dev_active & AD_ADLIB) {
            // Fill int32_t *buffer as int32 for 2x16Bit stereo
            oplperf_start();
            OPL_Pico_stereo_int16((int16_t*) buffer,PM_SAMPLES_PER_BUFFER); // interleave, 16Bit
            oplperf_end();
            }  // Adlib

#if USE_SBDSP
         if (dev_audiomix.dev_active & AD_SBDSP) {
             sbdsp_updatebuffer((int16_t*) buffer,PM_SAMPLES_PER_BUFFER);
            }  // Sound Blaster DSP
#endif

         if (dev_audiomix.dev_active & AD_CMS) {
             cms->generator(0).generate_frames((int16_t *) buffer, PM_SAMPLES_PER_BUFFER);
             cms->generator(1).generate_frames((int16_t *) buffer, PM_SAMPLES_PER_BUFFER);
            }  // CMS

         if (dev_audiomix.dev_active & AD_TDY) {
             tandysound->generator().generate_frames((int16_t *) buffer, PM_SAMPLES_PER_BUFFER);
            }  //Tandy (mono)

         if (dev_audiomix.dev_active & AD_MMB) {
             dev_psg_getbuffer((int32_t *) buffer,PM_SAMPLES_PER_BUFFER);
            }  //mindscape music board (Stereo)

         if (dev_audiomix.dev_active & AD_CVX) {
             int16_t cvx_buffer[PM_SAMPLES_PER_BUFFER*2];
             memset(cvx_buffer,0,sizeof(cvx_buffer));
             cvx_updatebuffer(cvx_buffer,PM_SAMPLES_PER_BUFFER);
             audio_lpf_apply(cvx_buffer, PM_SAMPLES_PER_BUFFER);   // Soften the harsh Covox resampling edge
             for (int n=0;n<PM_SAMPLES_PER_BUFFER*2;n++) ((int16_t*) buffer)[n] += cvx_buffer[n];
            }  // Covox (mono)

#if USE_GUS
         if (dev_audiomix.dev_active & AD_GUS) {
            gus_updatebuffer((int16_t*) buffer,PM_SAMPLES_PER_BUFFER);
           }  // Gravis Ultrasound
#endif

         give_mixed_buffer();
       } else break; // Return directly if no buffer to mix

    }  // for loop
dev_audiomix.mix_inprogress=false;
DBG_OFF_MIX();
}

void dev_audiomix_start() {

if (!dev_audiomix.enabled)
 {
/*
printf("Timer IRQ Priority : %x %x %x %x\n",irq_get_priority(TIMER_IRQ_0),irq_get_priority(TIMER_IRQ_1)
                                         ,irq_get_priority(TIMER_IRQ_2),irq_get_priority(TIMER_IRQ_3));
*/
   PM_INFO("dev_audiomix_start\n");

   dev_audiomix.enabled=pm_audio_init(0);      // Start the PicoMEM Audio output
   dev_audiomix.dev_active=0;                  // No audio device is mixing
   dev_audiomix.mix_inprogress=false;          // No audio mixing in progress
   audio_lpf_init();                           // (Re)compute the output de-harshening filter for PM_AUDIO_FREQUENCY

   PIC_Init();                                 // Audio devices need the PIC
 }
}

void dev_audiomix_stop()
{
if (dev_audiomix.enabled)
 {
  PM_INFO("dev_audiomix_stop\n");
  pm_audio_stop();
  dev_audiomix.dev_active=0;         // No audio device is mixing
 }
}

bool dev_audiomix_isactive()
{
   return dev_audiomix.enabled;
}
