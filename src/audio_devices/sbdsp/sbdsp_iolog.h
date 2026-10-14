#pragma once
// Sound Blaster I/O trace to the sbiolog.txt on the SD card, compiled only with SBDSP_IOLOG=1.
// Each line holds the time in us, the type and the data:
//   W 22C=D1                        Sound Blaster port write
//   R 22E=FF x37                    Sound Blaster port read, repeated 37 times
//   D 0B=49                         DMA controller (8237) register write
//   I irq 7 (IRQ line)              Sound Blaster IRQ on its ISA line or through the PicoMEM IRQ
//   S dma size=1023 mode=0 auto=1   DSP DMA transfer start
//   E 1234=3A                       DSP E2 answer written to the PC memory at this DMA offset

#include <stdint.h>

#define IOLOG_W    'W'
#define IOLOG_R    'R'
#define IOLOG_DMA  'D'
#define IOLOG_IRQ  'I'   // port = 1 if raised on the ISA IRQ line
#define IOLOG_DSP  'S'   // port = transfer size, value = mode | 0x80 if auto init
#define IOLOG_E2   'E'   // port = address (low 16 bits), value = E2 answer

#if SBDSP_IOLOG
#ifdef __cplusplus
extern "C" {
#endif
// Must be called BEFORE core 1 starts
void sbdsp_iolog_init(void);
void sbdsp_iolog_add(uint8_t type, uint16_t port, uint8_t value);
// Writes the log to the SD card. Must be called from the main loop.
void sbdsp_iolog_task(void);
#ifdef __cplusplus
}
#endif
#define IOLOG(type, port, value) sbdsp_iolog_add((type), (port), (value))
#else
#define IOLOG(type, port, value) do { } while (0)
#endif
