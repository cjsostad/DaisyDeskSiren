// ControlShell.h — DubSauce siren control surface (v0.2)
// Three pot pages (Live / Shape / Deep) behind a SHIFT pad, with knob PICKUP so
// switching pages never jumps an untouched parameter. Pure logic — no hardware calls,
// so it compiles/behaves identically on-device and off. main.cpp feeds it the 8 pot
// readings and asks it to fill a DubSirenParams.
//
// Pickup model (catch-on-move): each page stores its 8 knob values as normalized 0..1.
// On a page change every pot is "released" and holds its stored value; a pot re-captures
// its parameter as soon as you MOVE it past a small threshold, then tracks it absolutely.
// This keeps idle knobs from scrambling the patch on a page flip, while guaranteeing every
// knob responds immediately when touched — including ones whose value sits at an extreme
// (0.0/1.0), which a "pass-through" scheme could never catch. On power-up the instrument
// loads the default patch below regardless of physical knob positions.

#pragma once
#include "DubSirenVoice.h"
#include <cmath>

namespace dubsauce
{
enum Page { PAGE_LIVE = 0, PAGE_SHAPE = 1, PAGE_DEEP = 2, PAGE_FX = 3, PAGE_COUNT = 4 };

// FX-bus parameters — filled from the FX page here, consumed by FxBus.h.
struct FxParams
{
    float delay_time_ms = 300.f;
    float feedback      = 0.45f;
    float tape_tone     = 3500.f;
    float flutter       = 0.002f;
    float rev_decay     = 0.85f;
    float rev_tone      = 8000.f;
    float drive         = 0.15f;
};

class ControlShell
{
  public:
    void Init()
    {
        page_ = PAGE_LIVE;
        // Default patch, expressed as normalized knob values per page/pot.
        // (These reproduce the v0.1 default voice; see mapping in BuildParams.)
        static const float kDefaults[PAGE_COUNT][8] = {
            //  VR1     VR2     VR3    VR4    VR5     VR6     VR7    VR8
            { 0.333f, 0.781f, 0.333f, 0.5f, 0.566f, 0.919f, 0.35f, 0.15f }, // LIVE  (Tune ~200 Hz; VR7/8 = FX sends)
            { 0.25f,  0.5f,   0.0f,   0.0f, 0.8f,   0.0f,   0.0f,  0.25f }, // SHAPE (DS71: square, no sub/drive; VR5 = Volume 0.8)
            { 0.0f,   0.630f, 0.01f,  0.0f, 0.0f,   0.0f,   0.5f,  0.0f  }, // DEEP
            { 0.627f, 0.41f,  0.584f, 0.2f, 0.6f,   0.719f, 0.15f, 0.0f  }, // FX    (delay 300 ms, fb .45, tape tone, flutter, reverb)
        };
        for(int pg = 0; pg < PAGE_COUNT; pg++)
            for(int k = 0; k < 8; k++)
            {
                val_[pg][k]    = kDefaults[pg][k];
                caught_[pg][k] = false;   // released until the knob is moved through
                entry_[pg][k]  = -1.f;    // set on first read
            }
        first_ = true;
    }

    // Set page directly and re-arm pickup. Used by main.cpp's 5-page UI
    // (4 sound + 1 preset) which manages the cycle externally.
    void SetPage(Page p, const float pots[8])
    {
        if(p >= PAGE_COUNT) return;
        page_ = p;
        for(int k = 0; k < 8; k++)
        {
            caught_[page_][k] = false;
            entry_[page_][k]  = pots[k];
        }
    }

    Page CurrentPage() const { return page_; }

    // Copy the 8 normalized FX-page knob values (for the FX bus).
    void GetFxPage(float out[8]) const { for(int k = 0; k < 8; k++) out[k] = val_[PAGE_FX][k]; }

    // Read the 8 raw 0..1 knob values for any sound page (for LED feedback).
    void GetPageValues(Page p, float out[8]) const
    {
        if(p >= PAGE_COUNT) return;
        for(int k = 0; k < 8; k++) out[k] = val_[p][k];
    }

    // Export all 4 sound pages as a flat 4×8 grid (for preset save).
    void GetAllValues(float out[PAGE_COUNT][8]) const
    {
        for(int pg = 0; pg < PAGE_COUNT; pg++)
            for(int k = 0; k < 8; k++)
                out[pg][k] = val_[pg][k];
    }

    // Import all 4 sound pages from a preset and release every knob so
    // nothing jumps until the user touches it.
    void SetAllValues(const float in[PAGE_COUNT][8], const float pots[8])
    {
        for(int pg = 0; pg < PAGE_COUNT; pg++)
            for(int k = 0; k < 8; k++)
            {
                val_[pg][k]    = in[pg][k];
                caught_[pg][k] = false;
                entry_[pg][k]  = pots[k];
            }
    }

    // Call once per control update with the 8 live pot readings (0..1).
    void ProcessPots(const float pots[8])
    {
        if(first_)
        {
            for(int k = 0; k < 8; k++) entry_[page_][k] = pots[k];
            first_ = false;
        }
        for(int k = 0; k < 8; k++)
        {
            float v = pots[k];
            // Catch-on-move: a knob takes over once nudged past the threshold from where
            // it sat when this page was entered. Threshold > typical pot jitter.
            if(!caught_[page_][k] && fabsf(v - entry_[page_][k]) > kCatchMove)
                caught_[page_][k] = true;
            if(caught_[page_][k]) val_[page_][k] = v;
        }
    }

    // Map all three pages of normalized knob values into the voice parameters.
    void BuildParams(DubSirenParams& p) const
    {
        const float* L = val_[PAGE_LIVE];
        const float* S = val_[PAGE_SHAPE];
        const float* D = val_[PAGE_DEEP];

        // --- LIVE ---
        p.tune_hz      = expmap(L[0], 30.f, 9000.f);   // DS71 Tune range
        p.lfo_rate_hz  = expmap(L[1], 0.1f, 15.f);
        p.lfo_depth_oct= L[2] * 3.f;
        p.sweep_oct    = (L[3] - 0.5f) * 4.f;
        p.decay_ms     = expmap(L[4], 20.f, 4000.f);
        p.cutoff_hz    = expmap(L[5], 120.f, 18000.f);
        p.delay_send   = L[6];                 // inert until the FX stage
        p.reverb_send  = L[7];                 // inert until the FX stage

        // --- SHAPE ---
        p.osc_wave     = (OscWave)quant(S[0], (int)OscWave::COUNT);
        p.pulse_width  = 0.05f + 0.90f * S[1];
        p.sub_level    = S[2];
        p.drive        = S[3];
        p.level        = S[4];                 // siren volume (was Noise; Noise moved to FX VR8)
        p.lfo_wave     = (LfoWave)quant(S[5], (int)LfoWave::COUNT);
        p.lfo_swing    = S[6] * 0.75f;
        p.resonance    = S[7] * 0.90f;   // capped below self-destruct; voice clamps too

        // --- DEEP ---
        p.glide_ms     = D[0] * 500.f;
        p.sweep_ms     = expmap(D[1], 1.f, 2000.f);
        p.attack_ms    = D[2] * 1000.f;   // 0–1 s so a slow swell is clearly audible
        p.filt_env_amt = D[3];
        p.lfo_fade_ms  = D[4] * 2000.f;
        p.chop_depth   = D[5];
        p.chop_ratio   = expmap(D[6], 0.5f, 8.f);
        p.lfo_phase    = D[7];

        // FX page VR8 → Noise (moved here from SHAPE VR5 to free that slot for Volume)
        p.noise_level  = val_[PAGE_FX][7];

        // Fixed for now (reach the knobs in later stages)
        p.detune_cents = 0.f;
        p.lfo_slew     = 0.f;
        p.filt_mode    = FiltMode::LP;
        p.lfo_retrig   = true;
    }

    // Map the FX page into the effect-bus parameters.
    void BuildFx(FxParams& f) const
    {
        const float* X = val_[PAGE_FX];
        f.delay_time_ms = expmap(X[0], 20.f, 1100.f);  // capped to the internal-SRAM buffer
        f.feedback      = X[1] * 1.1f;                  // up to 1.1 → self-oscillating howl
        f.tape_tone     = expmap(X[2], 800.f, 10000.f); // tape bandwidth (feedback LP)
        f.flutter       = X[3] * 0.01f;                 // wow/flutter depth
        f.rev_decay     = 0.70f + X[4] * 0.25f;         // reverb size 0.70..0.95
        f.rev_tone      = expmap(X[5], 1000.f, 18000.f);
        f.drive         = X[6];                         // tape saturation
    }

  private:
    static constexpr float kCatchMove = 0.03f; // knob travel needed to grab a param

    static float expmap(float x, float lo, float hi)
    {
        if(x < 0.f) x = 0.f; else if(x > 1.f) x = 1.f;
        return lo * powf(hi / lo, x);
    }
    static int quant(float x, int n) // 0..1 -> 0..n-1
    {
        int i = (int)(x * n);
        if(i < 0) i = 0; else if(i >= n) i = n - 1;
        return i;
    }

    Page  page_ = PAGE_LIVE;
    float val_[PAGE_COUNT][8];
    bool  caught_[PAGE_COUNT][8];
    float entry_[PAGE_COUNT][8];
    bool  first_ = true;
};
} // namespace dubsauce
