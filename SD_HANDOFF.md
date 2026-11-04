# DubSauce — SD Card Persistence: Troubleshooting Handoff

## Goal
Persist 8 synth presets to a microSD card across power cycles on a **Daisy Seed3
Desktop Dev Kit**. The synth itself works fully; **SD persistence does not work**.

## Definitive current symptom
`f_mount(&fs, path, 1)` returns **FR_DISK_ERR (FatFS result code = 1)** — a low-level
SDMMC read failure. It is **NOT** FR_NO_FILESYSTEM(13), so the card format/partition is
**not** the cause. `fsi.Init()` succeeds. It does **not** hardfault (earlier builds with
the FatFS objects in DTCM appeared to hardfault; that was changed — see below).

## Hardware / build
- Board: Daisy **Seed3** on Electro-Smith **Desktop Dev Kit**. libDaisy **v9.0.0**,
  board class `daisy::desktop_devkit::Hardware`.
- App type: **BOOT_SRAM** (binary too big for the 128K internal flash). Flashed via the
  Daisy bootloader DFU to QSPI (0x90040000); runs from SRAM. Linker:
  `libDaisy/core/STM32H750IB_sram.lds`.
- Build (on the owner's Mac): `cd ~/Desktop/DaisyDesktopDubSiren && make clean && make`
- Flash: on the Seed press **RESET**, then **BOOT** (USR LED pulses), then `make program-dfu`.
- IMPORTANT: editing libDaisy source requires rebuilding the lib first
  (`cd libDaisy && make clean && make`), THEN the app. App-only edits do not.

## SD card
32GB, formatted **FAT32 via Windows (MBR)**. FatFS here has exFAT disabled
(`libDaisy/src/sys/ffconf.h` `_FS_EXFAT=0`), but the card is FAT32 so that is fine.

## Current SD code state
- Config: `sd_cfg.Defaults()`, `Speed::STANDARD` (25 MHz), `BusWidth::BITS_1`.
- `FatFSInterface fsi`, `PresetManager presets`, and a global `FIL g_sd_file` are placed
  in `DMA_BUFFER_MEM_SECTION` (`.sram1_bss` -> RAM_D2 @ 0x30000000, which libDaisy's MPU
  marks **non-cacheable**, see `libDaisy/src/sys/system.cpp` ~line 525). Rationale: the
  SRAM-bootloader linker puts `.bss` + stack in **DTCM (0x20000000)**, which the STM32H7
  SDMMC IDMA **cannot access**. Confirmed via build map: `fsi` was at 0x2000a7dc (DTCM).
- SD init is **non-blocking**: if mount fails, `sdReady=false` and the synth runs normally.
  `PresetManager::SaveToSD/LoadFromSD` take a `FIL&` (the DMA-safe `g_sd_file`).

## Tried and ruled OUT
- fsi moved from DTCM to RAM_D2 non-cacheable DMA section — still FR_DISK_ERR (but no
  longer hardfaults).
- `GPIO_PULLUP` added to SDMMC pins in `libDaisy/src/per/sdmmc.cpp` (currently baked into
  libdaisy.a) — no change.
- Speeds tried: MEDIUM_SLOW (12.5 MHz) and STANDARD (25 MHz).
- Bus widths tried: BITS_1 and default BITS_4.
- Card reformatted FAT32 / MBR — no change.
- USB serial logging (`hw.seed.StartLog(false)` + `PrintLine`) does **NOT** enumerate a
  CDC device on macOS from this bootloader build (no `/dev/cu.usbmodem*`). Serial debug
  is unavailable via this path.

## Hardware — CONFIRMED via official schematic (ES-Seed3-DevKit-Desktop-Rev3.pdf)
Schematic: https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed-3-desktop/ES-Seed3-DevKit-Desktop-Rev3.pdf
Pinout:    https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed-3-desktop/Seed3-Desktop-Dev-Kit-Pinout.pdf

The microSD slot is correctly designed and fully wired — hardware is NOT the blocker:
- Socket: TF-01A microSD (ref MICRO_SD7). 4-bit wired: DAT0..DAT3, CMD, CLK all connected.
- Seed nets: SD_DATA_0..3, SD_CMD, SD_CLK on Seed pins 2-7 (= D-pins D1..D6):
  D1/PC11=DAT3, D2/PC10=DAT2, D3/PC9=DAT1, D4/PC8=DAT0, D5/PD2=CMD, D6/PC12=CK.
- Card detect: SDMMC_DETECT -> D7 (PG10). Firmware inits hw.sd_detect on D7.
- Pull-ups: R50/R51/R52 = 47K near the connector; libDaisy sdmmc.cpp was also changed to
  GPIO_PULLUP so STM32 internal pull-ups are on too.
- Power: +3V3 to VDD; no load switch.
=> The slot is fine. FR_DISK_ERR is therefore a SOFTWARE / CONFIG problem, not a bad slot.

## TOP SUSPECTS / next steps (prioritized, software-focused)
1. **DMA memory domain (PRIME SUSPECT).** fsi/presets/g_sd_file are currently in RAM_D2
   (0x30000000, non-cacheable) via DMA_BUFFER_MEM_SECTION. libDaisy's known-working SD
   examples leave fsi in ordinary .bss, which under the *flash* linker lands in D1 **AXI
   SRAM (0x24000000)** — the same power domain as the SDMMC1 controller/IDMA. SDMMC1 IDMA
   reaching across D1->D2 is a plausible cause of FR_DISK_ERR. FIX TO TRY FIRST: relocate
   those three objects to **D1 AXI SRAM**. In STM32H750IB_sram.lds add a NOLOAD section
   mapped to SRAM, e.g.:
       .axisram_bss (NOLOAD) : { . = ALIGN(32); *(.axisram_bss .axisram_bss*); . = ALIGN(32); } > SRAM
   and place the objects with __attribute__((section(".axisram_bss"))). App-only rebuild.
   This makes our setup match the proven-working example exactly.
2. **Distinguish card-init vs data-transfer failure.** Instrument BSP_SD_Init()/HAL_SD_Init
   return and the HAL_SD ErrorCode in libDaisy/src/util/bsp_sd_diskio.c + sd_diskio.c. If
   card init (CMD0/CMD8/ACMD41/CMD2/CMD3) fails -> CMD/clock/power path. If init succeeds
   but disk_read fails -> DAT0/DMA path. This splits FR_DISK_ERR in half. (needs libdaisy.a rebuild)
3. **Prove the stack works outside the bootloader:** build libDaisy examples/SDMMC_HelloWorld
   as a plain internal-flash app on THIS board with THIS card. Works -> the BOOT_SRAM /
   bootloader path is the differentiator. Fails -> deeper issue.
4. **Bootloader SDMMC/RCC state:** the Daisy bootloader may leave SDMMC kernel clock / pins
   in a state the BOOT_SRAM app does not re-init. Check RCC SDMMC clock source + divider and
   HAL_SD_MspInit vs a flash-boot app.
5. Try Speed::SLOW (lowest clock) to rule out signal integrity, though hardware looks fine.

## Key files
- `src/main.cpp` — SD init (see "---- SD card init ----") + synth.
- `src/PresetManager.h` — SaveToSD/LoadFromSD(FIL&). File format: 8-byte magic
  "DUBSPRST" + 8 occupied flags + 8*4*8 floats = presets.bin.
- `Makefile` — APP_TYPE=BOOT_SRAM, USE_FATFS=1.

## Owner constraints (IMPORTANT)
- Give terminal/build commands **ONE AT A TIME**; wait for confirmation. Do NOT chain commands.
- Do NOT compile in the cloud — the owner builds/flashes on his Mac.
- Research primary sources (schematics, libDaisy source, forums) — do not guess.
