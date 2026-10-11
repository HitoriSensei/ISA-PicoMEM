/*
Title  : SoundBlaster DSP Emulation
Date   : 2023-12-30
Author : Kevin Moonlight <me@yyzkevin.com> modified for the PicoMEM by Freddy VETELE (07/2025)
*/

/*
To test : Wolf3D, Duke Nukem 2 (ADPCM), DUNE, DUNE 2, RAPTOR, DOOM, Terminal Velocity
          GODS, Heart of China, Wacky Wheels, Day of the tentacle, XCOM,

XT : Mod Master XT, Prince of Persia, Ghostbusters 2, Tongue of the Fatman, 4D Sports Boxing
*/
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pico_pic.h"
#include "bandlimit/bl_filter.h"
#include "bandlimit/ts_dac.h"

bool dma_inprogress=false;
uint32_t PM_DMA_SendTime;

#include "dev_audiomix.h"
#include "../pm_gvars.h"
#include "pm_defines.h"
#include "pm_audio.h"
#include "../../pm_libs/isa_dma.h"       // ISA/PC DMA code
#include "../../pm_libs/bus_irq.h"       // ISA/PC IRQ code
#include "dev_sbdsp.h"
#include "sbdsp_iolog.h"
#include "audio_fifo.h"
#if USE_HWDMA
#include "../../pm_libs/isa_irq.h"
#include "../../pm_libs/pm_pccmd.h"
#endif

#include "sbdsp.h"

#define OUTPUT_SAMPLERATE   49716ul

//#define DSP_VERSION_MAJOR 2
//#define DSP_VERSION_MINOR 1

#define DSP_UNUSED_STATUS_BITS_PULLED_HIGH 0x7F

// SB Pro (CT1345) mixer registers
#define MIXER_RESET             0x00
#define MIXER_VOICE             0x04
#define MIXER_MIC               0x0A
#define MIXER_INPUT             0x0C
#define MIXER_STEREO            0x0E    // Bit 1 stereo, bit 5 output filter off
#define MIXER_MASTER            0x22
#define MIXER_FM                0x26
#define MIXER_CD                0x28
#define MIXER_LINE              0x2E

static uint8_t mixer_state[0x30];       // SB Pro mixer registers, 00h-2Fh
static int32_t sbpro_voice_gain[2];     // Voice x Master, left and right (Q15)
static bool    sbpro_gain_on;           // A voice gain is not 1
extern volatile int32_t opl_gain[2];    // FM output gain (opl_dbopl.cpp)

static const int8_t DSP_VERSION_MAJOR[4] = { 0,2,3,4 };
static const int8_t DSP_VERSION_MINOR[4] = { 0,1,2,5 };

enum DSP_MODES {
	MODE_NONE,
	MODE_RESET,
	MODE_DAC,
	MODE_DMA,
	MODE_DMA_PAUSE,
	MODE_DMA_MASKED,
	MODE_DMA_REQUIRE_IRQ_ACK        /* Sound Blaster 16 style require IRQ ack to resume DMA */
};

enum DSP_OUT_MODES {
    DSP_DMA_2,         // 2-bit ADPCM
    DSP_DMA_3,         // 3-bit ADPCM
    DSP_DMA_4,         // 4-bit ADPCM
    DSP_DMA_8U,        // 8-bit PCM Unsigned
    DSP_DMA_8S,        // 8-bit PCM Unsigned
    DSP_DMA_8S_S,      // 8-bit PCM Unsigned
    DSP_DMA_16,        // 16-bit PCM (Always signed)
    DSP_DMA_16_S,      // 16-bit PCM Stereo(Always signed)
};

// Sound Blaster DSP I/O port offsets
#define MIXER_ADDRESS       0x4     // SB Pro mixer register index
#define MIXER_DATA          0x5
#define DSP_RESET           0x6
#define DSP_READ            0xA
#define DSP_WRITE           0xC
#define DSP_WRITE_STATUS    0xC
#define DSP_READ_STATUS     0xE

// Sound Blaster DSP commands.
#define DSP_STATUS              0x04    // 2.00+ : Read pending DSP operations status
#define DSP_DIRECT_DAC          0x10    // 1.xx+ :8-bit Output sample in direct mode
#define DSP_DIRECT_ADC          0x20    // 1.xx+ :8-bit Input sample in direct mode

#define DSP_DMA_ADC             0x24    // 1.xx+ :8-bit DMA PCM Input, Single

#define DSP_SET_TIME_CONSTANT   0x40    // 1.xx+ :Set digitized sound transfer constant
#define DSP_SET_DAC_SAMPLE_RATE 0x41    // ! 4.xx+ :Set sample Rate (Output)
#define DSP_SET_ADC_SAMPLE_RATE 0x42    // ! 4.xx+ :Set sample Rate (Input)

#define DSP_DMA_SINGLE          0x14    // 1.xx+ :8-bit DMA PCM Output, Single
#define DSP_DMA_SINGLE2         0x15    // 1.xx+ :8-bit DMA PCM Output, Single (Alternate)
#define DSP_DMA_SINGLE_IN       0x24    // !(N/A) 1.xx+ :8-bit DMA PCM Input, Single

#define DSP_DMA_ADPCM_2         0x16    // ! 1.xx+ :8-bit DMA ADPCM 2 bit Output, Single
#define DSP_DMA_ADPCM_2_REF     0x17    // ! 1.xx+ :8-bit DMA ADPCM 2 bit Output, Single  With Reference
#define DSP_DMA_ADPCM_4         0x74    // ! 1.xx+ :8-bit DMA ADPCM 4 bit Output, Single
#define DSP_DMA_ADPCM_4_REF     0x75    // ! 1.xx+ :8-bit DMA ADPCM 4 bit Output, Single with Reference
#define DSP_DMA_ADPCM_26        0x76    // ! 1.xx+ :8-bit DMA ADPCM 3 bit Output, Single
#define DSP_DMA_ADPCM_26_REF    0x77    // ! 1.xx+ :8-bit DMA ADPCM 3 bit Output, Single with Reference

#define DSP_DMA_BLOCK_SIZE      0x48    // 2.00+ : Set block size for highspeed/dma
#define DSP_DMA_AUTO            0X1C    // 2.00+ :8-bit DMA PCM Output, Auto
#define DSP_DMA_HS_AUTO         0x90    // 2.01+ :8-bit DMA PCM Output, Auto, High Speed
#define DSP_DMA_HS_SINGLE       0x91    // 2.01+ :8-bit DMA PCM Output, single, High Speed
#define DSP_DMA_HS_AUTO_IN      0x98    // !(N/A) 2.01+ :8-bit DMA PCM Input, Auto, High Speed
#define DSP_DMA_HS_SINGLE_IN    0x99    // !(N/A) 2.01+ :8-bit DMA PCM Input, Single, High Speed
#define DSP_DMA_ADPCM           0x7F    // creative ADPCM 8bit to 3bit
#define DSP_MIDI_READ_POLL      0x30    // 1.xx+ : MIDI Input Polling
#define DSP_MIDI_WRITE_POLL     0x38    // 1.xx+ : MIDI Output Polling

/*
 075h           DMA DAC, 4-bit ADPCM Reference                      SB
 076h           DMA DAC, 2.6-bit ADPCM                              SB
 077h           DMA DAC, 2.6-bit ADPCM Reference                    SB
 07Dh           Auto-Initialize DMA DAC, 4-bit ADPCM Reference      SB2.0
 07Fh           Auto-Initialize DMA DAC, 2.6-bit ADPCM Reference    SB2.0
 080h           Silence DAC                                         SB
 098h           Auto-Initialize DMA ADC, 8-bit (High Speed)         SB2.0-Pro2
*/

#define DSP_INPUT_MONO          0xA0    // 3.xx, SB Pro mono input mode
#define DSP_INPUT_STEREO        0xA8    // 3.xx, SB Pro stereo input mode

#define DSP_DMA_PAUSE           0xD0    // 1.xx+ : DMA Pause
#define DSP_DMA_RESUME          0xD4    // 1.xx+ : DMA Resume
#define DSP_DMA_PAUSE16         0xD5    // !4.xx+ : DMA 16-bit Pause
#define DSP_DMA_RESUME16        0xD6    // !4.xx+ : DMA 16-bit Resume
#define DSP_DMA_EXIT_AUTO8      0xDA    // 2.00+ : Exit Autoinit mode 8-bit
#define DSP_DAC_PAUSE_DURATION  0x80    // 1.xx+ : Pause Output (Used by Tryrian for detection)
#define DSP_ENABLE_SPEAKER      0xD1    // 1.xx+ : Turn On Speaker  - Ok
#define DSP_DISABLE_SPEAKER     0xD3    // 1.xx+ : Turn Off Speaker - Ok
#define DSP_SPEAKER_STATUS      0xD8    // 2.00+ : Get Speaker Status
#define DSP_IDENT               0xE0    // 2.00+ : Returns the complement of the inbox value
#define DSP_VERSION             0xE1    // 1.xx+ : Get DSP Version
#define DSP_IDENT_E2            0xE2    // 2.00+ : Returns the complement of the inbox value, alternate
#define DSP_WRITETEST           0xE4    // 2.00+ : Write to a diagnostic register
#define DSP_READTEST            0xE8    // 2.00+ : Read the diagnostic register
#define DSP_SINE                0xF0
#define DSP_IRQ                 0xF2    // 2.00+ : IRQ requeest, trigger a DSP interrupt
#define DSP_CHECKSUM            0xF4


#define DSP_MLC_NONE          0
#define DSP_MLC_DMA_RESET     2
#define DSP_MLC_DSP_RESET     3
#define DSP_MLC_SENDIRQ       4
#define DSP_MLC_SENDIRQ_WAIT  5
#define DSP_MLC_SET_TIME_CST  6      // Set Time Constant

static irq_handler_t SBDSP_DMA_isr_pt;

sbdsp_t sbdsp;
audio_fifo_8_t sbdsp_fifo; // FIFO for the DSP samples

static uint32_t DSP_DMA_EventHandler(Bitu val);
static PIC_TimerEvent DSP_DMA_Event = {
    .handler = DSP_DMA_EventHandler,
};
static void sbdsp_adpcm_decode_byte(uint8_t raw);

// Direct DAC (command 0x10) : games write each sample with the CPU, polling the DSP status
// (no IRQ, no DMA), at their own rate. Time stamped and band-limited to the measured rate (ts_dac.h)
static ts_dac_t sbdsp_dac;

// Output filter of the DMA samples (bl_filter.h), the left channel in stereo
static bl_state_t sbdsp_bl;
static bl_state_t sbdsp_bl_r;
static void sbmixer_init();

#if USE_HWDMA
// DSP command E2 answers with a DMA transfer to the memory. Creative's CT-VOICE.DRV (also embedded in
// X-Wing ROLAND_W.IMS/ADLIB_W.IMS...) points the DMA to its jump table and sends 2 E2 commands: the bytes
// unscramble the playback code, without them the game freezes at the first digitized sound.
// The board only sees the DMA cycles of the memory reads (DACK+IOW), and the PCI chipsets don't send the
// 8237 registers writes to the ISA bus, so the PC writes the answers: E2 raises the PicoMEM IRQ in PC
// command mode (the program waits in the BIOS), the main loop reads the channel current address and count,
// writes the bytes there and sets the registers as after a real transfer.
#define SBDSP_E2_QSIZE 8
static volatile uint8_t sbdsp_e2_q[SBDSP_E2_QSIZE];
static volatile uint32_t sbdsp_e2_head, sbdsp_e2_tail;  // Written by core 1 (I/O) / core 0 (main loop)

// E2 command (core 1): queue the answer and ask the PC to enter the PC command mode
static void sbdsp_e2_queue(uint8_t value) {
    if ((sbdsp.dma8!=1) && (sbdsp.dma8!=3)) return;  // The BIOS reads the channels 1 and 3 only
    // Needs the PicoMEM IRQ, on its own line: on the SB one, the program handler would take it as the SB IRQ
    if ((BV_IRQ==0) || (BV_IRQ==sbdsp.irq)) {
        IOLOG(IOLOG_E2,0xFFFF,BV_IRQ);       // Answer not written
        return;
       }
    uint32_t h=sbdsp_e2_head;
    if (h-sbdsp_e2_tail>=SBDSP_E2_QSIZE) return;
    sbdsp_e2_q[h%SBDSP_E2_QSIZE]=value;
    sbdsp_e2_head=h+1;
    isa_pm_irq_raise(IRQ_R_PCCMD,1,false);
}

// Main loop: once the BIOS waits for a command in the PicoMEM IRQ, write the queued answers
static void sbdsp_e2_send(void) {
    if (sbdsp_e2_tail==sbdsp_e2_head) return;
    if (!(SVAR_IRQ->ISR & (1<<IRQ_R_PCCMD)) || (PCCR_PCSTATE!=PCC_PCS_WAITCMD)) return;
    uint8_t page;
    uint16_t offset, count;
    PC_GetDMARegs(sbdsp.dma8,&page,&offset,&count);   // Current address and count
    uint32_t t=sbdsp_e2_tail;
    uint16_t n=0;
    while (t+n!=sbdsp_e2_head) {
        uint8_t value=sbdsp_e2_q[(t+n)%SBDSP_E2_QSIZE];
        PC_MW8(((uint32_t) page<<16)+(uint16_t) (offset+n),value);   // The address increments inside the 64KB page
        IOLOG(IOLOG_E2,(uint16_t) (offset+n),value);
        n++;
       }
    sbdsp_e2_tail=t+n;
    PC_SetDMARegs(sbdsp.dma8,(uint16_t) (offset+n),(uint16_t) (count-n),0);
    PC_End();
}
#endif

static __force_inline void sbdsp_dma_disable() {
    sbdsp.mode=MODE_NONE;
    sbdsp.dma_enabled=false;
   // sbdsp.vdma_flushdata = true; // Set the flush flag
    PIC_RemoveEvent(&DSP_DMA_Event);
    sbdsp.cur_sample8 = 0x80;   // zero current sample
    sbdsp.adpcm_code_index = 0;
}

static __force_inline void sbdsp_dma_enable() {
    if(!sbdsp.dma_enabled) {
        sbdsp.mode= MODE_DMA; // Set the mode to DMA
        sbdsp.dma_enabled=true;
        sbdsp.timer_delta=0;
#if USE_HWDMA
        isa_dma_set_isr(SBDSP_DMA_isr_pt);
#endif
   // PM_INFO("Start DMA w: %d",sbdsp.dma_rx_width);
        PIC_AddEvent(&DSP_DMA_Event, sbdsp.dma_interval, 0);
    } else { PM_INFO("SB> DMA Already Enabled"); }
}


static uint32_t __not_in_flash_func(DSP_DMA_EventHandler)(Bitu val) {
    uint32_t current_interval;
    uint32_t total_ticks;
    DBG_ON_1();
    sbdsp.dma_sample_count_rx++;
    PM_DMA_SendTime=time_us_64();

    // For ADPCM, a single raw DMA byte decodes into adpcm_codes_per_byte output
    // samples: only fetch a fresh byte once every adpcm_codes_per_byte ticks,
    // matching the real DSP's DMA request rate (one request per encoded byte,
    // not per decoded sample).
    if(sbdsp.adpcm_code_index==0) {
        dma_inprogress=true;
#if USE_HWDMA
        sbdsp.dma_rx_index=0;
        isa_dma_start_write();
#else
        sbdsp.vdma_to_send++;      // Ask one more byte to transfer
#endif
    }
 //    PM_INFO("X");

    if(sbdsp.adpcm_codes_per_byte>1) {
        sbdsp.adpcm_code_index++;
        if(sbdsp.adpcm_code_index>=sbdsp.adpcm_codes_per_byte) sbdsp.adpcm_code_index=0;
    }

    current_interval = sbdsp.dma_interval+sbdsp.timer_delta;
    // printf("%u\n", current_interval);

    total_ticks = (uint32_t)sbdsp.dma_sample_count * (sbdsp.adpcm_codes_per_byte ? sbdsp.adpcm_codes_per_byte : 1);

    DBG_OFF_1();
    if(sbdsp.dma_sample_count_rx <= total_ticks) {
        return current_interval;
    } else {
        if(sbdsp.autoinit) {
          //  sbdsp.vdma_flushdata = true; // Set the flush flag
#if USE_HWDMA
            bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);
#else
            sbdsp.mainloop_action=DSP_MLC_SENDIRQ;
#endif
            sbdsp.dma_sample_count_rx=0;
            sbdsp.adpcm_code_index=0;
            return current_interval;
        }
        else {
          //  sbdsp.vdma_flushdata = true; // Set the flush flag
            sbdsp_dma_disable();
#if USE_HWDMA
            bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);
#else
            sbdsp.mainloop_action=DSP_MLC_SENDIRQ;
#endif
         return 0;
        }
    }
    return 0;
}


static void __not_in_flash_func(sbdsp_dma_isr)(void) {
    const uint32_t dma_data = isa_dma_complete_write();
    if (sbdsp.adpcm_codes_per_byte>1) sbdsp_adpcm_decode_byte((uint8_t) dma_data);
    else afifo_add_sample8(&sbdsp_fifo, (uint8_t) dma_data);
//    sbdsp.dma_rx_index++;
//    if (sbdsp.dma_rx_index++!=sbdsp.dma_rx_width) isa_dma_start_write();
      sbdsp.dma_rx_index=0;
      if (++sbdsp.dma_rx_index!=1) isa_dma_start_write();
      else dma_inprogress=false;
}

// Send an IRQ after some time, for "Tinny" DMA transfer simulation
static uint32_t DSP_DMA_IRQ_EventHandler(Bitu val) {
  DBG_ON_IRQ_SB();
  bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);
  //sbdsp.mainloop_action=DSP_MLC_SENDIRQ;
  //PM_INFO("SB IRQ ");
  return 0;
}
static PIC_TimerEvent DSP_DMA_IRQ_Event = {
    .handler = DSP_DMA_IRQ_EventHandler,
};

// End of the DAC PAUSE (80h silence): the DSP generates an interrupt
static uint32_t DSP_DAC_Resume_eventHandler(Bitu val) {
  bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);
  //sbdsp.mainloop_action=DSP_MLC_SENDIRQ;
  sbdsp.dac_resume_pending = false;
  return 0;
}
static PIC_TimerEvent DSP_DAC_Resume_event = {
    .handler = DSP_DAC_Resume_eventHandler,
};


void sbdsp_init()
{

// Need to be reinitialized when the SB emulation is restarted
    sbdsp.dav_dsp=0;        // No DSP Data available
    sbdsp.dsp_busy=0;       // DSP Not busy
    sbdsp.mainloop_action=0;
	sbdsp.mode=MODE_NONE;

    sbdsp.dma_mode = DSP_DMA_8U;
    sbdsp.adpcm_ref = false;
    sbdsp.adpcm_need_ref = false;
    sbdsp.adpcm_codes_per_byte = 1;
    sbdsp.adpcm_code_index = 0;
    sbdsp.adpcm_reference = 0x80;
    sbdsp.adpcm_scale = 0;

    sbdsp.outbox = 0xAA;

    // Initialize buffer for the E2 command
    sbdsp.ident_e2[0] = 0xAA;
    sbdsp.ident_e2[1] = 0x96;


    afifo_init8(&sbdsp_fifo); // initialize the audio FIFO
    bl_init();                // Output filter
    bl_reset(&sbdsp_bl);
    sbdsp_bl.post.sb = true;
    ts_dac_init(&sbdsp_dac);  // Direct DAC
    sbdsp_dac.post.sb = true;
    bl_reset(&sbdsp_bl_r);    // Right channel of the SB Pro stereo
    sbdsp_bl_r.post.sb = true;
    sbdsp.dma_stereo = false;
    sbmixer_init();

#ifdef BOARD_PICOMEM
    PM_INFO("sbdsp_init\n");
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;
    //    PIC_Init();  // To remove when Audio mix is added !!!!

#else // PicoGUS
    puts("Initing ISA DMA ISR...");
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;
    dma_config = DMA_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);
#endif
}

// Number of decoded PCM samples produced from a single raw DMA byte for a given DSP_OUT_MODES value.
static __force_inline uint8_t sbdsp_codes_per_byte(uint8_t out_mode) {
    switch(out_mode) {
        case DSP_DMA_2: return 4;  // 2-bit ADPCM   : 4 codes/byte
        case DSP_DMA_3: return 3;  // 2.6-bit ADPCM : 3 codes/byte
        case DSP_DMA_4: return 2;  // 4-bit ADPCM   : 2 codes/byte
        default:        return 1;  // 8/16-bit PCM  : 1 sample/byte
    }
}

void sbdsp_start_pcm_dma(bool autoinit, uint16_t xfer_size, uint8_t out_mode) {
    PM_INFO("DMA %d,%d,%d",autoinit,xfer_size,out_mode);
    IOLOG(IOLOG_DSP,xfer_size,out_mode|(autoinit ? 0x80 : 0));
     sbdsp.dav_dsp=0;
     sbdsp.autoinit=autoinit;
     sbdsp.dma_mode = out_mode;
     sbdsp.adpcm_codes_per_byte = sbdsp_codes_per_byte(out_mode);
     sbdsp.adpcm_code_index = 0;
     sbdsp.dma_sample_count = xfer_size;
     sbdsp.dma_sample_count_rx=0;
     sbdsp.dma_rx_width=sbdsp.dma_stereo ? 2 : 1;
     sbdsp.dma_xfer_count = (xfer_size + 1) / sbdsp.dma_rx_width;
     sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
     sbdsp_dma_enable();
    }

static __force_inline void sbdsp_output(uint8_t value) {
    sbdsp.outbox=value;
    sbdsp.dav_pc=1;
}

void sbdsp_process(void) {
    if(sbdsp.reset_state) return;
    sbdsp.dsp_busy=1;

    if(sbdsp.dav_dsp) {
        if(!sbdsp.current_command) {
            sbdsp.current_command = sbdsp.inbox;
            sbdsp.current_command_index=0;
            sbdsp.dav_dsp=0;
        }
    }

    switch(sbdsp.current_command) {

        case DSP_DMA_PAUSE:               // 1.xx+ : DMA Pause  > To add : Insert silences
            sbdsp.current_command=0;
            sbdsp_dma_disable();
#if USE_HWDMA
            isa_dma_stop_write();
#endif
            sbdsp.mode= MODE_DMA_PAUSE; // Set the mode to DMA_PAUSE
            PM_INFO("DMA PAUSE\n");
            break;

        case DSP_DMA_RESUME:              // 1.xx+ : DMA Resume
            if (sbdsp.mode==MODE_DMA_PAUSE) {
              sbdsp_dma_enable();
             } else PM_INFO("!R");
            sbdsp.current_command=0;
            PM_INFO("DMA RESUME\n");
            break;

        case DSP_DMA_EXIT_AUTO8:       // 2.00+ : Exit Autoinit mode 8-bit
            sbdsp.current_command=0;
            sbdsp.autoinit=false;
            //PM_INFO("EXIT AUTO\n");
            break;

        case DSP_DMA_HS_AUTO:    // 2.01+ :8-bit DMA PCM Output, Auto, High Speed
        case DSP_DMA_AUTO:       // 2.00+ :8-bit DMA PCM Output, Auto
            PM_INFO("DMA_AUTO %d\n",sbdsp.dma_block_size);
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;
            sbdsp_start_pcm_dma(true, sbdsp.dma_block_size, DSP_DMA_8U);
            break;

        case DSP_SET_TIME_CONSTANT: // 1.xx+ :Set digitized sound transfer constant
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
//                PM_INFO("DSP_SET_TIME_CONSTANT %d\n",sbdsp.time_constant);
//                    sbdsp.mainloop_action=DSP_MLC_SET_TIME_CST;
                    sbdsp.time_constant = sbdsp.inbox;
                    sbdsp.dma_interval = 256 - sbdsp.time_constant;  // Interval is the Nb of us between each sample read

        // Initialize the timing values for the DMA Buffer read
                    sbdsp.sample_rate  = 1000000ul / sbdsp.dma_interval;
                    // Maybe change this in the buffer read code, triggered by a counter
        /// Buffer resampling variables init
                    sbdsp.sample_step  = (sbdsp.sample_rate * 65536ul) / PM_AUDIO_FREQUENCY; // Step for the Audio rate convertion
                    sbdsp.increment    = 0;
                    sbdsp.readsample   = true;
                    sbdsp.prev_sample8u = 0x80;
                    sbdsp.prev_sample8u_left = 0x80;
                    sbdsp.prev_sample16u = 0x8000;
                    sbdsp.prev_sample16u_left = 0x8000;

                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_BLOCK_SIZE:   // 2.00+ : Set block size for highspeed/dma
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {      //Block size LSB
                    sbdsp.dma_block_size=sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) { // Block size MSB
                    sbdsp.dma_block_size += (sbdsp.inbox << 8);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    PM_INFO("SB BSize:%u ",sbdsp.dma_block_size);
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_HS_SINGLE:    // 2.01+ :8-bit DMA PCM Output, single, High Speed
            PM_INFO("DMA_HS_SINGLE\n");
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;
            sbdsp_start_pcm_dma(false, sbdsp.dma_block_size, DSP_DMA_8U);
            break;

        case DSP_DMA_SINGLE:       // 1.xx+ :8-bit DMA PCM Output, Single
        case DSP_DMA_SINGLE2:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    PM_INFO("DMA_SINGLE %u\n",sbdsp.dma_sample_count);
#if USE_HWDMA
                   sbdsp_start_pcm_dma(false, sbdsp.dma_sample_count, DSP_DMA_8U);
#else
                   if (sbdsp.dma_sample_count<10)
                     {
                      sbdsp_dma_disable();
                      PIC_AddEvent(&DSP_DMA_IRQ_Event,sbdsp.dma_interval*(sbdsp.dma_sample_count+1),0);  // Send an IRQ fast with no DMA transfer
                     } else  sbdsp_start_pcm_dma(false, sbdsp.dma_sample_count, DSP_DMA_8U);
#endif
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_ADC:          // 1.xx+ :8-bit DMA PCM Input, Single (Used by Windows 3.11 driver install)
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    PM_INFO("DMA_ADC_SINGLE %u\n",sbdsp.dma_sample_count);
                    PIC_AddEvent(&DSP_DMA_IRQ_Event,sbdsp.dma_interval*(sbdsp.dma_sample_count+1),0);  // Send an IRQ with no DMA transfer (fake)
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_ADPCM_2_REF:   // First byte used as reference
            sbdsp.adpcm_ref=true;
        case DSP_DMA_ADPCM_2:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    PM_INFO("DMA_ADPCM_2 %u\n",sbdsp.dma_sample_count);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.adpcm_need_ref = sbdsp.adpcm_ref;
                    if(sbdsp.adpcm_ref) sbdsp.adpcm_scale = 0;
                    sbdsp.adpcm_ref = false;
                    sbdsp_start_pcm_dma(false, sbdsp.dma_sample_count, DSP_DMA_2);
                }
                sbdsp.current_command_index++;
            }
            break;


        case DSP_DMA_ADPCM_4_REF: // ! 1.xx+ :8-bit DMA ADPCM 2 bit Output, Single  With Reference
            sbdsp.adpcm_ref=true;
        case DSP_DMA_ADPCM_4:     // ! 1.xx+ :8-bit DMA ADPCM 2 bit Output, Single
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    PM_INFO("DMA_ADPCM_4 %u\n",sbdsp.dma_sample_count);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.adpcm_need_ref = sbdsp.adpcm_ref;
                    if(sbdsp.adpcm_ref) sbdsp.adpcm_scale = 0;
                    sbdsp.adpcm_ref = false;
                    sbdsp_start_pcm_dma(false, sbdsp.dma_sample_count, DSP_DMA_4);
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_ADPCM_26_REF:   // First byte used as reference  LENGTH = (SAMPLES-1 + 1)/2 + 1
            sbdsp.adpcm_ref=true;
        case DSP_DMA_ADPCM_26: // LENGTH = (SAMPLES-1 + 2)/3
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    PM_INFO("DMA_ADPCM_26 %u\n",sbdsp.dma_sample_count);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.adpcm_need_ref = sbdsp.adpcm_ref;
                    if(sbdsp.adpcm_ref) sbdsp.adpcm_scale = 0;
                    sbdsp.adpcm_ref = false;
                    sbdsp_start_pcm_dma(false, sbdsp.dma_sample_count, DSP_DMA_3);
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_INPUT_MONO:       // SB Pro input modes, recording is not emulated
        case DSP_INPUT_STEREO:
            sbdsp.current_command=0;
            break;

        case DSP_IRQ:
     //       PM_INFO("DSP_IRQ\n");
            sbdsp.current_command=0;
            DBG_ON_IRQ_SB();
            bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);   // Force an SB Interrupt
            break;

        case DSP_VERSION:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command_index=1;
                sbdsp_output(DSP_VERSION_MAJOR[sbdsp.type]);
            }
            else {
                if(!sbdsp.dav_pc) {
                    sbdsp.current_command=0;
                    sbdsp_output(DSP_VERSION_MINOR[sbdsp.type]);
                }

            }
            break;

        case DSP_IDENT:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                  //  PM_INFO("DSP_IDENT\n");
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp_output(~sbdsp.inbox);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_IDENT_E2: // yet another "protection"... used by CT-VOICE.DRV
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp.ident_e2[0] += sbdsp.inbox ^ sbdsp.ident_e2[1];
                    sbdsp.ident_e2[1] = (sbdsp.ident_e2[1] >> 2) | (sbdsp.ident_e2[1] << 6);
                    // The answer is a DMA transfer (written by the PC with the HW DMA, see sbdsp_e2_queue)
                    // Also in the outbox, for the programs that read it there
#if USE_HWDMA
                    sbdsp_e2_queue(sbdsp.ident_e2[0]);
#endif
                    sbdsp_output(sbdsp.ident_e2[0]);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_ENABLE_SPEAKER:
            PM_INFO("ENABLE SPEAKER\n");
            sbdsp.speaker_on = true;
            sbdsp.current_command=0;
            break;
        case DSP_DISABLE_SPEAKER:
            PM_INFO("DISABLE SPEAKER\n");
            sbdsp.speaker_on = false;
            sbdsp.current_command=0;
            break;
        case DSP_SPEAKER_STATUS:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command=0;
                sbdsp_output(sbdsp.speaker_on ? 0xff : 0x00);
            }
            break;

        case DSP_DIRECT_DAC:
            // Take the sample as soon as it is written, so it is time stamped when the game
            // writes it. Jill of the Jungle stops accessing the DSP at the end of a sound, and
            // waiting for the next access played the last sample at the start of the next sound.
            if(sbdsp.dav_dsp) {
                sbdsp.cur_sample8=sbdsp.inbox;
                ts_dac_write(&sbdsp_dac, sbdsp.inbox);
                sbdsp.dav_dsp=0;
                sbdsp.current_command=0;
            }
            break;
        case DSP_DIRECT_ADC:
              sbdsp.outbox = 0x79;  // Return a fake "Silence" value
              sbdsp.dav_pc=1;
              sbdsp.current_command=0;
            break;
        //case DSP_MIDI_READ_POLL:
        //case DSP_MIDI_WRITE_POLL:
        case DSP_WRITETEST:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    //PM_INFO("DSP_WRITETEST\n\r");
                    sbdsp.test_register = sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_READTEST:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command=0;
                sbdsp_output(sbdsp.test_register);
            }
            break;

        case DSP_DAC_PAUSE_DURATION:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    sbdsp.dac_pause_duration=sbdsp.inbox;
                    sbdsp.dav_dsp=0;
                }
                else if(sbdsp.current_command_index==2) {
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    sbdsp.dac_pause_duration += (sbdsp.inbox << 8);
                    sbdsp.dac_resume_pending = true;
                    PM_INFO("SB Pause:%u\n\r",sbdsp.dac_pause_duration);
                    // When the specified duration elapses, the DSP generates an interrupt.
                    PIC_AddEvent(&DSP_DAC_Resume_event, sbdsp.dma_interval * sbdsp.dac_pause_duration, 0);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_STATUS:   // DSP Status SB 2.0 version
              sbdsp.dav_pc=1;
              sbdsp.current_command=0;
              switch(sbdsp.type) {
                  case SBT_2:
                      sbdsp.outbox = 0x88;  // Return SB2.00 status
                      break;
                  case SBT_PRO2:
                      sbdsp.outbox = 0x7B;  // Return SBPro2 status
                      break;
                  default:
                      sbdsp.outbox = 0Xff;  //Everything enabled
                      break;
              }
            break;
        //case DSP_SINE:
        //case DSP_CHECKSUM:
        case 0:
            //not in a command
            break;
        default:
            PM_INFO("Unknown Command: %x\n",sbdsp.current_command);
            sbdsp.current_command=0;
            break;

    }
    sbdsp.dsp_busy=0;
}

static uint32_t DSP_Reset_EventHandler(Bitu val) {
    sbdsp.reset_state=0;
    sbdsp.outbox = 0xAA;
    sbdsp.dav_pc=1;
    sbdsp.current_command=0;
    sbdsp.current_command_index=0;

    sbdsp.dma_block_size=0x7FF; //default per 2.01
    sbdsp.dma_xfer_count = 0;
    sbdsp.dma_xfer_count_left = 0;
    sbdsp.dma_sample_count=0;
    sbdsp.dma_sample_count_rx=0;
    sbdsp.dma_signed = false;
    sbdsp.dma_mode = DSP_DMA_8U;
    sbdsp.adpcm_ref = false;
    sbdsp.adpcm_need_ref = false;
    sbdsp.adpcm_codes_per_byte = 1;
    sbdsp.adpcm_code_index = 0;
    sbdsp.adpcm_reference = 0x80;
    sbdsp.adpcm_scale = 0;
    sbdsp.speaker_on = false;
//    sbdsp.dma_done = false;
    sbdsp.dac_resume_pending = false;
    sbdsp.mode=MODE_NONE;
    return 0;
}
static PIC_TimerEvent DSP_Reset_Event = {
    .handler = DSP_Reset_EventHandler,
};

static __force_inline void sbdsp_reset(uint8_t value) {
    //TODO: COLDBOOT ? WARMBOOT ?
    value &= 1; // Some games may write unknown data for bits other than the LSB.
    switch(value) {
        case 1:
            PM_INFO("SB>RST ");
            PIC_RemoveEvent(&DSP_Reset_Event);
            sbdsp.autoinit=false;
            sbdsp_dma_disable();
#if USE_HWDMA
            isa_dma_stop_write();
            afifo_reset8(&sbdsp_fifo); // Reset the Audio FIFO
            sbdsp.stereo_origin = 0;
#else
            sbdsp.mainloop_action=DSP_MLC_DMA_RESET;
#endif
	        sbdsp.mode=MODE_RESET; // Set the mode to RESET
            sbdsp.reset_state=1;
//            PM_INFO("Ok\n");
            break;
        case 0:
            if(sbdsp.reset_state==1) {
                sbdsp.dav_pc=0;
                // AAh is ready a few us after the reset ends. 100us is only the longest time
                // Creative tells drivers to wait. Crystal Dream pulses the reset again before
                // each single check of 2xE, about 768 empty loops later (5us on a Pentium), so
                // a 100us delay was cancelled each time and the card was not found on a fast CPU.
                PIC_RemoveEvent(&DSP_Reset_Event);
                PIC_AddEvent(&DSP_Reset_Event, 2, 0);
                sbdsp.reset_state = 2;
            }
            break;
        default:
            break;
    }
}

// SB Pro mixer volume, 3 bits per channel (bits 7-5 left, 3-1 right) from -46dB to 0dB.
// Creative documents 4 as -11dB and about 4dB per step, 0 is almost off.
static const int32_t sbpro_vol_gain[8] = { 164, 2603, 3970, 6055, 9235, 14086, 21484, 32768 };   // Q15

static inline int32_t sbpro_vol(uint8_t reg, int ch) {
    return sbpro_vol_gain[ch ? ((reg >> 1) & 7) : (reg >> 5)];
}

// Gains of the SB digital output and of the FM, from the mixer registers. Unity without the mixer.
static void sbmixer_update_gains() {
    for (int ch = 0; ch < 2; ch++) {
        if (sbdsp.type == SBT_PRO2) {
            int32_t master = sbpro_vol(mixer_state[MIXER_MASTER], ch);
            sbpro_voice_gain[ch] = (sbpro_vol(mixer_state[MIXER_VOICE], ch) * master) >> 15;
            opl_gain[ch] = (sbpro_vol(mixer_state[MIXER_FM], ch) * master) >> 15;
        } else {
            sbpro_voice_gain[ch] = 32768;
            opl_gain[ch] = 32768;
        }
    }
    sbpro_gain_on = (sbpro_voice_gain[0] != 32768) || (sbpro_voice_gain[1] != 32768);
}

static void sbmixer_set_stereo(bool stereo) {
    if (stereo == sbdsp.dma_stereo) return;
    // The bytes already in the FIFO are dropped when stereo starts, the next one is a right sample
    sbdsp.stereo_origin = sbdsp_fifo.samples_received;
    sbdsp.dma_stereo = stereo;
}

// Register values after a mixer reset, as documented by Creative. Voice, Master and MIDI are at
// 4 (-11dB), CD and Line off, mono, filters on, microphone input.
static void sbmixer_reset() {
    memset(mixer_state, 0, sizeof(mixer_state));
    mixer_state[MIXER_VOICE]  = 0x88;
    mixer_state[MIXER_MASTER] = 0x88;
    mixer_state[MIXER_FM]     = 0x88;
    bl_sbpro_filter = true;
    sbmixer_set_stereo(false);
    sbmixer_update_gains();
}

// Mixer of a card that was just installed. Programs rarely set the volumes, and DOS users ran
// SBP-SET to set them, so Voice, Master and MIDI start at the maximum instead of -11dB.
static void sbmixer_init() {
    sbmixer_reset();
    mixer_state[MIXER_VOICE]  = 0xEE;
    mixer_state[MIXER_MASTER] = 0xEE;
    mixer_state[MIXER_FM]     = 0xEE;
    sbmixer_update_gains();
}

// Register index after the CT1600 mirrors (see sbmix.cpp), -1 above 3Fh where nothing is
static int sbmixer_index() {
    uint8_t i = sbdsp.mixer_command;
    if (i >= 0x40) return -1;
    i = (i < 0x20) ? (i & 0x0F) : (0x20 | (i & 0x0F));
    if (i == 0x0F) i = MIXER_STEREO;
    return i;
}

static uint8_t sbmixer_read(void) {
    int i = sbmixer_index();
    if (i < 0) return 0xFF;
    if (i <= 0x01) i = MIXER_STEREO;   // Reading 00h and 01h reads 0Eh on the CT1600
    uint8_t v = mixer_state[i];
    switch (i) {
        case MIXER_VOICE:
        case MIXER_MASTER:
        case MIXER_FM:
        case MIXER_CD:
        case MIXER_LINE:
            return v | 0x11;               // The unused bits read 1
        case MIXER_STEREO:
            return (v & 0x22) | 0x11;      // 11h after a reset on the CT1600
        case MIXER_INPUT:
            return (v & 0x2E) | 0x11;
        case MIXER_MIC:
            return v & 0x06;
        default:
            return v;
    }
}

static void sbmixer_write(uint8_t value) {
    int i = sbmixer_index();
    if (i < 0) return;
    if (i <= 0x01) {                   // Writing 00h or 01h resets the mixer
        sbmixer_reset();
        return;
    }
    mixer_state[i] = value;
    switch (i) {
        case MIXER_STEREO:
            bl_sbpro_filter = !(value & 0x20);
            sbmixer_set_stereo(value & 0x02);
            break;
        case MIXER_VOICE:
        case MIXER_MASTER:
        case MIXER_FM:
            sbmixer_update_gains();
            break;
    }
}

uint8_t sbdsp_read(uint8_t address)
{
 switch(address) {
      case MIXER_DATA:
            return (sbdsp.type == SBT_PRO2) ? sbmixer_read() : 0xFF;
      case DSP_READ:          // 0x0A : Return DSP Data, if the outbox is not empty
            sbdsp.dav_pc=0;
            return sbdsp.outbox;
      case DSP_READ_STATUS:   // 0x0E : Return bit7=1 if there is data to read from the DSP (Also acknowledge IRQ)
            bus_irq_lower(IRQ_R_AUDIO,sbdsp.irq); // Acknowledge the IRQ
            DBG_OFF_IRQ_SB();
//            PM_INFO("iAck");
        return sbdsp.dav_pc << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
      case DSP_WRITE_STATUS:  // 0x0C : Return bit7=0 if the DSP is ready to receive Command/Data
        {
        // The SB and SB Pro DSP flips its busy bit by itself while DMA playback runs, and is
        // ready otherwise (the SB16 flips it all the time). Here it is busy 8 reads out of 16
        // during DMA, as in DOSBox, so a driver waiting for ready gets it within 8 reads.
        // CT-VOICE.DRV (X-Wing) waits for busy then ready before D0 (stop or pause a playing
        // voice), and freezes the PC for up to 65535 reads if busy never comes.
        // Jill of the Jungle reads the status once per timer tick and drops the Direct DAC
        // sample when busy. It uses no DMA, so it always reads ready.
        // Crystal Dream polls the busy bit to find the end of a DMA block, and never starts
        // the next one if the bit keeps flipping after the block (as on the SB16).
        // Not busy during the 80h silence (dac_resume_pending), the DSP keeps taking commands as
        // in DOSBox (CT-VOICE.DRV also waits for ready with no timeout).
        static uint8_t write_status_reads;
        bool busy = false;
        if (sbdsp.dma_enabled) busy = (++write_status_reads & 8) != 0;
        return (sbdsp.dav_dsp | sbdsp.dsp_busy | busy) << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        }
      default:
//            PM_INFO("SB READ: %x\n\r",address);
            return 0xFF;
  }
}


// Return true if a value is really written
bool sbdsp_write(uint8_t address, uint8_t value)
{
    switch(address) {
        case DSP_WRITE:         // 0x0C : Write command/Data to the DSP
            if(sbdsp.dav_dsp) PM_INFO("WARN - DAV_DSP OVERWRITE\n");
            sbdsp.inbox = value;
            sbdsp.dav_dsp = 1;
            return true;
            break;
        case DSP_RESET:         // 0x06 : Reset Port (Write 0 then 1)
            sbdsp_reset(value);
            return true;
            break;
        case MIXER_ADDRESS:
            sbdsp.mixer_command = value;
            break;
        case MIXER_DATA:
            if (sbdsp.type == SBT_PRO2) sbmixer_write(value);
            else {
                // The SB 2.0 has no mixer. Only the output filter switch is followed, for the
                // SB Pro 2 filter mode of programs set up for an SB Pro.
                if (sbdsp.mixer_command == MIXER_RESET) bl_sbpro_filter = true;
                else if (sbdsp.mixer_command == MIXER_STEREO) bl_sbpro_filter = !(value & 0x20);
            }
            break;
        default:
          //  PM_INFO("SB WRITE: %x => %x \n\r",value,address);
            break;
    }
return false;
}

// Creative ADPCM decode tables (2-bit, 2.6-bit "3-bit" and 4-bit variants).
// Sourced from the DOSBox-X's decode_ADPCM_{2,3,4}_sample()
// (src/hardware/sblaster.cpp), the de facto reference implementation of
// these otherwise-undocumented Creative tables.
static const int8_t sbdsp_adpcm2_scaleMap[24] = {
     0,  1,  0,  -1,  1,  3,  -1,  -3,
     2,  6, -2,  -6,  4, 12,  -4, -12,
     8, 24, -8, -24, 16, 48, -16, -48
};
static const uint8_t sbdsp_adpcm2_adjustMap[24] = {
      0,   4,   0,   4,
    252,   4, 252,   4,
    252,   4, 252,   4,
    252,   4, 252,   4,
    252,   4, 252,   4,
    252,   0, 252,   0
};

static const int8_t sbdsp_adpcm3_scaleMap[40] = {
    0,  1,  2,  3,   0,  -1,  -2,  -3,
    1,  3,  5,  7,  -1,  -3,  -5,  -7,
    2,  6, 10, 14,  -2,  -6, -10, -14,
    4, 12, 20, 28,  -4, -12, -20, -28,
    5, 15, 25, 35,  -5, -15, -25, -35
};
static const uint8_t sbdsp_adpcm3_adjustMap[40] = {
      0, 0, 0, 8,    0, 0, 0, 8,
    248, 0, 0, 8,  248, 0, 0, 8,
    248, 0, 0, 8,  248, 0, 0, 8,
    248, 0, 0, 8,  248, 0, 0, 8,
    248, 0, 0, 0,  248, 0, 0, 0
};

static const int8_t sbdsp_adpcm4_scaleMap[64] = {
    0,  1,  2,  3,  4,  5,  6,  7,   0,  -1,  -2,  -3,  -4,  -5,  -6,  -7,
    1,  3,  5,  7,  9, 11, 13, 15,  -1,  -3,  -5,  -7,  -9, -11, -13, -15,
    2,  6, 10, 14, 18, 22, 26, 30,  -2,  -6, -10, -14, -18, -22, -26, -30,
    4, 12, 20, 28, 36, 44, 52, 60,  -4, -12, -20, -28, -36, -44, -52, -60
};
static const uint8_t sbdsp_adpcm4_adjustMap[64] = {
      0, 0, 0, 0, 0, 16, 16, 16,
      0, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0, 16, 16, 16,
    240, 0, 0, 0, 0,  0,  0,  0,
    240, 0, 0, 0, 0,  0,  0,  0
};

// Decode one ADPCM code, updating the running reference/scale decoder state.
// Returns the decoded 8-bit unsigned PCM sample.
static __force_inline uint8_t sbdsp_adpcm_step(uint8_t code, const int8_t *scaleMap, const uint8_t *adjustMap, uint8_t table_size) {
    int16_t samp = (int16_t)code + sbdsp.adpcm_scale;
    if (samp < 0) samp = 0;
    else if (samp >= table_size) samp = table_size - 1;

    int16_t ref = (int16_t)sbdsp.adpcm_reference + scaleMap[samp];
    if (ref > 0xFF) ref = 0xFF;
    else if (ref < 0x00) ref = 0x00;

    sbdsp.adpcm_reference = (uint8_t) ref;
    sbdsp.adpcm_scale = (uint8_t)(sbdsp.adpcm_scale + adjustMap[samp]);
    return sbdsp.adpcm_reference;
}

// Decode one raw DMA byte according to the current ADPCM mode and push the
// resulting PCM8 sample(s) into the DSP audio FIFO.
static void sbdsp_adpcm_decode_byte(uint8_t raw) {
    if (sbdsp.adpcm_need_ref) {
        // The first byte of a "with reference" transfer is an uncompressed
        // PCM8 sample used to (re)initialize the decoder state.
        sbdsp.adpcm_reference = raw;
        sbdsp.adpcm_scale = 0;
        sbdsp.adpcm_need_ref = false;
        afifo_add_sample8(&sbdsp_fifo, (int8_t) raw);
        return;
    }

    switch (sbdsp.dma_mode) {
        case DSP_DMA_2:
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>6)&0x3, sbdsp_adpcm2_scaleMap, sbdsp_adpcm2_adjustMap, 24));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>4)&0x3, sbdsp_adpcm2_scaleMap, sbdsp_adpcm2_adjustMap, 24));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>2)&0x3, sbdsp_adpcm2_scaleMap, sbdsp_adpcm2_adjustMap, 24));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step( raw    &0x3, sbdsp_adpcm2_scaleMap, sbdsp_adpcm2_adjustMap, 24));
            break;
        case DSP_DMA_3:
            // "2.6-bit" packing: two 3-bit codes followed by one 2-bit code
            // (whose missing low bit is always 0), 8 bits for 3 samples.
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>5)&0x7, sbdsp_adpcm3_scaleMap, sbdsp_adpcm3_adjustMap, 40));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>2)&0x7, sbdsp_adpcm3_scaleMap, sbdsp_adpcm3_adjustMap, 40));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw<<1)&0x6, sbdsp_adpcm3_scaleMap, sbdsp_adpcm3_adjustMap, 40));
            break;
        case DSP_DMA_4:
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step((raw>>4)&0xF, sbdsp_adpcm4_scaleMap, sbdsp_adpcm4_adjustMap, 64));
            afifo_add_sample8(&sbdsp_fifo,(int8_t) sbdsp_adpcm_step( raw    &0xF, sbdsp_adpcm4_scaleMap, sbdsp_adpcm4_adjustMap, 64));
            break;
        default:
            break;
    }
}

// dma buffer level must be !=0
// Use another variable to have the value stable during the transfer
void sbdsp_transfer(uint16_t samples)
{
 uint32_t to_transfer;

 /*
if ((sbdsp_fifo.samples_received-sbdsp_fifo.samples_sent) < AUDIO_FIFO_START_THRESHOLD-256) {
    sbdsp.timer_delta= -5; // Speed up the transfer
   } else if ((sbdsp_fifo.samples_received-sbdsp_fifo.samples_sent) < AUDIO_FIFO_START_THRESHOLD-256) {
    sbdsp.timer_delta= 5; // Slow down the transfer
   } else sbdsp.timer_delta= 0; // Normal transfer speed
*/

  uint32_t afifolvl=AUDIO_FIFO_SIZE - (sbdsp_fifo.samples_received-sbdsp_fifo.samples_sent);

  if (sbdsp.adpcm_codes_per_byte>1) {
      // ADPCM: each raw byte can expand into up to adpcm_codes_per_byte PCM
      // samples, so keep enough headroom in the FIFO for the worst case.
      to_transfer = MIN(samples, afifolvl / sbdsp.adpcm_codes_per_byte);
      to_transfer = MIN((uint32_t) isa_dma.buffer_level, to_transfer);
      if (to_transfer==0) return;
      for (uint32_t i=0; i<to_transfer; i++) {
          sbdsp_adpcm_decode_byte((uint8_t) isa_dma_buffer_get());
      }
      sbdsp.vdma_sent+=to_transfer;
      return;
  }

  to_transfer = MIN(samples,afifolvl);  // Minimum of samples to send and audio fifo level
  if (to_transfer==0) return;
  if (to_transfer==1)
       {
        afifo_add_sample8(&sbdsp_fifo,isa_dma_buffer_get());
        sbdsp.vdma_sent++;
       } else
       {
        to_transfer = MIN(isa_dma.buffer_level,samples); // Get the number of bytes to transfer to the Audio fifo

        afifo_add_samples8(&sbdsp_fifo, isa_dma_buffer_get_ptr(to_transfer), to_transfer);
        sbdsp.vdma_sent+=to_transfer;  // !!! If the else is here, Raptor crack
       }
}

// Worked/Code to call regularly from the main loop
// Transfer data from the DMA to the Sound Blaster Audio FIFO
// "input" is sbdsp.vdma_to_send the value change during the code execution.
void sbdsp_dma_worker()
{
#if USE_HWDMA

    if (dma_inprogress)
        if ((time_us_64()-PM_DMA_SendTime) > 100) { // 100us
         PM_INFO("SB DMA Timeout\n");
        // isa_dma_complete_write();
        // dma_inprogress=false;
        }

    sbdsp_e2_send();   // E2 command answers to write in the PC memory

#else

#if DBG_PIN_AUDIO
  if (SVAR_IRQ->PM_IRR) DBG_ON_INT_SB();
     else DBG_OFF_INT_SB();
#endif

  if (sbdsp.mainloop_action==DSP_MLC_DMA_RESET) {  // Perform the reset here, to not change the variable in the middle of the worker execution
      PM_INFO(">MLCRST ");
      sbdsp.vdma_to_send=0;   // Reset the Virtual DMA sample count
      sbdsp.vdma_sent=0;
      isa_dma_buffer_free();  // Free the DMA buffer
      sbdsp.mainloop_action=DSP_MLC_NONE;
      return;
     }
/*
  if (sbdsp.mainloop_action==DSP_MLC_RESET) {
     afifo_reset8(&sbdsp_fifo); // Reset the FIFO
   }
*/

  if (sbdsp.vdma_to_send!=sbdsp.vdma_sent) {  // Execute only if there is data to transfer
  DBG_ON_SB_UPD();
  dev_sbdsp_enable_mix(); // Enable mixing

  if (isa_dma.buffer_level==0) isa_dma_transfer(sbdsp.dma8); //(SB DMA for test=1)
  if (isa_dma.buffer_level>0)  sbdsp_transfer(sbdsp.vdma_to_send-sbdsp.vdma_sent);

  DBG_OFF_SB_UPD();
 }

if (sbdsp.mainloop_action==DSP_MLC_SENDIRQ) {  // Perform the reset here, to not change the variable in the middle of the worker execution
 //     PM_INFO(">I ");
      DBG_ON_IRQ_SB();
      bus_irq_raise(IRQ_R_AUDIO,sbdsp.irq,true);
      sbdsp.mainloop_action=DSP_MLC_NONE;
      return;
     }

#endif
}

// Add the DSP output (Direct DAC and DMA) to buff
static void sbdsp_render(int16_t* buff,uint32_t samples)
{
  // Direct DAC : only a compare when it is not used
  if (ts_dac_pending(&sbdsp_dac)) ts_dac_render(&sbdsp_dac, buff, samples);

  const uint32_t rate = sbdsp.sample_rate ? sbdsp.sample_rate : 22050;
  if (sbdsp.dma_stereo) {
      int32_t old = (int32_t) (sbdsp.stereo_origin - sbdsp_fifo.samples_sent);
      if (old > 0) afifo_drop8(&sbdsp_fifo, old);
  }
  if (sbdsp.dma_stereo) bl_render_fifo8s(&sbdsp_bl, &sbdsp_bl_r, &sbdsp_fifo, rate / 2, buff, samples);   // SB Pro, right and left bytes
    else bl_render_fifo8(&sbdsp_bl, &sbdsp_fifo, rate, buff, samples);   // 8 bit mono
}

static int16_t sbdsp_mixbuf[PM_SAMPLES_PER_BUFFER*2];

// return an audio buffer by reading from the audio fifo
void sbdsp_updatebuffer(int16_t* buff,uint32_t samples)
{
  if (!sbpro_gain_on) {
      sbdsp_render(buff, samples);
      return;
  }
  // SB Pro mixer Voice and Master volumes
  const int32_t gl = sbpro_voice_gain[0], gr = sbpro_voice_gain[1];
  while (samples) {
      const uint32_t n = MIN(samples, (uint32_t) PM_SAMPLES_PER_BUFFER);
      memset(sbdsp_mixbuf, 0, n * 2 * sizeof(int16_t));
      sbdsp_render(sbdsp_mixbuf, n);
      for (uint32_t i = 0; i < n * 2; i += 2) {
          int32_t l = buff[i]   + ((sbdsp_mixbuf[i]   * gl) >> 15);
          int32_t r = buff[i+1] + ((sbdsp_mixbuf[i+1] * gr) >> 15);
          buff[i]   = (int16_t) (l > 32767 ? 32767 : l < -32768 ? -32768 : l);
          buff[i+1] = (int16_t) (r > 32767 ? 32767 : r < -32768 ? -32768 : r);
      }
      buff += n * 2;
      samples -= n;
  }
}

// Sound Blaster type (SBT_2, SBT_PRO2, SBT_NONE when removed), with its mixer at power on
void sbdsp_set_type(uint8_t type)
{
  sbdsp.type = type;
  sbmixer_init();
}
