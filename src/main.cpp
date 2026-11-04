/** DubSauce — Seed3 Desktop dub siren, bench build v0.5.1
 *
 *  Voice + control shell + lightweight hand-rolled FX bus (tape echo + stereo reverb).
 *  Preset bank (8 slots, persisted to SD card as presets.bin).
 *
 *  SHIFT pad (S9) cycles five UI pages: LIVE -> SHAPE -> DEEP -> FX -> PRESET.
 *  Bottom-row LED count shows the page (1–5 LEDs). Top-row LEDs show knob positions
 *  on sound pages, or preset slot status on the PRESET page (solid = occupied,
 *  flashing = empty/available).
 *
 *  S1+S9 combo = "Save As": enters save mode on the PRESET page. Long-press (1 s)
 *  any top-row pad to save the current patch there. S9 cancels. Bottom-row LEDs
 *  flash in save mode to distinguish it from normal preset browsing.
 *
 *  Trigger: SW18 up momentary / center mute / down latch · S1 pad · GATE IN 1 · MIDI note.
 *  S1 LED overrides knob brightness while sounding · CV OUT 1 mirrors the wail.
 *
 *  POT PAGES (sound pages — knobs tracked with pickup):
 *    LIVE : VR1 Tune  VR2 WailRate  VR3 WailDepth  VR4 Sweep  VR5 Decay  VR6 Tone  VR7 DelaySend  VR8 ReverbSend
 *    SHAPE: VR1 OscWave VR2 PW/Blend VR3 Sub  VR4 Drive  VR5 Volume VR6 LFOWave  VR7 Swing  VR8 Resonance
 *    DEEP : VR1 Glide  VR2 SweepTime VR3 Attack VR4 FiltEnv VR5 LFOFade VR6 ChopDepth VR7 ChopRate VR8 PhaseOff
 *    FX   : VR1 DelayTime VR2 Feedback VR3 TapeTone VR4 Wow/Flutter VR5 RevDecay VR6 RevTone VR7 TapeDrive VR8 Noise
 */
#include "desktop_devkit.h"
#include "per/sdmmc.h"
#include "sys/fatfs.h"
#include "DubSirenVoice.h"
#include "ControlShell.h"
#include "FxBus.h"
#include "PresetManager.h"

using namespace daisy;
using namespace daisy::desktop_devkit;
using namespace dubsauce;

static constexpr int PAD_TRIGGER = 0; // S1 (top-left)
static constexpr int PAD_SHIFT   = 8; // S9 (bottom-left)
static constexpr int BOTTOM_ROW  = 8;
static constexpr int NUM_PAGES   = 5; // 4 sound + 1 preset
static constexpr uint32_t LONG_PRESS_MS = 1000;

Hardware         hw;
SdmmcHandler     sdcard;
// fsi / g_sd_file / presets live in D1 AXI SRAM (.axisram_bss @ 0x24000000) -- the SAME
// power domain as the SDMMC1 controller/IDMA, matching libDaisy's working SD examples.
// Required because this BOOT_SRAM app's linker puts ordinary globals + stack in DTCM
// (0x20000000), which the STM32H7 SDMMC DMA cannot reach; and D2 SRAM (0x30000000) is
// cross-domain and returned FR_DISK_ERR. See SD_HANDOFF.md.
#define SD_AXISRAM __attribute__((section(".axisram_bss")))
FatFSInterface   SD_AXISRAM fsi;
FIL              SD_AXISRAM g_sd_file;
DubSirenVoice    siren;
ControlShell     shell;
DubSirenParams   params;
FxParams         fxp;
PresetManager    SD_AXISRAM presets;
static volatile bool sdReady       = false;
static volatile bool pendingSDSave = false;  // set in IRQ, serviced in main loop

static bool           latched  = false;
static volatile bool  midiGate = false;
static volatile float midiFreq = 220.f;
static volatile bool  sounding = false;
static volatile int   uiPage   = 0;        // 0-3 sound, 4 preset
static volatile bool  saveMode = false;

// LED state written in AudioCallback, read in main loop
static volatile float topLeds[8] = {0};
static volatile float botLeds[8] = {0};

// Long-press tracking for save mode
static bool     padHeld[8]      = {false};
static uint32_t padHoldStart[8] = {0};

// Save-confirmation flash: 3 blinks on the saved slot before returning to LIVE
static volatile int      saveFlashSlot  = -1;   // -1 = inactive
static volatile uint32_t saveFlashStart = 0;
static constexpr uint32_t FLASH_DURATION_MS = 750; // 3 blinks × 250ms cycle

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.UpdateAllControls();
    uint32_t now_ms = System::GetNow();

    float pots[8];
    for(int i = 0; i < 8; i++) pots[i] = hw.pot[i].Value();

    // ---- S9 (SHIFT) handling ----
    if(hw.button[PAD_SHIFT].RisingEdge())
    {
        if(hw.button[PAD_TRIGGER].Pressed() && !saveMode)
        {
            // S1+S9 combo → enter save mode, jump to preset page
            saveMode = true;
            uiPage   = 4;
        }
        else if(saveMode)
        {
            // S9 in save mode → cancel, back to LIVE
            saveMode = false;
            uiPage   = 0;
            shell.SetPage(PAGE_LIVE, pots);
        }
        else
        {
            // Normal page advance (5-page cycle)
            uiPage = (uiPage + 1) % NUM_PAGES;
            if(uiPage < 4) shell.SetPage((Page)uiPage, pots);
        }
    }

    // ---- Pot processing (runs on whichever sound page the shell is on) ----
    shell.ProcessPots(pots);
    shell.BuildParams(params);
    shell.BuildFx(fxp);

    // ---- Save-confirmation flash timeout → return to LIVE ----
    if(saveFlashSlot >= 0 && (now_ms - saveFlashStart) >= FLASH_DURATION_MS)
    {
        saveFlashSlot = -1;
        uiPage = 0;
        shell.SetPage(PAGE_LIVE, pots);
    }

    // ---- Preset page pad handling (blocked during flash) ----
    if(uiPage == 4 && saveFlashSlot < 0)
    {
        if(saveMode)
        {
            // Long-press any top-row pad to save the current patch
            for(int s = 0; s < 8; s++)
            {
                if(hw.button[s].Pressed())
                {
                    if(!padHeld[s]) { padHoldStart[s] = now_ms; padHeld[s] = true; }
                    if(now_ms - padHoldStart[s] > LONG_PRESS_MS)
                    {
                        float allVals[4][8];
                        shell.GetAllValues(allVals);
                        presets.Save(s, allVals);
                        pendingSDSave  = true;   // deferred to main loop
                        saveMode       = false;
                        saveFlashSlot  = s;
                        saveFlashStart = now_ms;
                        // Stay on preset page during flash — LIVE transition after flash ends
                        padHeld[s] = false;
                        break;
                    }
                }
                else { padHeld[s] = false; }
            }
        }
        else
        {
            // Tap a pad to recall a stored preset
            for(int s = 0; s < 8; s++)
            {
                if(hw.button[s].RisingEdge() && presets.HasPreset(s))
                {
                    float allVals[4][8];
                    if(presets.Load(s, allVals))
                    {
                        shell.SetAllValues(allVals, pots);
                        uiPage = 0;
                        shell.SetPage(PAGE_LIVE, pots);
                    }
                    break;
                }
            }
        }
    }

    // ---- Trigger (sound pages only — S1 is a preset pad on page 5) ----
    bool gate = false;
    if(uiPage < 4)
    {
        int  tpos = hw.tog_sw18.Read();
        bool held = hw.button[PAD_TRIGGER].Pressed() || hw.gate_in[0].State();
        bool edge = hw.button[PAD_TRIGGER].RisingEdge() || hw.gate_in[0].Trig();
        switch(tpos)
        {
            case Switch3::POS_UP:   gate = held; break;
            case Switch3::POS_DOWN: if(edge) latched = !latched; gate = latched; break;
            default:                gate = false; latched = false; break;
        }
    }
    bool  g = gate || midiGate;
    float f = midiGate ? midiFreq : params.tune_hz;
    params.trig_mode = TrigMode::Gate;
    siren.SetParams(params);
    siren.Gate(g, f);
    sounding = g;

    hw.SetDacOut(DacHandle::Channel::ONE,
                 (uint16_t)daisysp::fclamp((siren.LfoValue() * 0.5f + 0.5f) * 4095.f, 0.f, 4095.f));

    // ---- Compute LED state (read by main loop) ----
    if(saveFlashSlot >= 0)
    {
        // Save-confirmation flash: only the saved slot blinks fast, all others dark
        bool blinkOn = ((now_ms / 125) % 2) != 0;   // ~4 Hz fast blink
        for(int i = 0; i < 8; i++)
            topLeds[i] = (i == saveFlashSlot && blinkOn) ? 1.f : 0.f;
    }
    else if(uiPage < 4)
    {
        // Sound page → top LEDs reflect knob positions
        float kv[8];
        shell.GetPageValues(shell.CurrentPage(), kv);
        for(int i = 0; i < 8; i++) topLeds[i] = kv[i] * 0.9f;
        if(sounding) topLeds[0] = 1.f;   // S1 trigger indicator overrides VR1
    }
    else
    {
        // Preset page → top LEDs show slot status
        bool flashOn = ((now_ms / 250) % 2) != 0;   // ~2 Hz blink
        for(int s = 0; s < 8; s++)
        {
            if(presets.HasPreset(s))
                topLeds[s] = 0.8f;                     // solid = occupied
            else
                topLeds[s] = flashOn ? 0.35f : 0.f;    // flashing = empty
        }
    }

    // Bottom row: page count (1–5 LEDs); flash in save mode
    bool saveBlink = saveMode && (((now_ms / 150) % 2) != 0);
    int  numLit    = uiPage + 1;
    for(int i = 0; i < 8; i++)
    {
        if(i < numLit)
            botLeds[i] = saveMode ? (saveBlink ? 0.6f : 0.1f) : 0.6f;
        else
            botLeds[i] = 0.f;
    }

    // ---- Audio ----
    for(size_t i = 0; i < size; i++)
    {
        float s = siren.Process();
        float oL, oR;
        FxProcess(s, in[0][i], in[1][i],
                  params.delay_send, params.reverb_send, fxp, oL, oR);
        out[0][i] = oL;
        out[1][i] = oR;
    }
}

int main(void)
{
    hw.Init();
    hw.seed.StartLog(false);   // non-blocking USB serial log (view with a serial monitor)
    System::Delay(50);
    siren.Init(hw.seed.AudioSampleRate());
    shell.Init();
    FxInit(hw.seed.AudioSampleRate());
    presets.Init();

    // ---- SD card init ----
    // fsi / presets / g_sd_file live in non-cacheable DMA RAM (see declarations above),
    // so the SDMMC DMA controller can reach the FatFS buffers. Without that, mount and
    // file I/O hardfault on SRAM-bootloader builds.
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();                                // known-good defaults for all fields
    sd_cfg.speed = SdmmcHandler::Speed::STANDARD;     // 25 MHz
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_1;     // 1-bit bus: most compatible (per Daisy forum)
    sdcard.Init(sd_cfg);

    FatFSInterface::Config fsi_config;
    fsi_config.media = FatFSInterface::Config::MEDIA_SD;

    // Attempt to mount the SD card and load saved presets. Non-blocking: if any step
    // fails, sdReady stays false and the synth runs normally (no persistence).
    // KNOWN ISSUE (unsolved): on this Desktop Dev Kit BOOT_SRAM build, f_mount returns
    // FR_DISK_ERR (1) -- a low-level SDMMC read failure. SD persistence does not work
    // yet. See SD_HANDOFF notes. sdReady guards all SD writes so the synth is unaffected.
    if(fsi.Init(fsi_config) == FatFSInterface::Result::OK)
    {
        FATFS& fs = fsi.GetSDFileSystem();
        if(f_mount(&fs, fsi.GetSDPath(), 1) == FR_OK)
        {
            sdReady = true;
            presets.LoadFromSD(g_sd_file);
        }
    }

    // Boot indicator: after a pause (clear of the power-on flash), blink the TOP row 3x
    // if the SD card mounted, or the BOTTOM row 3x if it did not.  Top = good, bottom = bad.
    for(int i = 0; i < 16; i++) hw.SetLed(i, 0.f);
    hw.UpdateLeds();
    System::Delay(1500);
    {
        int base = sdReady ? 0 : BOTTOM_ROW;
        for(int k = 0; k < 3; k++)
        {
            for(int i = 0; i < 8; i++) hw.SetLed(base + i, 1.f);
            hw.UpdateLeds(); System::Delay(250);
            for(int i = 0; i < 8; i++) hw.SetLed(base + i, 0.f);
            hw.UpdateLeds(); System::Delay(250);
        }
        for(int i = 0; i < 16; i++) hw.SetLed(i, 0.f);
        hw.UpdateLeds();
    }

    hw.seed.StartAudio(AudioCallback);

    uint32_t last_led = System::GetNow();
    while(true)
    {
        while(hw.midi.HasEvents())
        {
            auto m = hw.midi.PopEvent();
            if(m.type == NoteOn)
            {
                if(m.data[1] > 0) { midiFreq = daisysp::mtof((float)m.data[0]); midiGate = true; }
                else              { midiGate = false; }
            }
            else if(m.type == NoteOff) { midiGate = false; }
        }

        // ---- Deferred SD card write (can't do file I/O in audio IRQ) ----
        if(pendingSDSave && sdReady)
        {
            bool ok = presets.SaveToSD(g_sd_file);
            hw.seed.PrintLine("SD: SaveToSD -> %s", ok ? "OK" : "FAIL");
            pendingSDSave = false;
        }

        uint32_t now = System::GetNow();
        if(now - last_led >= 16)
        {
            for(int i = 0; i < 8; i++) hw.SetLed(i, topLeds[i]);
            for(int i = 0; i < 8; i++) hw.SetLed(BOTTOM_ROW + i, botLeds[i]);
            hw.UpdateLeds();
            last_led = now;
        }
        System::Delay(1);
    }
}
