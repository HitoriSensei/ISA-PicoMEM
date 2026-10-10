# PicoMemBlaster

PicoMemBlaster is a fork of the [ISA PicoMEM](https://github.com/FreddyVRetro/ISA-PicoMEM) firmware for
the **PicoMEM 2.0** that focuses on the **Sound Blaster**: compatibility with more games and drivers,
and sound quality of both its digital audio and its FM (OPL) music. The Covox and Disney Sound
Source outputs get the same digital audio quality improvements.

It is based on the official PicoMEM firmware for the PicoMEM 2 (release of October 4 2026), before
the CD-ROM emulation work (not released in the official PicoMEM firmware yet, `USE_CDROM=0`).

Only the PicoMEM 2.0 has been tested. The PicoMEM 1.x (RP2040) builds share part of the changed
code (FM emulation, mixer, `config.txt`) but have not been built or tested with these changes.

- What it sounds like, measured: [PicoMemBlaster audio quality](docs/picomemblaster/AUDIO_QUALITY.md)
  (Sound Blaster filtering against a real SB16, FM against the Nuked OPL3 chip emulator).
- Every change in detail: [CHANGES.md](CHANGES.md).

## Changes from the official PicoMEM firmware

### Sound Blaster digital audio (and Covox / Disney)

- **SB16 style band-limited output**: 8 bit DMA sound is resampled through a 48 tap windowed sinc
  whose cutoff follows the playback rate, as the real SB16's brickwall filter. Images and aliases
  above half the rate drop from 5-10 dB down to about 50 dB down, the treble droop of the old
  sample-and-hold is gone.
- **DSP Direct DAC (command 10h)** emulated: each write is time stamped and replayed at its real
  timing through the same filter.
- **Covox DAC / Disney Sound Source** through the same band-limited filter; the Covox DAC no longer
  needs a 43 kHz timer interrupt.
- **Creative CT-VOICE.DRV support (Star Wars X-Wing, ...)**: DSP command E2h answered by DMA into the
  PC memory (needs the PicoMEM BIOS), E2h no longer falls through to "speaker on".
- **No more freezes while samples play**: the DSP write-status reports busy as a real DSP (drivers
  waiting for it froze the PC for about 130 ms), the 80h silence no longer reports busy.
- **Multiple IRQ lines**: choose the PicoMEM IRQ among the jumpered lines (`IRQ` in `config.txt`, now
  including IRQ 6) and keep the others for the Sound Blaster; the PicoMEM IRQ is raised on its own
  line (it collided with a Sound Blaster on IRQ 7).

### FM (OPL2 / OPL3)

- **OPL emulation checked against Nuked OPL3** and fixed: rhythm mode drums (garbled in the official PicoMEM firmware, now
  the chip's noise generator and hi-hat / snare / cymbal phases), FM
  modulation timing (feedback FM now sample identical), the chip's log-sine / exponential
  arithmetic, waveform 7, vibrato and tremolo (exact rate, depth and phase), envelopes on the chip's
  shared clock, OPL3 drum panning and second register array.
- **Analog output stage**: DAC sample-and-hold, the card's RC low-pass (Sound Blaster 1.x / 2.0 by
  default, Sound Blaster Pro, or none as the SB16) and DC blocking (`OPL_FILTER`).
- FM output clipped instead of wrapping around on loud passages, and mixed with (no longer erasing)
  the MIDI output.
- **Separate FM port sets**: `PMINIT /sbopl` switches the Sound Blaster FM ports (base+8/+9),
  `/adlib` only the AdLib ports, so a real OPL card can be used next to the emulated Sound Blaster.
  The AdLib port can be moved with `Fxxx` in `BLASTER` (e.g. `F380`).

### Cost

Extra CPU load compared with the official PicoMEM firmware, as a share of the processing power of
one RP2350 core running at 360 MHz (the core that mixes the audio; estimated from instruction
counts):

- Sound Blaster digital audio: +3.5 % per playing stream, the band-limited filter (`USE_LPF=0`
  restores the official PicoMEM firmware's sample-and-hold).
- FM: +1.8 % with 9 voices playing, +2.8 % with 18.
- Covox DAC: +1.8 % while a program writes it.
- RAM: +19 KB (12 KB of it is the filter kernel).

Details in [PicoMemBlaster audio quality](docs/picomemblaster/AUDIO_QUALITY.md#cpu-and-ram-cost).

### Configuration without PMINIT

`config.txt` can now set up the whole card at power on: `SB`, `SBOPL`, `ADLIB <port>`, `CMS`, `TDY`,
`MMB`, `CVX` / `DSS`, `GUS`, `MPU`, `JOY` (see [CHANGES.md](CHANGES.md)).

### Wireless SD card access and firmware updates

- **SD card access from a web browser** (Wi-Fi): browse, download, upload, rename and delete files and create
  folders on the PicoMEM's SD card from any computer on the network, while the PC runs. Needs initial one-time set-up with
  an `http.txt` file on the SD card (static IP address, mask, gateway, optional password,
  [example](#sd-card-access-via-wifi-http)); the page
  also shows the Wi-Fi MAC address so you could set up a static DHCP on the router. The Wi-Fi must be first configured as described in the
  [ne2000 emulation via Wi-Fi](#ne2000-emulation-via-wifi)
- **Firmware update from the SD card** (PicoMEM 2.0): put `picomem.uf2` at the root of the SD card
  and restart the PC, the PicoMEM installs it into the free slot of an A/B flash layout. A firmware
  that does not start is rolled back automatically.

Together they give a **fully wireless update path**: upload the new `picomem.uf2` from a browser,
restart the PC, done. There is no need to open the PC to reach the BOOTSEL button or connect USB to
the card. Physical access is needed only once, for the first install: `partition.uf2` (the A/B
partition table, built next to the firmware) and then `picomem.uf2`, copied in BOOTSEL mode.

### Development and debugging

- **Sound Blaster I/O log**: firmware built with `SBDSP_IOLOG=1` records what a program does with
  the emulated Sound Blaster (port reads / writes, 8237 DMA writes, IRQs, DSP DMA starts, with µs
  time stamps) in `sbiolog.txt` on the SD card. For finding compatibility problems; it uses 24 KB
  of RAM and its SD card writes can disturb the audio, so it is off in normal builds.
  [Details](#debug--sound-blaster-io-log-on-the-sd-card-picomem-20)
- `PM_FW_TBYB=0` builds a firmware without the "Try Before You Buy" flag, to load it with a
  debugger (SWD).

### Gravis UltraSound and General MIDI (MPU-401)

The GUS and MPU-401 / General MIDI emulation are not modified in this fork. The only change that
touches them is a side effect of the FM work: the FM output used to overwrite the MIDI output in
the mixer, it is now added to it. The new `config.txt` `GUS` and `MPU` keys only start them at boot.

Both are untested in PicoMemBlaster and may or may not be affected by the other changes.

## Fixes

Bugs of the official PicoMEM firmware fixed in PicoMemBlaster.

### Sound Blaster fixes

- Star Wars X-Wing and other games using Creative's CT-VOICE.DRV had no digitized sound: DSP
  command E2h is now answered by DMA into the PC memory (needs the PicoMEM BIOS).
- DSP command E2h fell through to "speaker on" (missing `break`), cancelling it and taking its
  argument as the next command.
- The DSP write-status never reported busy, so drivers waiting for it froze the PC for about
  130 ms at a time (display, mouse and music stuttered while samples played).
- The DSP reported busy during the 80h silence command and stopped taking commands.
- On the PicoMEM 2.0 the PicoMEM IRQ was always raised on the IRQ 7 line, whatever IRQ the BIOS
  chose, colliding with a Sound Blaster on IRQ 7.
- `IRQ 6` in `config.txt` was ignored on the PicoMEM 2.0.

### FM fixes

- OPL2 rhythm mode drums were written as mono into the stereo buffer and came out garbled.
- The hi-hat, snare and top cymbal used a different noise generator, phase bit formula, snare noise
  polarity and cymbal phase than the chip.
- Drums using waveforms 0, 5, 6 or 7 were sign-flipped or read from the wrong part of the waveform
  table.
- OPL3 drums ignored their channels' left / right panning.
- The FM carrier used the modulator output of the previous sample, changing the sound of every FM
  voice with feedback.
- Waveform 7 started half a period off.
- Tremolo ran 0.96 % too fast and 0.2 dB too shallow.
- Vibrato ran one step ahead of the chip and its depth was rounded the wrong way.
- Attacks were 1-2 % too slow, and rate 15 attacks on low notes were not instant.
- Writes to the OPL3 second register array while the OPL3 mode was off did not go to the first
  array as on the chip.
- Loud FM passages wrapped around (overflow) instead of clipping.
- The FM output overwrote the General MIDI (MPU) output in the mixer instead of being mixed with it.

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

## SD card access via Wifi (HTTP)

When the Wifi is connected, the PicoMEM can serve the SD card to a web browser : browse, download, upload, rename, delete files and create folders.<br />
The Wifi chip is shared with the ne2000 emulation, so the server uses its own **static IP address**.<br />

**How to use it :**
- Create a http.txt file at the root of the SD card, one value per line (empty lines and lines starting with # are ignored) :
  1. IP address (ex: 192.168.1.50)
  2. Network mask (ex: 255.255.255.0)
  3. Gateway (ex: 192.168.1.1)
  4. Password (optional)

  Example `http.txt`:
  ```
  # PicoMEM HTTP server
  # IP address (static, free on the network)
  192.168.1.50
  # Network mask
  255.255.255.0
  # Gateway
  192.168.1.1
  # Password (optional, up to 39 characters, letters and digits are safest: it is sent in the URL)
  retro486
  ```
  Comments must be on their own line, starting with `#` at the start of the line: anything after
  a value is read as part of it. Remove the last line to have no password.
- Open `http://192.168.1.50/` in a browser. Command line : `curl -T file.zip "http://192.168.1.50/f?p=/file.zip"` (upload) and `curl -o file.zip "http://192.168.1.50/f?p=/file.zip"` (download). Add `&k=PASSWORD` if a password is set.
- Without http.txt nothing is started and the SD card is never touched by the server. Without a password anybody on the network can modify the SD card.
- Files are read/written by the PicoMEM main loop, so a transfer may briefly disturb the audio. Don't write to disk image files used by the emulated hard disk while the PC is running.

## Firmware update from the SD card (PicoMEM 2.0)

The PicoMEM 2.0 can update its firmware from the SD card, without USB. The flash is split in two firmware slots (A/B) : the new firmware is written in the free slot, and the bootrom goes back to the previous one if the new one does not start.<br />

**One time setup (USB, BOOTSEL mode) :** install the partition table, then the firmware.
- The build writes `partition.uf2` side by side with `picomem.uf2` (made from `src/fwupdate/pm_partitions.json` with picotool).
- Copy `partition.uf2` to the RP2350 drive (BOOTSEL mode). The current firmware no longer starts and the board comes back in BOOTSEL mode (if not, reconnect it with BOOTSEL pressed). After correct `partition.uf2` flash the `picotool partition info` should show the A and B partitions.
- Copy `picomem.uf2` to the RP2350 drive. It goes in slot A.

**Update :**
- Copy `picomem.uf2` (the normal build output) to the root of the SD card (with the HTTP server : upload it from the browser), then restart the PicoMEM.
- The file is checked first (RP2350 firmware for the PicoMEM 2.0, complete). If it is refused, the PicoMEM starts normally.
- Otherwise the ISA bus stops during the update (a few seconds, just after power on), then the PicoMEM restarts on the new firmware. Usually the PC is still in its own POST and does not notice it. If the PC reached the PicoMEM BIOS during the update, it boots without the PicoMEM or hangs : **restart the PC**.
- The file is renamed `picomem.uf2.done` (or `picomem.uf2.bad`), and the result is written in `fwupdate.txt`. The `picomem.uf2.done` / `picomem.uf2.bad` of a previous update are deleted first: if one can't be deleted (read only for example), nothing is done and `fwupdate.txt` says why.
- Copying `picomem.uf2` in BOOTSEL mode still works, it also goes in the free slot.
- The firmware is built as "Try Before You Buy" : it only starts from an update (SD card or BOOTSEL copy) until it has run once. To load it with a debugger (SWD), set `PM_FW_TBYB` to 0 in `src/CMakeLists.txt`.
- With an OLED screen, "Updating..." is shown during the update, and "UPD Failed!" if the update was refused or failed.

**Partition table update from the SD card :** a board already set up (and running a firmware with this feature) can get a new partition table without USB.
- Copy `partition.uf2` and `picomem.uf2` (both from the same build) to the root of the SD card, then restart the PicoMEM. `partition.uf2` alone is refused.
- The table is checked first (picotool partition table with its hash) and the firmware must fit a slot of the new table outside the running one. If the table is the one already installed, only the firmware is updated.
- The firmware is written first, then the table. The bootrom has two partition table slots (flash 0 and 4 KB) : the new table goes in the free one, then the old one is erased. A power cut during the update still leaves a valid table, but the board may then need the BOOTSEL setup again.
- Both files are renamed `.done` (or `.bad`), the result is in `fwupdate.txt`.

## Debug : Sound Blaster I/O log on the SD card (PicoMEM 2.0)

Build with `SBDSP_IOLOG=1` (PicoMEM 2.0 section of `src/CMakeLists.txt`) to record what a program does with the emulated Sound Blaster in `sbiolog.txt` at the root of the SD card (rewritten at each power on) : port reads/writes, DMA controller (8237) writes, Sound Blaster IRQs (and if they use the ISA IRQ line or the PicoMEM IRQ) and DSP DMA starts, with a time stamp in µs. The file is written when the program is quiet for 0.5 s, so wait a moment before switching off. Debug only : it uses 24KB of RAM and the SD card writes can briefly disturb the audio.

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
