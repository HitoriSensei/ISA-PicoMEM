#pragma once
// I/O trace to the SD card, for debugging (SBDSP_IOLOG=1 in CMakeLists.txt, compiled out otherwise).
// The ISA I/O handlers (core 1) and the audio code add entries to a RAM ring buffer, the main
// loop writes them to "sbiolog.txt" at the root of the SD card (rewritten at each power on).
// Line format: time in us, type, data
//   W 22C=D1      Sound Blaster port write
//   R 22E=FF x37  Sound Blaster port read (x37 : 37 consecutive identical reads)
//   D 0B=49       DMA controller (8237) register write
//   I irq 7 (IRQ line)   Sound Blaster IRQ raised, on its ISA line or forwarded by the PicoMEM BIOS
//                        through the PicoMEM IRQ (no jumper detected for this IRQ)
//   S dma size=1023 mode=0 auto=1   DSP DMA transfer started
//   E 1234=3A     DSP E2 command answer written to the PC memory, at this DMA offset ("not written" if no PicoMEM IRQ)

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
void sbdsp_iolog_init(void);    // Before core 1 starts
void sbdsp_iolog_add(uint8_t type, uint16_t port, uint8_t value);
void sbdsp_iolog_task(void);    // Main loop
#ifdef __cplusplus
}
#endif
#define IOLOG(type, port, value) sbdsp_iolog_add((type), (port), (value))
#else
#define IOLOG(type, port, value) do { } while (0)
#endif
