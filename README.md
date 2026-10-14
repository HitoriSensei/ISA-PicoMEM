# PicoMemBlaster

PicoMemBlaster is a fork of the [ISA PicoMEM](https://github.com/FreddyVRetro/ISA-PicoMEM)
firmware for the **PicoMEM 2.0**. It focuses on the **Sound Blaster**, with better
**compatibility** with games and drivers and better **sound quality** of both its digital audio and
its FM (OPL) music. The Covox and Disney Sound Source outputs get the digital audio
improvements too.

It is based on the official PicoMEM 2 firmware from before the CD-ROM emulation. As of October 11,
2026, only that source is available.

While the PicoMEM 1.x (RP2040) firmware shares part of the changed
code (FM emulation, mixer, `config.txt`), only the PicoMEM 2.0 has been tested (I don't have access
to the PicoMEM 1.x hardware).

The audio output has been benchmarked against Nuked OPL3 (the FM part) and the real SB16 measured output curve (the digital part).
The results are documented in [PicoMemBlaster audio quality](docs/picomemblaster/AUDIO_QUALITY.md).

## Changes from the official PicoMEM firmware

### Sound Blaster digital audio (and Covox / Disney)

The 8-bit DMA sound got a few selectable filtering modes.

In SB16 filter mode (enabled by default) it is resampled through a 48 tap
windowed sinc whose cutoff follows the playback rate, similar to the "brickwall" filter of the real SB16 (just not as steep).

The SB Pro 2 filter applies a 3.2kHz low-pass + the analogue stage, matching the SB Pro 2 hardware.

The "analogue" filter applies a 8kHz RC low-pass and DC blocking, gently reducing the aliasing and harshness without making the sound dull, similar to many analogue stages or older sound cards. This filter is applied by default to FM output and Covox output.

The **output filter mode** can be changed at run time with `PMINIT /lpf x`:

- 0, no filter (sample-and-hold)
- 1, the SB16 filter (default)
- 2, the SB Pro 2 low-pass (3.2kHz, switched by the program through mixer register 0Eh) and the
  card analogue stage
- 3, the analogue stage alone (8kHz RC low-pass and DC blocking, as on the SB Pro FM output)

**Sound Blaster Pro 2 emulation** is available next to the default Sound Blaster 2.0 emulation. In
`config.txt`, add the type "4" after the port, IRQ and DMA (`SB 220 5 1 4`). In DOS, set `T4` in the
`BLASTER` variable (`SET BLASTER=A220 I5 D1 T4`) and run `PMINIT /sb 1`. With `T3`, or without a
type, the Sound Blaster 2.0 is emulated.
(Note: If you set up the card in `config.txt` you DON'T have to call `PMINIT`, unless you want to 
change the card characteristics in runtime.)

Software used to test the SB Pro 2 stereo emulation:

- Duke Nukem 3D (no channel reverse needed)
- Quake
- Epic Pinball
- Crystal Dream
- Wolfenstein 3D 1.4 (Uses mixer panning for "stereo")

**DSP Direct DAC (command 10h)** is supported. Every write is time stamped, and the mixer replays
the samples with the same timing. In the SB16 filter mode it only goes through the analogue stage
because the sinc filter needs a stable sample rate.

Software used to test:
- Jill of the Jungle (old ver)
- Doom2D

The **Covox DAC** is replayed the same way and no longer needs a 43kHz timer interrupt. The
**Disney Sound Source** (6993Hz FIFO) goes through the band-limited filter.

The **DSP reset response is faster**. The reset answer (AAh) is ready 2us after the reset instead of 100us,
so Crystal Dream, and other software that probes the reset status, finds the card on a fast CPU (Tested on P3 450MHz).

Added fix for **IRQ lines**. Previously the IRQ7 line could not be used for SB, because it collided with PicoMEM bios
IRQ. Now BIOS and SB can choose any of the IRQ lines.

### FM (OPL2 / OPL3)

The **OPL emulation was checked against Nuked OPL3**. Its fixes are listed in [FM fixes](#fm-fixes).

Added the gentle **analogue output stage** filter by default.

The **FM port sets are now separately configurable**. `PMINIT /sbopl` switches the Sound Blaster FM ports
(base+8/+9) and `/adlib` only the AdLib ports, so a real OPL card can be used next to the emulated
Sound Blaster. The AdLib port can be moved with `Fxxx` in `BLASTER` (e.g. `F380`).

### Cost

The main CPU cost is the SB16 filter. Below are the estimated CPU and RAM usage increases:

| Part | Extra load                                                                       |
|------|----------------------------------------------------------------------------------|
| Sound Blaster digital audio | +3.5% per playing stream (build with `USE_LPF=0` to restore the sample-and-hold) |
| FM | +1.8% with 9 voices playing, +2.8% with 18                                       |
| Covox DAC | +1.8% while a program writes it                                                  |
| RAM | +19KB, of which 12KB is the filter kernel                                        |

Details are in [PicoMemBlaster audio quality](docs/picomemblaster/AUDIO_QUALITY.md#cpu-and-ram-cost).

### Gravis UltraSound and General MIDI (MPU-401)

The GUS and MPU-401 / General MIDI emulation are not modified in this fork. They are only touched
by the FM mixing fix listed below and by the new `config.txt` `GUS` and `MPU` keys, which start them
at boot.

Both are untested in PicoMemBlaster and may or may not be affected by the other changes.

## Fixes

### Sound Blaster fixes

- Star Wars X-Wing and other games using Creative's CT-VOICE.DRV had no digitized sound. DSP command
E2h is now answered by DMA into the PC memory, which needs the PicoMEM BIOS.

- DSP command E2h fell through to "speaker on" because of a missing `break`. This cancelled it and
took its argument as the next command.

- The DSP write-status never reported busy, so drivers waiting for it froze the PC for about 130ms at
a time. It now reports busy during DMA playback as a real SB or SB Pro DSP does, and ready without
DMA. Jill of the Jungle needs the ready state because it drops its Direct DAC sample when the DSP
is busy. Crystal Dream finds the end of each DMA block when the bit stops changing.

- The DSP reported busy during the 80h silence command and stopped taking commands.

- On the PicoMEM 2.0 the PicoMEM IRQ was always raised on the IRQ 7 line, whatever IRQ the BIOS
chose. It collided with a Sound Blaster on IRQ 7.

- `IRQ 6` in `config.txt` was ignored on the PicoMEM 2.0.

### FM fixes

- OPL2 rhythm mode drums were written as mono into the stereo buffer and came out garbled.

- The hi-hat, snare and top cymbal used a different noise generator, phase bit formula, snare noise
polarity and cymbal phase than the chip.

- Drums using waveforms 0, 5, 6 or 7 were sign-flipped or read from the wrong part of the waveform
table.

- OPL3 drums ignored the left and right panning of their channels.

- The FM carrier used the modulator output of the previous sample. This changed the sound of every
FM voice with feedback.

- Waveform 7 started half a period off.

- Tremolo, Vibrato and Attacks fixes, described in detail in [PicoMemBlaster audio quality](docs/picomemblaster/AUDIO_QUALITY.md).

- The sine and the other waveforms used linear arithmetic instead of the log-sine and exponential
tables of the chip.

- Loud FM passages wrapped around (overflow) instead of clipping.

- The FM output overwrote the General MIDI (MPU) output in the mixer instead of being mixed with it.

## Additional features

### Configuration without PMINIT

`config.txt` can set up the whole card at power on with the `SB`, `SBOPL`, `ADLIB <port>`, `CMS`,
`TDY`, `MMB`, `CVX` / `DSS`, `GUS`, `MPU` and `JOY` keys.

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

With this, the whole card can be configured without `PMINIT`.

### Wireless SD card access and firmware updates

- **SD card access over Wi-Fi**. It needs a one-time set-up with an `http.txt` file on the SD card
([example](#sd-card-access-via-wifi-http)). The Wi-Fi must first be configured as described in
[ne2000 emulation via Wi-Fi](#ne2000-emulation-via-wifi).

- **Firmware update from the SD card** (PicoMEM 2.0 only). Needs one-time `partition.uf2` flash (see [One-time setup](#One-time-setup)). Put the `picomem.uf2` at the root of
the SD card and restart the PC. The PicoMEM installs it into the free slot of an A/B flash layout
and rolls back a firmware that does not start. The progress is displayed on the OLED display (if you have one).

### Development and debugging

The **Sound Blaster I/O log** helps to find compatibility problems. A firmware built with
`PM_SBDSP_IOLOG=1` records what a program does with the emulated Sound Blaster in the `sbiolog.txt` on
the SD card ([details](#sound-blaster-io-log-on-the-sd-card-picomem-20)). It is off in normal
builds because it uses 24KB of RAM and its SD card writes can disturb the audio.

`PM_FW_TBYB=0` builds a firmware without the "Try Before You Buy" flag, to load it with a debugger
(SWD).

### SD card access via Wifi (HTTP)

When the Wifi is connected, the PicoMEM can serve the SD card over HTTP. The Wifi chip is shared with
the ne2000 emulation, so the server uses its own **static IP address**.

### Set-up

Create an `http.txt` file at the root of the SD card with one value per line:

```
# PicoMEM HTTP server
# IP address (static, free on the network)
192.168.1.50
# Network mask
255.255.255.0
# Gateway
192.168.1.1
# Password (optional, up to 39 characters, letters and digits are safest because it is sent in the URL)
retro486
```

Without the `http.txt` the HTTP server is not started.

### Use

Open `http://192.168.1.50/` in a browser. From the command line, upload with
`curl -T file.zip "http://192.168.1.50/f?p=/file.zip"` and download with
`curl -o file.zip "http://192.168.1.50/f?p=/file.zip"`. Add `&k=PASSWORD` when a password is set.

The PicoMEM main loop reads and writes the files, so a transfer may briefly disturb the audio. Don't
write to the disk image files of the emulated hard disk while the PC is running as this may cause an 
FS corruption.

## Firmware update from the SD card (PicoMEM 2.0)

### One-time setup

The partition table and then the firmware are installed over USB in BOOTSEL mode.

Copy the `partition.uf2` to the RP2350 drive. The board should come back in BOOTSEL mode.
If it doesn't, reconnect it with BOOTSEL pressed.
The`picotool partition info` command should then show the A and B partitions.

Copy the `picomem.uf2` to the RP2350 drive.

### Update

Copy the `picomem.uf2` (the normal build output) to the root of the SD card, or upload it from the
browser with the HTTP server. Then restart the PicoMEM.

When PicoMEM starts, it begins the flash process (you should see the "Updating..." message in the OLED if you use one). The update process should not be noticed by the PC as it is
still in its own POST. However, if the PC reached the PicoMEM BIOS during the update and boots without the PicoMEM or hangs, the PC be **restarted**.

After the flash is complete (or aborted), the FW file is renamed to `picomem.uf2.done` or `picomem.uf2.bad`, and the result is written in the
`fwupdate.txt`.

Note: Copying the `picomem.uf2` in BOOTSEL mode still works too.

## Sound Blaster I/O log on the SD card (PicoMEM 2.0)

A firmware built with `cmake -DPM_SBDSP_IOLOG=1` records what a program does with the emulated
Sound Blaster. The log goes to the `sbiolog.txt` at the root of the SD card, rewritten at each power
on.

The log holds the port reads and writes, the DMA controller (8237) writes, the DSP DMA starts and
the Sound Blaster IRQs with the line they use. Each entry has a time stamp in µs.

The file is written after the program is quiet for 0.5s, so wait a moment before switching off. The
log uses 24KB of RAM, and its SD card writes can briefly disturb the audio.

---

*The original PicoMEM documentation follows.*

# ISA PicoMEM Extension board (For 8086/8088 PC)

**PicoMEM 1.14:**<br />
<a href="url"><img src="https://github.com/FreddyVRetro/ISA-PicoMEM/blob/main/jpg/PM114_Close.jpg" align="middle" height=40% width=40% ></a>

## Introduction

The PicoMEM is designed as a way to run Emulated ISA boards on a real PC.<br />
The PicoMEM Board currently connect the full 8Bit Memory and I/O Bus plus an IRQ to a Raspberry Pi Pico, through some multiplexor/Level shifter chip.<br />
The Pi Pico also has a 8Mbyte PSRAM connected in SPI and a MicroSD Slot.<br />

The PicoMEM Board can be seen as both a working PC extention board as well as a Development platform.

Full Firmware and drivers sources available.<br />
**PicoMEM Hardware is not open.**<br />

**NEW : SD and USB disk direct access** A Network redirector driver allow full access to the SD and USB for DOS 3.2 + !<br />
The PicoMEM is now the only board providing full USB drive in FAT23/ExtFS from DOS.

To follow the PicoMEM news, see it in action on multiple machines you can follow me on X:<br />
[FreddyV on X](https://x.com/FreddyVETELE)

To see my other projects, here is my Youtube Channel:<br />
[FreddyV Retro Zone](https://www.youtube.com/@freddyvretrozone2849)

### How to get a PicoMEM ?

**The PicoMEM is available at Texelec ! (USA / World)**
[Texelec PicoMEM link](https://texelec.com/product/picomem/?highlight=picomem)

**The PicoMEM at Serdashop (Europe / World)**
[Serdashop PicoMEM](https://www.serdashop.com/PicoMEM)

**The PicoMEM at Flamelily Shop (UK /World)**  PicoMEM 2 Available !
[Flamelily PicoMEM link](https://shop.flamelily.co.uk/picomem)

You can have one from me (Mainly for Europe, with wait time), on my form:
[FreddyV PicoMEM Form](https://docs.google.com/forms/d/e/1FAIpQLScwYPvnVoGynLLgP_hiLMH_qn9uBX1sxims7Ah4LabjQ0mSLw/viewform)


### How to use the PicoMEM ?

Please go to the Wiki page : https://github.com/FreddyVRetro/ISA-PicoMEM/wiki

## Board description

The PicoMEM exist in 4 Versions : 1.0, 1.1, 1.11, 1.14 and 1.2A<br />

**PicoMEM variant :**

PicoMEM 1.2A   : It is a PicoMEM 1.14 with the DAC Added on Board, DAC Added by Serdaco.<br />
PicoMEM LP 1.0 : PicoMEM variant designed for Low profile ISA Slot, like on the Sinclair PC200.<br />
PicoMEM 2.0    : New PicoMEM with RP2350 soldered onboard, DAC, RTC, front panel connector, USB A.<br />

**Picomem LP (Low profile):**<br /> 
<a href="url"><img src="https://github.com/FreddyVRetro/ISA-PicoMEM/blob/main/jpg/PM10LP.jpg" align="middle" height=30% width=30% ></a>

**Hardware :**
  - 2 Layers ISA Board connecting a Raspberry Pi Pico through some buffers/Multiplexor and inverter.
  - The Pi Pico is connected to the full Memory and I/O Address space, and one IRQ. (No DMA)
  - The Pi Pico is connected to a PSRAM (8Mb, 100/130MHz, PIO) and a MicroSD (12/24MHz, SPI) slot in SPI.
  - The Pi Pico is overclocked at 270MHz.
  - The MicroSD connector share the SPI BUS of the PSRAM, adding some limitations. (We need to stop the PC IRQ during Disk Access)
  - One IRQ Line that can be connected to IRQ 3 or 5 for Rev 1.0, 2,5 or 7 for Rev 1.1.
  - QwiiC Connector (SPI) (Added to V1.1)

**Software :**
  - A Full BIOS with a "Phoenix BIOS Like" text interface in assembly.
  - C/C++ Code, PIO and ARM assembly on the Pi Pico.
  - Multiple other projects library used. (List below)
  - The PC can send multiple command to ask the Pico to perform tasks.
  - In the reverse, the Pico can also send commands to the Pico.

## Current Functionality

- **Memory emulation with 16Kb Address granularity:**<br />
   128Kb of RAM can be emulated from the Pi Pico internal RAM with No Wait State.<br />
   It can emulate the whole 1Mb of RAM address space from the PSRAM. (With 4-5 Wait States added)<br />
   4MB of EMS Emulation.<br />
   Memory emulation is used to add 8Kb of "Private" memory for the PicoMEM BIOS Usage.<br />
   PicoMEM Disks data transfer done via the emulated Memory.<br />

- **ROM Emulation** for its internal BIOS and custom ROM loaded from the MicroSD. (Custom ROM not implemented yet)<br />
   The Board has its own BIOS, used to automatically detect/Extend/Configure the RAM emulation and select Floppy/Disk images.
- **Floppy and Disk** "emulation" from .img files stored in uSD through FasFs and DosBOX int13h emulation code.
   Emulate 2 Floppy and 4 Disk (80h to 83h), Disk up to 4Gb (More later)
- **SD and USB disk direct access** A Network redirector driver allow full access to the SD and USB for DOS 3.2 +
   As it is a network redirector, the SD and USB filesystem can be anything (Even FAT32/ExtFS)
- **USB Mouse** support through a USB OTC Adapter. (Micro USB to USB A or USB Hub)
- **POST Code** (Port 80 Display in Hexa) via the QwiiC connector: https://www.sparkfun.com/products/16916
- **ne2000 network card** emulation via Wifi. (Pico W PicoMEM only)
- **USB Joystick** for PS4 and Xinput controllers.
- **Tandy 1000** (Old models with Tandy Graphic) now supported, even for RAM upgrade.
- **NEW** First Pico controlled application : RAM/ROM Dump.

**Audio/Sound cards emulation :**<br />

- **Adlib** using a PCM5102 I2S module.<br />
- **CMS/Game Blaster and Tandy** sound chip emulation.<br />
- **The extremely rare Mindscape Music Board** sound card is now emulated. (Dual AY-3-8910)<br />
- **Covox (8Bit DAC on parallel port)** sound device emulated. (LPT1/LPT2)<br />
- **Sound Blaster**  Done via a DMA emulation (Memory copy via an interrupt), the compatibility is low<br />
- **General MIDI** Onboard General MIDI rendered on the PicoMEM 2 (TinySoundFont)<br />
- **PS/1 and Covox** emulation in progress<br />

## Future Functionality

- There is already a mechanism implemented, so that the Pi Pico can send command to the PC, we can have the Pi Pico taking "Virtually" the control of the PC.
  > This can be used to perform ROM/RAM dump, Disk/Floppy DUMP/Write, display/kb redirection.... (RAM/ROM DUMP is done)
- More USB Host to be added  (Keyboard, MIDI...)
- Bluetooth support for device like Gamepad may be added.
- Use of the Qwiic connector for more information display (OLED), maybe RTC and other.

## PicoMEM 2

PicoMEM 2 is the evolution of the PicoMEM with on board RP2350B chip.<br />
It can be used like a PicoMEM 1 and removed most of its limitation.

<a href="url"><img src="https://github.com/FreddyVRetro/ISA-PicoMEM/blob/main/jpg/PM202_Close.jpg" align="middle" height=40% width=40% ></a>


**Differences with the PicoMEM 1.14 :**

**Performance increase / New hardware:**

- Global performance increase thanks to the RP2350 at 360MHz (Vs RP2040 at 240MHz)
- Zero wait RAM emulation moved from 128KB to 256KB
- IOCHRDY Signal generation (used for PSRAM Memory emulation) moved from 120ns to 40ns, allowing for more PC to support EMS.
- Global RAM/IO performance increase for more PC compatibility.
- Now use QSPI PSRAM, for faster RAM/EMS emulation (only 1 Wait state)
- Support DMA from PicoMEM emulated RAM (Except when the PicoMEM does DMA)
- Multi IRQ support : IRQ 3,5,6,7 are independant
- DMA Support : Can select DMA 1 or 3 Via jumper on board
- New : Real Time Clock with a battery for RTC support
- New : Wifi antenna is now "outside" for improved Wifi signal. (More than 10dB better)

**Connectivity / panels:**

- USB A Connector for direct Joystick/Mouse/USB drive connection, or USB Hub
- Serial port (TX/RX) + LED Connector foe external LED connection or HDDClicker.
- 2x QwiiC connectors that can be used at any moment for possible Upgrade.
- 1x Front Panel connector for OLED + 5 Buttons
- SSD1306 128x32 OLED Screen on the front panel now display POST Code / Logos

**Audio improvements:**

 - DAC is now onboard, with better sound quality (4 Layers routing) and Jack connector
 - Sound Blaster code now use Hardware DMA for greatly improved compatibility.
 - Gravis UltraSound support Added, 1MB RAM, DMA and IRQ.
 - MPU / General MIDI via a modified TinySoundFont implementation (https://github.com/schellingb/TinySoundFont)
 - As all emulation can be active at the same time, you can configure General MIDI + GUS on DOOM for example.
 - Now generate Stereo OPL3 instead of OPL2

**PicoMEM 2 exclusive fonctionality:**
 - **NEW**: CD-ROM and CD Audio emulation
 - **NEW**: CD Audio player on the front panel
 - **NEW**: Option ROM emulation (via config.txt)

**Other updates:** (possible on PicoMEM 1)
 - More Joystick supported (DirectInput, PS4/PS5, Switch/Switch2... )  (https://github.com/joypad-ai/joypad-os)
 - Added support for variable disk geometry (CHS) via a "imagename".chs file
 - Load images from a sub Folder added

**Planned:**
- USB RJ45 Dongles
- USB MIDI / MPU401
- General MIDI sound font loading

## Alternative Firmware:

The PicoGraph firmware transform the PicoMEM 2 to a Video card, using USB Displaylink (V2)<br />
It can emulate MGA, Hercule, CGA, VGA, Cirrus Logic CL-GD5429 and the Sony DTL-H201A PSX GPU board.<br \>
**Link:** https://github.com/ianhan/picograph<br />

## Compatibility/Limitations

**PicoMEM 1 limitations:** 

The Board can't be used for Video emulation, as it require a way for the Pico to actually display something, and only 3 pins are "Free".<br />

The Pi Pico is limmited in its speed, this is excellent and bad at the same time:
- Multiple complex function can't be emulated at the same time, choices need to be done.
- We can still program the Pico and Feel like doing coding on a "Retro" Machine, so we don't have the effect "Look, it is easy, he put a processor in the PC that can emulate the full PC"
- There are still problems on various machines.

**PicoMEM 2 limitations:**<br />

Most of the PicoMEM 1.14 limitations are now gone with the PicoMEM 2<br />

The DMA treansfer (To the PicoMEM) does not work when the data to transfer in in the PicoMEM RAM (For the moment)<br />

## Memory emulation details :

As the PicoMEM is an 8Bit ISA Card, the RAM Emulation is most of the time slower than the PC own RAM.
The PicoMEM is then more suitable to extend a **512KB PC to 640KB**, Add some UMB (For Driver) than **extend an IBM PC With 128Kb of RAM**.
(Other RAM Card for 5150/5160 is still recommended, as full 0Wait State)

### Memory emulation capabilities :

- The PicoMEM BIOS auto detect (At Hardware level) the 1st Mb of RAM and Display the RAM MAP with a "Checkit" like Display.
- You can select to add RAM from the Internal SRAM (128Kb with No Wait State)
- Emulation from the PSRAM (External RAM) can add RAM to any Address Space. (2x slower than SRAM emulated RAM)
- EMS Emulation emulate a LOTECH Board with Up to 4Mb.
- Disk access on PM RAM emulated memory are done at the Pico uSD speed. (3Mb/s)

### Memory emulation limitations :

**PicoMEM 1:**<br />
- Memory emulation with PSRAM is quite slow.<br />
- **The emulated Memory does not support DMA**, Will be corrected in future PicoMEM HW Release.<br />
  Then :
    - As the real floppy use DMA, **you should disable temporarily the RAM emulation** if the real Disk access are not working.
    - For SoundCard, if the PC has 512Kb of base RAM, it is really unlikely that the DMA Buffer will be placed in emulated RAM, it may work 90% of the time.
    - Boot/Use of MFM / XT Hard Drive with DMA will fail.

**PicoMEM 2:**<br />
 - One Wait State for PSRAM emulated memory (RAM above 256Kb or EMS)<br />

## Disk "emulation"

The PicoMEM can add 4 disks to the BIOS, Disk up to 4Gb.<br />
It can also mount Floppy image as A: or B:

**The PicoMEM does not emulate disks**, it send the BIOS Disk access commands to the Pi Pico. <br />
Then, it is more a "Disk BIOS" emulator than a Disk emulator. <br />

**Full SD and USB Disk access**
The PicoMEM can access the full MicroSD and USB drive connected to it, even if formatted in FAT32, from DOS 3.2 !<br />
Thanks to a modified EtherDFS driver and an embedded EtherDFS server, the MicroSD and any USB key can be seen as a network drive.<br />

Another particularity is that it use Memory to perform the Data transfer, this allow for the maximum possible transfer speed, even on 8088 CPU. <br />
Anyway, single sector read is slower than multiple sector read as the PicoMEM need to read the sector from the SD, then send the data to the PC Memory. <br />
With multiple sector Read, the Pico read the next sector while the data is copied to the PC Memory. <br />

It is highly recommended to use reasonable disk image size, below 500Mb. <br /> 
100 or 200Mb are ideal size, as it will be recognized and usable by DOS 6 and DOS 3.31 <br />
As you can add 4 Disk with 4 partitions, you can create diferent disk images for different usage, like a partition with games, another with music ... <br />

**Warning :** 
- You may have compatibility problem with some uSD, even if the compatibility has been greatly improved in the November 2023 firmware.<br />
- Support only SDHC SD Cards.<br />

## ne2000 emulation via Wifi

The PicoMEM can emulate a ne2000 network card via Wifi.<br />
**Warning :** As the Wifi antenna is inside the PC case, the connection quality may be low. (Not valid on PicoMEM 2)<br />

**How to use it :**
- Create a wifi.txt file with the SSID in the first line and the Password in the 2nd line.<br />
- Use ne2000.com 8Bit (command line : ne2000 0x60 0x3 0x300) or pm2000 (command line : pm2000 0x60)<br />
- The Wifi Access point need to be relatively close to increase the chance of connection success. The IRQ Can be changed in the BIOS Menu (Default is IRQ 3)<br />

## Tested machines :
- IBM 5150, 5160, 5170
- IBM PS/1, IBM PS/2 30 286, PS2 8535 (Warning : As its HDD use DMA, does not boot if emulated RAM is added)
- Compaq Portable 2 (286): Ok<br />
- **New : Tandy 1000** : Now tested on a Tandy 1000 SX, EX and HX other to confirm. (October 2024 firmware)<br />
- Tandy 1000 RLX Rev B Need a new BIOS to work.<br />
- Amstrad PC1512, PC1640, Sinclair PC200 (It is my DEV Machine)<br />
- Schneider EuroPC1:  PSRAM/EMS Not working on PicoMEM 1, Ok on the PicoMEM 2
- Schneider EuroPC2, Olivetti M21, Olivetti M24, Sega TeraDrive.<br />
- Commodore PC1, PC10/PC20. (Need Fast RAM Firmware)<br />
- Worked on Various 486, 386, 286 (Has more chance to work with lower ISA Clocks)<br />
- Tested with some Pentium, Pentium MMX, Pentium 2, AMD K6 ...<br />
- Amiga 2000 with a A2088 (PC Config A000) A2286/A2386 SX Board. (Use the BIOS in C800)
- Book8088 (At 4.77MHz and 8MHz with the latest firmware), **The Power need to be connected at 8MHz**. 
  EMS/PSRAM based RAM is failing. (Working with PicoMEM 2)
- Amstrad PPC512/PPC640 : No more problem with the PicoMEM 2

## Failing Machines :
- The number of failing machines is decreasing, if it does not work on your machine, you can post an issue in this GitHub repository.
- Fail on some fast 286 (16MHz)
- IBM PS/2 M25
- Amstrad PPC512 /PPC640 : Problem on some PPC (Can't Boot) Not sure if it depends on the external ISA connectors boards used. <br />
  > Problem identified on some ISA expantion boards, 100uF Low ESR filtering capacitors may be needed.
- NuXT / EMM8088 homebrew computers seems to have problem. (NEW: EMM8088 working with the last GlaBIOS) <br />
- Tandy 1000 RXS Fail, but work if the BIOS is updated (See the Wiki)
- Pocket8086 : IO Port error
- Pocket386  : A BUG on this machine ISA BUS prevent the PicoMEM and any BIOS Based board to boot.

## Known bug :

## License

Software is Available on this GitHub as GNU 2. <br />
Hardware is currently closed. <br />

## Contributors / External Libraries

* [Ian Scott](https://github.com/polpo/): Idea to use the Pi Pico instead of the not available Pi 2/4 and Zero plus help on the Hardware design.
* [PSRAM Code by Ian Scott](https://github.com/polpo/rp2040-psram) : PSRAM PIO Code.
* [FatFS by Chan](http://elm-chan.org/fsw/ff/00index_e.html) : ExFS Library for microcontrollers.
* [FatFS SD by Carl J Kugler III](https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico) : FatFS for SD Access in SPI with a Pi Pico
* DOSBox (www.dosbox.com): DosBOX BIOS Int13h Code, heavily modified for ExFS support and PicoMEM Interface, with bug correction.
* Most of the Sound Blaster code and ne2000 emmulation code port to Pico by yyzkevin.
* [XInput library](https://github.com/Ryzee119/tusb_xinput.git) : XInput USB Joystick.
* [Mitsutaka Okazaki / Graham Sanderson emu8950 v1.1.0] (https://github.com/digital-sound-antiques/emu8950) : Adlib/OPL2 emulation
* Aaron Giles DREAMM / SCUMM Games emulator : CMS / Tandy emulation
* Fast disk access TinyUSB branch by Artem Vasilev (wbcbz7) (https://github.com/wbcbz7/tinyusb/)
* KME CD device and Sound Blaster DSP by yyzkevin.
* [Etherdfs by Mateusz Viste](https://mateusz.fr/etherdfs/)
* [TinySoundFont by Bernhard Schelling](https://github.com/schellingb/TinySoundFont)
