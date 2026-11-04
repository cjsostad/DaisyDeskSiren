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
    // NO in-class initializers here: fxReverb lives in SDRAM (DSY_SDRAM_BSS), and global ctors
    // run before hw.Init() powers up the SDRAM controller. An in-class default would make this
    // object's constructor write to not-yet-ready SDRAM and hard-fault at boot. Init() (called
    // from FxInit, after hw.Init) sets all of these at runtime instead.
    daisysp::DelayLine<float, 2048> comb_[4];
    daisysp::DelayLine<float, 1024> ap_[2];
    float damp_[4];
    float fb_, dampCoef_;
};

static constexpr size_t FX_MAXDEL = 96000; // ~2 s @ 48 kHz, in SDRAM
static daisysp::DelayLine<float, FX_MAXDEL> DSY_SDRAM_BSS fxDelayBuf;
static MiniReverb          DSY_SDRAM_BSS fxReverb;   // its comb/allpass buffers → SDRAM
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

// Shared tape stage: advance the smoothed/fluttered delay time and return the current echo tap.
inline float fxTapeRead_(const FxParams& fx)
{
    float tgt = fx.delay_time_ms * 0.001f * fxSr;
    if(tgt > (float)(FX_MAXDEL - 2)) tgt = (float)(FX_MAXDEL - 2);
    if(tgt < 1.f) tgt = 1.f;
    fxTargetDelay = tgt;
    fxCurDelay += 0.0002f * (fxTargetDelay - fxCurDelay);

    float dtime = fxCurDelay * (1.f + fxFlutter.Process() * fx.flutter);
    if(dtime < 1.f) dtime = 1.f;
    fxDelayBuf.SetDelay(dtime);
    return fxDelayBuf.Read();
}

// dry = siren (mono); inL/inR = line in; sends come from the Live page (VR7/VR8).
inline void FxProcess(float dry, float inL, float inR,
                      float delaySend, float reverbSend,
                      const FxParams& fx, float& outL, float& outR)
{
    float echo    = fxTapeRead_(fx);
    float inMono  = 0.5f * (inL + inR);
    float busSrc  = dry + inMono;

    // tape echo with filtered + saturated feedback (manual one-pole LP = tape bandwidth)
    float k = 1.f - expf(-2.f * 3.14159265f * fx.tape_tone / fxSr);
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

// Per-voice-send variant for the multi-engine build:
//   dry        = summed DIRECT signal (all voices, heard dry)
//   sendDelay  = already-weighted sum feeding the echo  (Σ voice*itsDelaySend)
//   sendReverb = already-weighted sum feeding the reverb (Σ voice*itsReverbSend)
// Line-in is mixed dry and sent at a modest fixed amount so throws still touch external audio.
inline void FxProcessMix(float dry, float sendDelay, float sendReverb,
                         float inL, float inR,
                         const FxParams& fx, float& outL, float& outR)
{
    float echo   = fxTapeRead_(fx);
    float inMono = 0.5f * (inL + inR);

    float k = 1.f - expf(-2.f * 3.14159265f * fx.tape_tone / fxSr);
    if(k < 0.f) k = 0.f; else if(k > 1.f) k = 1.f;
    fxTapeLP += k * (echo * fx.feedback - fxTapeLP);
    float fb = tanhf(fxTapeLP * (1.f + fx.drive * 2.f));
    fxDelayBuf.Write(sendDelay + inMono * 0.3f + fb);

    fxReverb.SetFeedback(fx.rev_decay);
    float damp = 1.f - fx.rev_tone / 18000.f;
    fxReverb.SetDamp(damp < 0.05f ? 0.05f : (damp > 0.6f ? 0.6f : damp));
    float revIn = sendReverb + inMono * 0.3f + echo * 0.25f;
    float rev   = fxReverb.Process(revIn);

    outL = tanhf(dry + inL + echo + rev);
    outR = tanhf(dry + inR + echo + rev);
    if(!std::isfinite(outL)) outL = 0.f;
    if(!std::isfinite(outR)) outR = 0.f;
}
} // namespace dubsauce
