/* Copyright (C) 2026 Freddy VETELE

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program.
If not, see <https://www.gnu.org/licenses/>.
*/

/* dev_mpu.cpp : PicoMEM simple MPU (Midi Processor Unit) implementation
To test : Doom, Doom2, Hexen, Duke nukem 3D, Raptor, Descent, Gods, Dune 2, Sim City 2000, Ultima 7, Ultima 8, Wing Commander 1/2/3,
          Alone in the Dark, Lemmings, Warcraft 1/2,
          X-COM UFO Defense, X-COM Terror from the Deep,
          Civilization 1/2, Star Control 2, Daggerfall,
          Eye of the Beholder 1/2/3, Dark Sun Shattered Lands,
          Betrayal at Krondor, Anvil of Dawn,
          Might and Magic 3/4/5,
          Heroes of Might and Magic 1/2,
          Lands of Lore 1/2,
          The Elder Scrolls: Arena/Daggerfall,
          Fallout 1/2,
          System Shock 1/2,
          Planescape Torment, Baldur's Gate 1/2,
          Icewind Dale 1/2,
          Arcanum,
          Messiah,
          Freespace 1/2,
          Descent Freespace 3

The MPU-401 port protocol (data/command/status registers, reset and
UART-mode handshake, the software FIFO) is unchanged. What used to happen
to a fully reassembled MIDI message - handing it to the internal TSF
soundfont synth - is replaced by forwarding the exact same bytes out over
a real MIDI-speed (31250 baud) hardware UART on GPIO12 (TX) / GPIO13 (RX),
so a real external module (e.g. a Roland MT-32) receives genuine MIDI
data instead of PicoMEM rendering the notes itself. SysEx runs (used by
many MT-32-aware games for device detection/setup, e.g. the "MT-32 Reset"
and display messages) are forwarded verbatim too, byte for byte, instead
of being dropped as they were for the internal synth.

https://www.midimusicadventures.com/queststudios/mt32-resource/utilities/
https://drive.google.com/drive/folders/1TbLOvr6GwqrxteWH8RD7Z7Q7t8q-vsIe

 */

#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "../pm_debug.h"
#include "../pm_gvars.h"
#include "pm_defines.h"
#include "dev_picomem_io.h"
#include "dev_audiomix.h"
#include "pm_audio.h"

#if USE_USBHOST
#include "hid_dev.h"
//uint32_t tuh_midi_stream_flush( uint8_t dev_addr);
//#include "usb_midi_host.h"
#include "tusb.h"
#endif

#include <string.h>

/* 0x330: data port
 * 0x331: read: status port
 *       write: command port
 * status port:
 * bit 6: 0=ready to write cmd or MIDI data; 1=interface busy
 * bit 7: 0=data ready to read; 1=no data at data port
 * command port:
 *  0xff: reset - triggers ACK (FE) to be read from data port
 *  0x3f: set to UART mode - triggers ACK (FE) to be read from data port
 */

// Hardware MIDI OUT: real 31250 baud UART on GPIO12(TX)/GPIO13(RX), the
// board's default debug-UART pins (inert on PicoMEM 2.0 - stdio never
// enables that peripheral, see src/CMakeLists.txt STDIO_UART/ASYNC_UART).
#define MPU_MIDI_UART      uart0
#define MPU_MIDI_TX_PIN    12
#define MPU_MIDI_RX_PIN    13
#define MPU_MIDI_BAUD_RATE 31250

static bool mpu_uart_ready = false;

static void dev_mpu_uart_init()
{
  if (mpu_uart_ready) return;
  uart_init(MPU_MIDI_UART, MPU_MIDI_BAUD_RATE);
  gpio_set_function(MPU_MIDI_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(MPU_MIDI_RX_PIN, GPIO_FUNC_UART);
  uart_set_format(MPU_MIDI_UART, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(MPU_MIDI_UART, true);
  mpu_uart_ready = true;
  PM_INFO("MPU MIDI UART ready (GPIO%d TX/GPIO%d RX @ %d baud)\n", MPU_MIDI_TX_PIN, MPU_MIDI_RX_PIN, MPU_MIDI_BAUD_RATE);
}

// _raw: MIDI is binary, must not go through uart_putc()'s \n -> \r\n translation
static __force_inline void dev_mpu_uart_putc(uint8_t Data)
{
  uart_putc_raw(MPU_MIDI_UART, Data);
}

static bool bReset = false;
static uint8_t bUART = 0;

static unsigned char midi_status_byte = 0x80;
static unsigned char midi_mpu_status  = 0x80;
static unsigned char midi_msg_len;
static unsigned char midi_msg_cnt;
static unsigned char midi_msg_tx_cache[3];      // Store the midi message being sent to the mpu (message are 3 bytes long max)
static unsigned char midi_msg_tx_len;
static unsigned char midi_msg_tx_cnt;
static bool midi_in_message    = false;
static bool midi_in_message_tx = false;
static bool midi_in_sysex      = false;    // Capture side (dev_mpu_iow): currently receiving a sysex run
static bool midi_in_sysex_tx   = false;    // Drain side (dev_mpu_process_messages): currently forwarding a sysex run

static const int midi_lengths[8] = { 2, 2, 2, 2, 1, 1, 2, 0 };

bool dev_mpu_active = false;
volatile uint8_t dev_mpu_delay=0;      // counter for the Nb of second since last I/O

// Tiny 256 bytes FIFO
typedef struct mpu_fifo_t {
    uint8_t data[256];
    volatile uint8_t head;
    volatile uint8_t tail;
} mpu_fifo_t;

mpu_fifo_t *mpu_fifo=NULL;
#define MIDI_FIFO_SIZE 255

uint8_t mpu_fifo_init()
{
 if (!mpu_fifo)
  {
    mpu_fifo=(mpu_fifo_t *) calloc(1,sizeof(mpu_fifo_t));
    if (mpu_fifo == NULL) return CMDERR_MEM_ERR;  // Not enaugh memory
  }
 return 0;
}

void mpu_fifo_end()
{
 if (mpu_fifo)
  {
    free(mpu_fifo);
    mpu_fifo=NULL;
  }
}

void mpu_fifo_clear()
{
  if (mpu_fifo)
   {
     mpu_fifo->tail=0;
     mpu_fifo->head=0;
   }
}

uint8_t mpu_fifo_level()
{
 return mpu_fifo->head-mpu_fifo->tail;
}

void mpu_fifo_tx(uint8_t Data)
{
 //if (mpu_fifo_level()==MIDI_FIFO_SIZE)
 mpu_fifo->data[mpu_fifo->head]=Data;
 mpu_fifo->head++;
}

// Must be sure the fifo is not empty before
bool mpu_fifo_rx(uint8_t *Data)
{
 uint8_t res;
 if (mpu_fifo->head==mpu_fifo->tail) return false;  // Empty !
 *Data=mpu_fifo->data[mpu_fifo->tail];
 mpu_fifo->tail++;
 return true;
}

uint8_t dev_mpu_install(uint16_t port)
{
 if (dev_mpu_active) {
    PM_INFO("MPU already active\n");
    return 0; // Already active
  }

 PM_INFO("Install MPU Device on Port %x\n",port);
 bReset = false;
 midi_in_sysex = false;
 midi_in_sysex_tx = false;
 uint8_t retcode=mpu_fifo_init();  // Return 0 if the fifo can be allocated
 if (retcode) {
    PM_INFO("MPU FIFO Init Failed\n");
    return retcode;
   }

 dev_mpu_uart_init();   // Route MIDI out over the hardware UART instead of an internal synth

 SetPortType(port,DEV_MPU,1);
 dev_mpu_active=true;
 return 0;
}

void dev_mpu_remove()
{
 PM_INFO("Remove MPU Device\n");
 if (dev_mpu_active)  {
   DelPortType(DEV_MPU);
   mpu_fifo_end();  // Unallocate the mpu fifo
  }
}

void dev_mpu_process_messages()
{
  uint8_t Data;
  uint8_t cmd_ch;

// if (mpu_fifo_level()) PM_INFO("-PROCESS: h:%d t:%d LVL:%d\n",mpu_fifo->head, mpu_fifo->tail,mpu_fifo_level());

 while (mpu_fifo_rx(&Data)) {
  //  PM_INFO("D%X ",Data);

    if (midi_in_sysex_tx) {   // Streaming a variable-length sysex run: forward raw, terminated by 0xF7
        dev_mpu_uart_putc(Data);
        if (Data == 0xF7) midi_in_sysex_tx = false;
        continue;
       }

    if (Data == 0xF0) {
        midi_in_sysex_tx = true;
        dev_mpu_uart_putc(Data);
        continue;
       }

    if ((Data & 0xF0) >= 0x80) {  // The value is a message Start
        midi_msg_tx_len=midi_lengths[(Data>>4)-8];  // Read the Nb of parameters for this command
        midi_in_message_tx = true;
        midi_msg_tx_cnt = 0;
       }

    if (midi_in_message_tx) {
        midi_msg_tx_cache[midi_msg_tx_cnt]=Data;  // Store the Data in the message buffer
        if (midi_msg_tx_len==midi_msg_tx_cnt) {   // Forward the message once all its bytes are reassembled
           for (uint8_t i=0; i<=midi_msg_tx_len; i++) dev_mpu_uart_putc(midi_msg_tx_cache[i]);
            midi_in_message_tx = false;
            }
           midi_msg_tx_cnt++;
          }
  }
}

void dev_mpu_update()
{
  if (mpu_fifo) dev_mpu_process_messages();
}

extern bool dev_mpu_ior(uint32_t Addr,uint8_t *Data)
{
//  PM_INFO("MPU R(%X) ", Addr );
  if ( Addr == 0x330 ) {
        midi_mpu_status |= 0x80;
        if ( bReset ) {
                PM_INFO("CLR Rst ");
                bReset = false;
                *Data=0xfe;
                return true;
            } else {
              *Data=0xfe; // Always return Active Sensing.
              PM_INFO("%X ", *Data );
              return true;
            }
      } else {
         *Data=midi_mpu_status | (mpu_fifo_level()>=(MIDI_FIFO_SIZE-4) ? 0x40 : 0);
   //      PM_INFO("s%X ", *Data );
         return true;
        }
  return false; // nothing to return
}

void dev_mpu_iow(uint32_t Addr,uint8_t Data)
{
//   PM_INFO("MPU W(%X)=%X ", Addr, Data );
   if ( Addr == 0x331 ) {        // 0x331 : Command port
        if ( Data == 0x3f ) {   // 0x3F  : After RESET, Set UART mode
        	midi_mpu_status &= ~0x80;
               }
        if ( Data == 0xff ) {   // 0xFF  : Reset command
              //  dev_mpu_process_messages();
                PM_INFO("SET Rst ");
                bReset = true;   // Remains in Reset mode until status is read;
                midi_in_sysex = false;
                midi_in_message = false;
                mpu_fifo_clear();
                midi_mpu_status &= ~0x80;
               }
      } else if ( Addr == 0x330 ) { // 0x330 : Data Port
        if (!bReset) {
            dev_audiomix.dev_active = dev_audiomix.dev_active | AD_MPU;   // enable the mixing
            dev_mpu_delay=0;
                 // PM_INFO("Data %d ",Data);
                  if (mpu_fifo_level()==MIDI_FIFO_SIZE) {
                        PM_ERROR("MPU:Fifo Full\n");
                        return;  // Should never happens as the host code should check the status first
                     }

                  if (midi_in_sysex) {  // Forward sysex bytes verbatim (real MT-32-class hardware needs these)
                          mpu_fifo_tx(Data);
                          if (Data == 0xF7) {
                                  midi_in_sysex = false;
                                 }
                          return;
                         }

                  if (Data == 0xF0) {
                        midi_in_sysex = true;
                        mpu_fifo_tx(Data);
                        return;
                       }

                  if ((Data & 0xF0) >= 0x80) {  // The value is a message Start
                       midi_msg_len=midi_lengths[(Data>>4)-8];
                       midi_in_message = true;
                       midi_msg_cnt = 0;
                 //      PM_INFO("MSG %X, %d ",Data,midi_msg_len);
                      }

                  if (midi_in_message) {
                  //      PM_INFO("TX:%X ",Data);
                        mpu_fifo_tx(Data);  // Send the Data to the fifo
                        //PM_INFO("L%dC%d ",midi_msg_len,midi_msg_cnt);
                        if (midi_msg_len==midi_msg_cnt)
                           {
                   //         PM_INFO("\n",Data);
                            midi_in_message = false; //Stop sending until next valid message/command
                           }
                        midi_msg_cnt++;
                       }
                 }
  }
  return;
}

void dev_mpu_senddata(uint8_t Data)
{
  uint8_t Status;
  do { dev_mpu_ior(0x331,&Status); } while (Status!=0x80); // Wait ready
  dev_mpu_iow(0x330,Data);
}

uint8_t dev_mpu_readdata()
{
  uint8_t Status;
  uint8_t Data;

  do { dev_mpu_ior(0x331,&Status); } while (Status!=0x80); // Wait ready
  dev_mpu_ior(0x330,&Data);
  return Data;
}

void dev_mpu_sendcommand(uint8_t Data)
{
  uint8_t Status;
  do { dev_mpu_ior(0x331,&Status); } while (Status!=0x80); // Wait ready
  dev_mpu_iow(0x331,Data);
}

void dev_mpu_test()
{
  const uint8_t TestData[10] = { 0xA0, 1, 2, 0xB0, 3, 4, 0xC0, 5, 0xD0, 6};
  uint8_t Status;
  dev_mpu_install(0x330);

  /*
  dev_mpu_sendcommand(0xFF);
  dev_mpu_ior(0x330,&Status);
  dev_mpu_sendcommand(0x3F);
  dev_mpu_readdata();
*/

 for (int j=0; j<60; j++)
  for (int i=0; i<10; i++)
   {
    if (i==5) dev_mpu_update();
    dev_mpu_senddata(TestData[i]);
   }

  dev_mpu_remove();

}
