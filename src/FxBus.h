// FxBus.h — DubSauce FX bus (lightweight, self-contained).
// Tape echo (DaisySP DelayLine) + a hand-rolled Freeverb-style reverb. Deliberately uses
// NO DaisySP ReverbSc/Tone (this DaisySP build doesn't ship them) — only DelayLine and
// Oscillator, which it does. The faithful je-201 RE-201 model is shelved under src/je201/.
//
// INCLUDE AFTER desktop_devkit.h (needs the SDRAM macro + DaisySP).

#pragma once
#include "daisysp.h"
#include "ControlShell.h"
#include <cmath>

// The big delay buffer lives in the 256 KB RAM_D2 region via the custom linker section
// added to core/STM32H750IB_sram.lds. This is always-on internal RAM, so — unlike external
// SDRAM on a BOOT_SRAM app — it is guaranteed live before the first audio IRQ. If you ever
// re-clone/replace libDaisy, re-add that section or this buffer overflows DTCMRAM and the
// FX build won't link. The reverb (~41 KB) stays in default RAM (DTCMRAM).
#define DSY_RAMD2_BSS __attribute__((section(".ram_d2_bss")))

namespace dubsauce
{
// ---- compact mono Freeverb-style reverb (4 combs + 2 allpass) ----
class MiniReverb
{
  public:
    void Init()
    {
        const float combLen[4] = {1116.f, 1188.f, 1277.f, 1356.f};
        const float apLen[2]   = {556.f, 441.f};
        for(int i = 0; i < 4; i++) { comb_[i].Init(); comb_[i].SetDelay(combLen[i]); damp_[i] = 0.f; }
        for(int i = 0; i < 2; i++) { ap_[i].Init();   ap_[i].SetDelay(apLen[i]); }
        fb_ = 0.84f; dampCoef_ = 0.3f;
    }
    void SetFeedback(float f) { fb_ = f < 0.f ? 0.f : (f > 0.97f ? 0.97f : f); } // room size
    void SetDamp(float d)     { dampCoef_ = d < 0.f ? 0.f : (d > 0.9f ? 0.9f : d); }

    float Process(float x)
    {
        float out = 0.f;
        for(int i = 0; i < 4; i++)
        {
            float y  = comb_[i].Read();
            damp_[i] = y * (1.f - dampCoef_) + damp_[i] * dampCoef_; // LP in the feedback
            comb_[i].Write(x + damp_[i] * fb_);
            out += y;
        }
        out *= 0.25f;
        for(int i = 0; i < 2; i++)
        {
            float y = ap_[i].Read();
            ap_[i].Write(out + y * 0.5f);
            out = y - 0.5f * out; // Schroeder allpass
        }
        return out;
    }

  private:
    daisysp::DelayLine<float, 2048> comb_[4];
    daisysp::DelayLine<float, 1024> ap_[2];
    float damp_[4] = {0.f, 0.f, 0.f, 0.f};
    float fb_ = 0.84f, dampCoef_ = 0.3f;
};

// INTERNAL SRAM — no SDRAM. On this SRAM-loaded (BOOT_SRAM) app the external SDRAM
// controller isn't reliably up when the audio IRQ first runs, so any read of a buffer
// at 0xC0000000 hard-faults and freezes the MCU (dead unit, one stuck LED frame). The
// whole FX bus therefore lives in internal SRAM, the same memory the working voice uses.
// Cost: shorter max delay (~1.2 s). Plenty for dub; we can revisit SDRAM for 2 s+ later.
static constexpr size_t FX_MAXDEL = 57600; // ~1.2 s @ 48 kHz, internal RAM_D2
static daisysp::DelayLine<float, FX_MAXDEL> DSY_RAMD2_BSS fxDelayBuf;
// fxReverb (~41 KB) stays in default RAM (DTCMRAM). Its ctor runs before main(), which is
// safe because DTCMRAM is internal and always live (this would hard-fault in external SDRAM).
static MiniReverb          fxReverb;
static daisysp::Oscillator fxFlutter;                // wow/flutter modulator
static float fxSr = 48000.f, fxCurDelay = 14400.f, fxTargetDelay = 14400.f, fxTapeLP = 0.f;

inline void FxInit(float sr)
{
    fxSr = sr;
    fxDelayBuf.Init();
    fxReverb.Init();
    fxFlutter.Init(sr);
    fxFlutter.SetWaveform(daisysp::Oscillator::WAVE_SIN);
    fxFlutter.SetFreq(4.0f);
    fxCurDelay = fxTargetDelay = 0.3f * sr;
    fxDelayBuf.SetDelay(fxCurDelay);
    fxTapeLP = 0.f;
}

// dry = siren (mono); inL/inR = line in; sends come from the Live page (VR7/VR8).
inline void FxProcess(float dry, float inL, float inR,
                      float delaySend, float reverbSend,
                      const FxParams& fx, float& outL, float& outR)
{
    // smoothed delay time (manual one-pole, no fonepole dependency)
    float tgt = fx.delay_time_ms * 0.001f * fxSr;
    if(tgt > (float)(FX_MAXDEL - 2)) tgt = (float)(FX_MAXDEL - 2);
    if(tgt < 1.f) tgt = 1.f;
    fxTargetDelay = tgt;
    fxCurDelay += 0.0002f * (fxTargetDelay - fxCurDelay);

    float dtime = fxCurDelay * (1.f + fxFlutter.Process() * fx.flutter);
    if(dtime < 1.f) dtime = 1.f;
    fxDelayBuf.SetDelay(dtime);

    float inMono = 0.5f * (inL + inR);
    float busSrc = dry + inMono;

    // tape echo with filtered + saturated feedback (manual one-pole LP = tape bandwidth)
    float echo = fxDelayBuf.Read();
    float k    = 1.f - expf(-2.f * 3.14159265f * fx.tape_tone / fxSr);
    if(k < 0.f) k = 0.f; else if(k > 1.f) k = 1.f;
    fxTapeLP += k * (echo * fx.feedback - fxTapeLP);
    float fb = tanhf(fxTapeLP * (1.f + fx.drive * 2.f));
    fxDelayBuf.Write(busSrc * delaySend + fb);

    // reverb: decay -> room size, tone -> damping (brighter = less damping)
    fxReverb.SetFeedback(fx.rev_decay);
    float damp = 1.f - fx.rev_tone / 18000.f;
    fxReverb.SetDamp(damp < 0.05f ? 0.05f : (damp > 0.6f ? 0.6f : damp));
    float revIn = busSrc * reverbSend + echo * reverbSend * 0.5f;
    float rev   = fxReverb.Process(revIn);

    outL = tanhf(dry + inL + echo + rev);
    outR = tanhf(dry + inR + echo + rev);
    if(!std::isfinite(outL)) outL = 0.f;
    if(!std::isfinite(outR)) outR = 0.f;
}
} // namespace dubsauce
