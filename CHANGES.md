# Changes

## HTTP access to the SD card

Browse, download, upload, delete files, and create folders on the PicoMEM SD card from a web
browser, while the PC runs. `src/httpd/`

- Started when the Wifi is connected and `http.txt` is on the SD card: static IP address, mask,
  gateway, and an optional password. Without `http.txt` the SD card is never touched.
- Own lwIP network interface on the CYW43, shared with the NE2000 emulation: only the frames for
  the PicoMEM's IP address go to lwIP, the others to the NE2000.
- Browser page embedded in the firmware; plain HTTP requests also usable from the command line
  (`/ls`, `/f` download / upload / delete, `/d` create a folder).
- Everything runs from the main loop, the SD card is only accessed by requests.
- The page shows the Wifi MAC address (`GET /mac`, no password needed).

## Firmware update from the SD card (PicoMEM 2.0)

Update the firmware without USB. `src/fwupdate/`

- `picomem.uf2` (the normal build output) at the SD root is installed at power on: checked first
  (RP2350 family, PicoMEM 2.0 tag, all blocks present, fits the slot), then written into the free
  slot of an A/B partition table, read back, and started. A refused file leaves the PicoMEM
  starting normally.
- The file is renamed `picomem.uf2.done` (or `.bad`), the result is written in `fwupdate.txt`. The
  `.done` / `.bad` of a previous update are deleted first: if one can't be deleted, the update
  is not tried (logged), so that `picomem.uf2` can be renamed after the update. A failed rename
  is logged too.
- "Try Before You Buy" images: the bootrom goes back to the previous firmware if the new one does
  not start; once confirmed, the other slot is invalidated (`PM_FW_TBYB` to build without it, for
  SWD debugging).
- Safe write order: the first sector (IMAGE_DEF) is written last, an interrupted update leaves no
  valid image in the slot. Flash written at the boot clock (the overclock flash timing is lost by
  the flash functions).
- One time setup with USB: copy `partition.uf2` (partition table, built with the firmware from
  `src/fwupdate/pm_partitions.json`), then `picomem.uf2`, in BOOTSEL mode.

## Band-limited Sound Blaster and Covox output, Direct DAC

Cleaner digitized sound. `src/audio_devices/bandlimit/`

- SB16 style "brickwall" filter: band-limited (windowed sinc, 48 taps) resampling to the output
  rate, the cutoff follows the sample rate (just below half of it), for the SB DMA and the
  Covox / Disney output. `USE_LPF` (1 on the PicoMEM 2.0, 0: sample-and-hold, no kernel in RAM).
- Output filter modes, switched at run time with `PMINIT /lpf x` (new command `CMD_FilterMode`, 7Fh),
  for the SB DMA, SB Direct DAC, Covox and Disney outputs:
  - 0 none, each sample is held until the next one.
  - 1 SB16, the band-limited filter. Default, not available with `USE_LPF=0`.
  - 2 SB Pro 2, the CT1345 mixer output low-pass (3.2 kHz, 2nd order Butterworth) and the analogue
    stage. Programs turn the low-pass on and off with mixer register 0Eh bit 5, as documented by
    Creative (on by default and after a mixer reset). With the SB 2.0, which has no mixer, the
    writes to the mixer ports still switch it, for the programs set up for an SB Pro. The low-pass only applies
    to the Sound Blaster, the Covox and Disney get the analogue stage.
  - 3 analogue stage only, the 8 kHz RC low-pass and the DC blocking of the SB Pro FM output stage.
- DSP Direct DAC (command 10h) emulated: each CPU write is time stamped by the ISA core and the
  DAC level is replayed at the real write times. Each output sample is the mean level over its
  20 us, then goes through the IIR filters of the mode. Same for the Covox DAC. No timer runs when
  the DAC is not written.
- The Direct DAC and the Covox DAC are not band-limited, they get the analogue stage in the SB16
  mode. Programs write them from a timer interrupt that other interrupts delay (Jill of the
  Jungle's FM music handler, every 16 samples), and the band-limited filter, which needs evenly
  spaced samples, made a step at each late write (crackle, about 400 per second in Jill).

## OPL3 accuracy, FM analogue output stage

- OPL emulation (DBOPL) checked against Nuked OPL3 (sample by sample, same register writes):
  - Fix: the carrier used the modulator output of the previous sample: FM with feedback is now
    identical to Nuked OPL3 (was 0.9 % of the samples).
  - Fix: OPL2 mode rhythm (drums) written as mono into the stereo buffer: garbled drums.
  - Hi-hat, snare and top cymbal as the chip: its noise generator (taps 0 and 14, 36 steps per
    sample, read by each drum at its slot; was MAME's, once per sample), the hi-hat / cymbal phase
    bits (`hh3 ^ tc5` was missing), the snare noise polarity, the cymbal phase (0x80 / 0x280, was
    0x100 / 0x300), the waveform table offsets (drums on waveforms 0, 5, 6, 7 were sign flipped or
    misplaced), phases before their step, the hi-hat using the previous cymbal bits. Drums 0.56 dB
    from Nuked OPL3 in 1/6 octave bands (3.2 dB before, 6.7 dB in the official PicoMEM firmware), +16 instructions per
    sample in rhythm mode.
  - Fix: waveform 7 started half a period off.
  - Fix: tremolo 0.96 % too fast and 0.2 dB too shallow (52 step table): the table now holds two
    chip periods (105 steps of 256 samples), exact rate and depth, same CPU.
  - Fix: vibrato one step ahead of the chip, and its depth rounded after scaling (the chip shifts
    the integer f-number offset): vibrato identical to Nuked OPL3, same CPU.
  - Envelopes step on the chip's shared envelope clock: key on / off start the rate counter at
    its phase (was counted from the key on), attacks use the exact rate table (the old best-match
    search was 1-2 % slow), rate 15 attacks are instant from rate 60 (was 62). 55 % of the
    envelope samples identical to Nuked OPL3 (was 35 %), release timing error 9.3 -> 1.7 ms. No
    per-sample cost.
  - Improved chip log-sine / exponential arithmetic (`WAVE_TABLELOG`): sine / waveforms 22 dB closer to the
    chip.
  - OPL3 mode: drums panned with their channel's C0 bits; second register array written to the
    first while the OPL3 mode is off, as the chip.
- FM analogue output stage (`OPL_FILTER`): DAC sample-and-hold + card RC low-pass (SB 1.x / 2.0
  12 kHz by default, SB Pro 8 kHz, or none as the SB16) and DC blocking coupling capacitor, 3
  multiplies per sample
- The FM output is now clipped to 16 bit (loud passages wrapped around) and added to the mix
  (previously it erased the MPU output).

## Sound Blaster Pro 2

The Sound Blaster can be emulated as a Sound Blaster Pro 2 (CT1600), with `T4` (or `T2`) in
`BLASTER` for `PMINIT /sb 1`, or `SB <port> <irq> <dma> 4` in `config.txt`. `T3` (default) stays a
Sound Blaster 2.0. New command `CMD_SetSBType` (71h), the `BLASTER` T value.

- DSP version 3.02. The SB Pro input mode commands A0h / A8h are accepted (recording is not emulated).
- CT1345 mixer on base+4 / base+5, with the registers documented by Creative. Voice x Master set the
  digital audio volume, MIDI x Master the FM volume (also on the AdLib ports, the same chip), left and
  right. 3 bits per channel from -46 dB to 0 dB, 4 is -11 dB. CD, Line and Mic have no source.
- Read back as on the CT1600 (hardware notes in `sbmix.cpp`): unused bits of the volumes read 1,
  0Eh reads 11h after a reset, 00h-1Fh mirror 00h-0Fh, 0Eh is mirrored at 0Fh, reading 00h / 01h
  reads 0Eh, writing them resets the mixer, nothing above 3Fh (reads FFh).
- When the card is installed, Voice, Master and MIDI are at the maximum (as set by SBP-SET in DOS), a
  mixer reset sets them to the documented 4 (-11 dB).
- SB Pro stereo output: with the mixer stereo switch on, the DMA bytes alternate between the two
  channels, starting with the right one as on the real card. Creative tells programs to play one
  silent byte first, which goes right, so their data starts on the left (Quake). The Apogee Sound
  System (Duke Nukem 3D, Rise of the Triad) plays no silent byte and swaps its channels on the SB
  Pro instead (`MV_SetReverseStereo` in its released source). The bytes left in the FIFO when
  stereo starts are dropped.
- The output filter switch (0Eh bit 5) is used by the SB Pro 2 filter mode (`PMINIT /lpf 2`).

## Multiple IRQ lines (PicoMEM 1.5 / 2.0)

- `IRQ` in `config.txt` chooses the PicoMEM IRQ among the jumpered lines: the BIOS IRQ detection
  still runs, so the other jumpered IRQs stay real ISA lines for the emulated devices (e.g.
  PicoMEM on IRQ 6, Sound Blaster on IRQ 7). Applied at the end of the detection (`CMD_IRQAck`).
- Fix: the PicoMEM IRQ was always raised on the IRQ 7 line (PicoMEM 2.0), whatever IRQ the BIOS
  chose: it collided with a Sound Blaster on IRQ 7 (the game's SB handler took it). Now the line
  of the BIOS IRQ. `src/pm_libs/isa_irq.cpp`
- Fix: `IRQ 6` in `config.txt` was ignored on the PicoMEM 2.0 (only 3, 5, and 7 accepted).

## Sound Blaster compatibility: Creative CT-VOICE.DRV (e.g. X-Wing)

Star Wars X-Wing (and other CT-VOICE.DRV games) now play their digitised sounds.

- DSP command E2h is answered by DMA into the PC memory (HW DMA boards): the board cannot drive a DMA
  write, so the PC writes the answer through the PicoMEM BIOS command mode and the 8237 registers
  are set as after a real transfer. CT-VOICE.DRV unscrambles its playback code with these answers
  (copy protection).<hr>**Note:** Needs the PicoMEM BIOS enabled (its IRQ handler does the writes, not
  available with `BIOS 0` / `OFF` in `config.txt`) and the PicoMEM IRQ on its own line.<hr>
- Fix: the DSP write-status never reported busy. Drivers waiting for the busy bit (CT-VOICE.DRV
  before stop / pause voice) waited 65535 reads, freezing the PC ~130 ms with the interrupts
  disabled (display, mouse, music). Busy 8 reads out of 16, as on a real DSP and in DOSBox.
- Fix: the DSP reported busy during the 80h silence, it now keeps taking commands (as in DOSBox).
- Fix: DSP command E2h fell through to "speaker on" (missing `break`): it was cancelled and its
  argument taken as the next command.

## Extended configuration (`PMINIT`, `config.txt`)

- The FM chip's two port sets are enabled separately: `/adlib` controls the AdLib ports (388h), new
  `/sbopl` controls the Sound Blaster FM ports (228h/229h). `/sb 1` still enables both, then
  `/adlib 0` and / or `/sbopl 0` disables them (in any order on the command line). Disabled SB FM
  ports are ignored and not answered (bus left free for an other FM card with real OPL chip). The FM emulation runs
  while one of them is on. New command `CMD_SBOPLOnOff` (7Eh), needs the Sound Blaster on.
- AdLib port from `BLASTER` `Fxxx` (e.g. `SET BLASTER=A220 I5 D1 T3 F380`, an UniSound convention), 388h without it, 
  for `/adlib 1` and the AdLib enabled by `/sb 1`. 8 port aligned, 100h to 3F8h, refused if used by
  an other device (new error `CMDERR_BADPORT`).
- `config.txt` `ADLIB` also takes a port: `ADLIB 380` (`ADLIB 1`: 388h, `ADLIB 0`: off), for the
  AdLib started at boot. Firmware only: the port is not in the configuration shared with the BIOS
  (its layout is fixed by the BIOS binary), the BIOS menu still shows the AdLib On / Off, the port
  is not saved in `picomem.cfg`.
- `config.txt` `SB`: If Sound Blaster started at boot, like `PMINIT /sb 1`. `SB 0` (off,
  default), `SB 1` (220h, IRQ 5, DMA 1) or `SB <port> [<irq> [<dma> [<type>]]]` (e.g. `SB 240 7 3`),
  the type is the `BLASTER` T value, 3 Sound Blaster 2.0 (default) or 4 Sound Blaster Pro 2,
- `config.txt` `SBOPL`: `SBOPL 0` to start it without its FM ports (default 1, enabled with `SB`).
- `config.txt` `JOY 0` / `JOY 1`: USB joystick on the game port
- `config.txt` `MPU 0` / `MPU 1` (330h) / `MPU <port>` (300, 320, 330, 340, 360): MPU-401 /
  General MIDI started (or stopped) at boot, without the line the MPU is left as it is at reboot
  (as before). Needs a firmware built with `USE_MPU=1` (logged otherwise).
- `config.txt` `CMS`, `TDY`, `MMB` (0 / 1 default port / port, set the BIOS menu options),
- `config.txt` `CVX` / `DSS` (Covox DAC / Disney Sound Source: 0 / 1 LPT1 378h / 2 LPT2 278h / port, only one of
   them can be enabled at the same time)
- `config.txt` `GUS 0` / `GUS <port> [<irq> [<dma>]]`
  (needs `USE_GUS=1`). Valid ports as in PMINIT. The whole card can be configured without PMINIT:

  ```
  SB 220 5 1 3   # Sound Blaster: 0 (off) / 1 (220 5 1) / port [irq [dma [type 3 SB 2.0, 4 SB Pro 2]]]
  SBOPL 1        # its FM ports, base+8/+9
  ADLIB 388      # 0 / 1 (388) / port
  CMS 0          # 0 / 1 (220) / 210-260
  TDY 0          # 0 / 1 (2C0) / C0, 1E0, 2C0, 300
  MMB 0          # 0 / 1 (300) / 220, 2F0, 300, 310, 320
  CVX 1          # Covox: 0 / 1 (LPT1) / 2 (LPT2) / 278, 300, 378, 3BC   (or DSS: Disney)
  GUS 0          # 0 / port [irq [dma]]
  MPU 330        # 0 / 1 (330) / 300, 320, 330, 340, 360
  JOY 1          # USB joystick
  ```
  `config.txt` is read at power on (not at a PC reset).

## Sound Blaster I/O log on the SD card

Debug builds (`SBDSP_IOLOG=1`) record what a program does with the emulated Sound Blaster in
`sbiolog.txt`. `src/audio_devices/sbdsp/sbdsp_iolog.c`

- SB port reads / writes with µs time stamps (repeated identical reads merged with a count), SB
  IRQs (on the ISA line or through the PicoMEM IRQ), DSP DMA starts, E2 answers.
- RAM ring buffer filled by the ISA core, written by the main loop when the program is quiet,
  the buffer half full, or 1 s after the last write (a program stuck polling a port is logged
  too: it never gets quiet and its merged reads never fill the buffer).
