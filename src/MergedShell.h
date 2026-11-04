// MergedShell.h — DubSauce merged control surface (siren + Synare), v0.1
//
// One control surface driving BOTH engines. SW17 selects which engine the pot pages edit;
// SHIFT cycles a fixed 5-page set: pages 0..3 edit the current engine, page 4 = presets.
// Per-engine, per-page knob PICKUP (catch-on-move) so switching engine or page never scrambles
// an untouched parameter — same model as the siren's ControlShell, extended to two engines.
//
// Pure logic, no hardware calls. main_dubsauce.cpp feeds it the 8 pot readings + SW17 and asks
// it to fill DubSirenParams / SynareParams / FxParams.
//
// Page layout (fixed, so "page 5 = presets" is always true regardless of engine):
//   SIREN edit:  0 LIVE   1 SHAPE  2 DEEP   3 FX
//   SYNARE edit: 0 VOICE  1 SHAPE  2 DEEP   3 (reserved)
//   BOTH:        4 PRESETS (launch/save — knobs idle here)
// FX is global and edited on the SIREN FX page (page 3 while SW17 = Siren).

#pragma once
#include "SynareVoice.h"
#include "ControlShell.h"   // FxParams + siren enums (DubSirenVoice.h comes in via both)
#include <cmath>

namespace dubsauce
{
enum EditEngine { ENG_SIREN = 0, ENG_SYNARE = 1, ENG_COUNT = 2 };

static constexpr int EDIT_PAGES  = 4;   // pages 0..3 edit the current engine
static constexpr int PRESET_PAGE = 4;   // page 4 = preset launch/save
static constexpr int MERGE_PAGES = 5;   // total SHIFT pages

class MergedShell
{
  public:
    void Init()
    {
        engine_ = ENG_SIREN;
        page_   = 0;
        first_  = true;

        // siren defaults = the siren bench's default patch (per ControlShell)
        static const float sirenDef[EDIT_PAGES][8] = {
            { 0.333f, 0.781f, 0.333f, 0.5f, 0.566f, 0.919f, 0.35f, 0.15f }, // LIVE
            { 0.25f,  0.5f,   0.0f,   0.0f, 0.0f,   0.0f,   0.0f,  0.25f }, // SHAPE
            { 0.0f,   0.630f, 0.01f,  0.0f, 0.0f,   0.0f,   0.5f,  0.0f  }, // DEEP
            { 0.627f, 0.41f,  0.584f, 0.2f, 0.6f,   0.719f, 0.15f, 0.0f  }, // FX
        };
        // synare defaults = Chris's dialed boot patch + sensible DEEP page
        static const float synDef[EDIT_PAGES][8] = {
            { 0.60f, 0.60f, 0.80f, 0.50f, 0.60f, 0.80f, 0.50f, 0.00f }, // VOICE
            { 0.00f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f }, // SHAPE
            { 0.00f, 0.00f, 0.10f, 0.00f, 0.00f, 1.00f, 0.85f, 0.00f }, // DEEP (atk,filtT,drive,vel,mode,osc1,level,-)
            { 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f }, // reserved
        };

        for(int pg = 0; pg < EDIT_PAGES; pg++)
            for(int k = 0; k < 8; k++)
            {
                val_[ENG_SIREN][pg][k]  = sirenDef[pg][k];
                val_[ENG_SYNARE][pg][k] = synDef[pg][k];
                caught_[ENG_SIREN][pg][k] = caught_[ENG_SYNARE][pg][k] = false;
                entry_[ENG_SIREN][pg][k]  = entry_[ENG_SYNARE][pg][k]  = -1.f;
            }
    }

    // SW17 -> edit engine. Re-arm pickup for the newly-shown page so nothing jumps.
    void SetEngine(EditEngine e, const float pots[8])
    {
        if(e == engine_) return;
        engine_ = e;
        if(page_ < EDIT_PAGES) rearm_(engine_, page_, pots);
    }

    // SHIFT -> next page (wraps through the 5). Re-arm pickup when landing on an edit page.
    void NextPage(const float pots[8])
    {
        page_ = (page_ + 1) % MERGE_PAGES;
        if(page_ < EDIT_PAGES) rearm_(engine_, page_, pots);
    }

    void ProcessPots(const float pots[8])
    {
        if(page_ >= EDIT_PAGES) return;         // presets page: knobs idle
        if(first_)
        {
            for(int k = 0; k < 8; k++) entry_[engine_][page_][k] = pots[k];
            first_ = false;
        }
        for(int k = 0; k < 8; k++)
        {
            if(!caught_[engine_][page_][k] && fabsf(pots[k] - entry_[engine_][page_][k]) > kCatchMove)
                caught_[engine_][page_][k] = true;
            if(caught_[engine_][page_][k]) val_[engine_][page_][k] = pots[k];
        }
    }

    int        Page()          const { return page_; }
    bool       OnPresetPage()  const { return page_ == PRESET_PAGE; }
    EditEngine Engine()        const { return engine_; }

    // --- SIREN params (pages LIVE/SHAPE/DEEP; mirrors ControlShell) ---
    void BuildSiren(DubSirenParams& p) const
    {
        const float* L = val_[ENG_SIREN][0];
        const float* S = val_[ENG_SIREN][1];
        const float* D = val_[ENG_SIREN][2];

        p.tune_hz       = expmap(L[0], 30.f, 9000.f);
        p.lfo_rate_hz   = expmap(L[1], 0.1f, 15.f);
        p.lfo_depth_oct = L[2] * 3.f;
        p.sweep_oct     = (L[3] - 0.5f) * 4.f;
        p.decay_ms      = expmap(L[4], 20.f, 4000.f);
        p.cutoff_hz     = expmap(L[5], 120.f, 18000.f);
        p.delay_send    = L[6];
        p.reverb_send   = L[7];

        p.osc_wave      = (OscWave)quant(S[0], (int)OscWave::COUNT);
        p.pulse_width   = 0.05f + 0.90f * S[1];
        p.sub_level     = S[2];
        p.drive         = S[3];
        p.noise_level   = S[4];
        p.lfo_wave      = (LfoWave)quant(S[5], (int)LfoWave::COUNT);
        p.lfo_swing     = S[6] * 0.75f;
        p.resonance     = S[7] * 0.90f;

        p.glide_ms      = D[0] * 500.f;
        p.sweep_ms      = expmap(D[1], 1.f, 2000.f);
        p.attack_ms     = D[2] * 1000.f;
        p.filt_env_amt  = D[3];
        p.lfo_fade_ms   = D[4] * 2000.f;
        p.chop_depth    = D[5];
        p.chop_ratio    = expmap(D[6], 0.5f, 8.f);
        p.lfo_phase     = D[7];

        p.detune_cents  = 0.f;
        p.lfo_slew      = 0.f;
        p.filt_mode     = FiltMode::LP;
        p.lfo_retrig    = true;
        p.trig_mode     = TrigMode::Gate;
        p.level         = 0.8f;
    }

    // --- SYNARE params (pages VOICE/SHAPE/DEEP) ---
    void BuildSynare(SynareParams& p) const
    {
        const float* V = val_[ENG_SYNARE][0];
        const float* S = val_[ENG_SYNARE][1];
        const float* D = val_[ENG_SYNARE][2];

        p.tune_hz       = expmap(V[0], 30.f, 4000.f);
        p.sweep_oct     = (V[1] - 0.5f) * 8.f;
        p.sweep_ms      = expmap(V[2], 1.f, 2000.f);
        p.decay_ms      = expmap(V[3], 10.f, 3000.f);
        p.cutoff_hz     = expmap(V[4], 120.f, 18000.f);
        p.resonance     = V[5] * 0.92f;
        p.filt_env_amt  = (V[6] - 0.5f) * 2.f;
        p.noise_level   = V[7];

        p.osc_wave      = (SynWave)quant(S[0], (int)SynWave::COUNT);
        p.osc2_semi     = (S[1] - 0.5f) * 48.f;
        p.osc2_level    = S[2];
        p.lfo_rate_hz   = expmap(S[3], 0.1f, 40.f);
        p.lfo_depth_oct = S[4] * 2.f;
        p.click_level   = S[5];
        p.delay_send    = S[6];
        p.reverb_send   = S[7];

        p.attack_ms     = D[0] * 200.f;
        p.filt_ms       = D[1] * 1500.f;     // 0 => follow Decay (voice handles <=0)
        p.drive         = D[2];
        p.vel_amount    = D[3];
        p.filt_mode     = (FiltMode)quant(D[4], (int)FiltMode::COUNT);
        p.osc1_level    = D[5];
        p.level         = D[6];
    }

    // --- global FX (siren FX page) ---
    void BuildFx(FxParams& f) const
    {
        const float* X = val_[ENG_SIREN][3];
        f.delay_time_ms = expmap(X[0], 20.f, 1500.f);
        f.feedback      = X[1] * 1.1f;
        f.tape_tone     = expmap(X[2], 800.f, 10000.f);
        f.flutter       = X[3] * 0.01f;
        f.rev_decay     = 0.70f + X[4] * 0.25f;
        f.rev_tone      = expmap(X[5], 1000.f, 18000.f);
        f.drive         = X[6];
    }

  private:
    static constexpr float kCatchMove = 0.03f;

    void rearm_(int e, int pg, const float pots[8])
    {
        for(int k = 0; k < 8; k++) { caught_[e][pg][k] = false; entry_[e][pg][k] = pots[k]; }
    }
    static float expmap(float x, float lo, float hi)
    {
        if(x < 0.f) x = 0.f; else if(x > 1.f) x = 1.f;
        return lo * powf(hi / lo, x);
    }
    static int quant(float x, int n)
    {
        int i = (int)(x * n);
        if(i < 0) i = 0; else if(i >= n) i = n - 1;
        return i;
    }

    EditEngine engine_ = ENG_SIREN;
    int        page_   = 0;
    bool       first_  = true;
    float val_[ENG_COUNT][EDIT_PAGES][8];
    bool  caught_[ENG_COUNT][EDIT_PAGES][8];
    float entry_[ENG_COUNT][EDIT_PAGES][8];
};
} // namespace dubsauce
