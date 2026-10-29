# DubSauce — Seed3 Desktop dub siren (bench v0.1)

First flashable step of the multi-engine instrument: the **dub-siren voice only**, wired to the
real Daisy Audio Seed3 Desktop DevKit controls so you can turn knobs and hear it. The sampler,
Synare, shared FX bus, keyboard, and full PERFORM panel come next — see
`DubSiren_Seed3_ControlMap.md` for the whole plan.

## Files
- `src/DubSirenVoice.h` — the voice engine (hardware-independent). Osc select (incl. S-1 blend),
  8-shape wail LFO, pitch-env sweep, amp env (gate/one-shot), SVF filter, sub, drive, chopper.
- `src/main.cpp` — bench bring-up bound to `daisy::desktop_devkit::Hardware`.
- `Makefile` — set up for the Desktop kit + external-DFU bootloader.

## Prerequisites — update libDaisy to v9.0.0
The Desktop board class (`daisy::desktop_devkit::Hardware`) and the ext-DFU bootloader binary
only exist in **libDaisy v9.0.0**. In your DaisyExamples (or wherever libDaisy/DaisySP live):

    cd libDaisy && git fetch --tags && git checkout v9.0.0 && make clean && make
    cd ../DaisySP && make

Then edit `LIBDAISY_DIR` / `DAISYSP_DIR` at the top of the `Makefile` to point at them.

## Build & flash (differs from your bare-Seed flow)
The Desktop kit is powered through the **Seed3's own USB-C port**, so the app runs from SRAM via
the external-DFU bootloader. Flash the bootloader **once**, then the app each time.

    make                 # build
    make program-boot    # ONE TIME: install the ext-DFU bootloader on the Seed3
    make program-dfu     # flash the app (hold BOOT, tap RESET to enter DFU if needed)

After the bootloader is installed, day-to-day it's just `make && make program-dfu`.

## Controls (bench)
| Control | Function |
|---|---|
| VR1 | Tune |
| VR2 | Wail Rate |
| VR3 | Wail Depth |
| VR4 | Sweep (center = none, CCW dive down / CW up) |
| VR5 | Decay |
| VR6 | Tone (cutoff) |
| VR7 | **Resonance** *(bench override — final layout = Delay Send)* |
| VR8 | **Sub level** *(bench override — final layout = Reverb Send)* |
| SW18 (3-pos) | Trigger: **UP** momentary / **CENTER** mute / **DOWN** latch |
| Button S1 | Trigger pad |
| GATE IN 1 | External trigger |
| MIDI note in | Pitch + gate (mono) |
| CV OUT 1 | Mirrors the wail LFO (scope it) |
| LED 1 | Lit while sounding |

Default patch: S-1 blend voice, sub at pot, light drive, sine wail. Turn **Wail Depth** up and
**Wail Rate** to taste, hold S1 (SW18 up) — that's your siren.

## Known v0.1 notes
- VR7/VR8 are a bench convenience; they become Delay/Reverb sends once the shared FX bus lands.
- Pulse Width is a square/saw blend proxy for now; a true variable-PW PolyBLEP osc is next.
- Tempo-sync, presets (SD), the 15-note keyboard, and the PERFORM FX-throw panel are later stages.
- Not yet compiled against a toolchain here — expect to shake out the first build on your Mac.
