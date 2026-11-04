// SynareVoice.h — DubSauce Synare-style drum/percussion engine (v0.1, hardware-independent)
// Target: Daisy Seed3 / libDaisy v9.0.0 + DaisySP. Header-only, drops in beside DubSirenVoice.h.
//
// This is the VOICE only: it renders one mono percussion hit per trigger. No hardware I/O here,
// and it deliberately REUSES the shared building blocks already in DubSirenVoice.h — the 8-shape
// SirenLfo and the LfoWave / FiltMode enums — so the two engines stay consistent and small.
//
// MODEL (faithful to the Star Instruments Synare 3 + Troots TR7 "Snyare", the voicing targets):
//   (OSC1 + OSC2 + Noise + attack-click)  ->  resonant SVF low-pass  ->  VCA
//   with three independent percussive (one-shot) envelopes:
//     * amp env    — attack + exponential decay = the "Decay" ring-out (Synare master Decay)
//     * pitch sweep— decaying offset applied to BOTH oscillators; sign = dive-down / sweep-up.
//                    This is the Synare "SWEEP": negative => the classic descending disco tom.
//     * filter env — decaying offset applied to cutoff; bipolar so the filter can sweep up OR down.
//   plus a free-running LFO -> pitch (the TR7 "modulation": sine/saw warble, laser & siren-perc).
//
//   The real Synare 3 gets its purest "dive tom" by running the 4-pole OTA filter into
//   self-oscillation (resonance full = a sine source) and sweeping it down. A digital TPT SVF
//   won't sustain true self-oscillation cleanly, so here the dive tom is the SINE oscillator with
//   the pitch sweep; crank Resonance for the characteristic peak/whistle on top. Same result to
//   the ear, no blow-up risk. (Resonance is clamped below the SVF's unstable band, with a NaN net.)
//
// Trigger model: percussion is ONE-SHOT. A rising edge (pad / gate / MIDI note) fires the whole
//   hit and it rings out over Decay regardless of how long the trigger is held. Velocity (0..1)
//   scales level by Vel Amount. An optional note frequency overrides Tune for chromatic play.
//
// NOTE: not compiled in this workspace (no ARM toolchain here) — written against the exact DaisySP
//   API the siren voice already uses, to compile/iterate on your Mac like DubSirenVoice v0.1 did.

#pragma once
#include "daisysp.h"
#include "DubSirenVoice.h"   // SirenLfo, LfoWave, FiltMode (all in namespace dubsauce)
#include <cmath>

namespace dubsauce
{
enum class SynWave { Sine, Tri, Square, Saw, COUNT };

struct SynareParams
{
    // --- Oscillators ---
    float    tune_hz      = 180.f;   // 30..4000, exp-mapped upstream. Synare toms sit low.
    SynWave  osc_wave     = SynWave::Sine;  // Sine = disco tom; Square/Saw = zap/clang
    float    osc1_level   = 1.0f;
    float    osc2_semi    = 0.0f;    // OSC2 interval vs OSC1, semitones (-24..+24). 0 = unison.
    float    osc2_level   = 0.0f;    // metallic/clang when detuned & mixed in (Synare 2-osc)
    float    noise_level  = 0.0f;    // white-noise bed: snare / cymbal / clap textures
    float    click_level  = 0.0f;    // short attack noise burst = beater/stick snap
    float    drive        = 0.10f;   // tanh pre-filter grit

    // --- Pitch sweep (the Synare "SWEEP" — the signature dive) ---
    float    sweep_oct    = -2.0f;   // +/- octaves. NEGATIVE = dive down (disco tom default).
    float    sweep_ms     = 160.f;   // sweep decay time (1..2000)

    // --- Filter ---
    float    cutoff_hz    = 6000.f;
    float    resonance    = 0.45f;   // 0..~0.92; high = ringing peak / pseudo self-osc
    FiltMode filt_mode    = FiltMode::LP;
    float    filt_env_amt = 0.0f;    // -1..+1: env -> cutoff, bipolar (sweep filter up or down)
    float    filt_ms      = 0.0f;    // filter-env decay; <=0 => follow amp Decay

    // --- LFO -> pitch (TR7 modulation) ---
    float    lfo_rate_hz  = 6.0f;
    float    lfo_depth_oct= 0.0f;    // 0..2
    LfoWave  lfo_wave     = LfoWave::Sine;

    // --- Amp env (percussive) ---
    float    attack_ms    = 1.0f;    // ~instant
    float    decay_ms     = 300.f;   // the ring-out ("Decay")

    // --- Velocity / output ---
    float    vel_amount   = 0.0f;    // 0 = ignore velocity (full hit), 1 = fully velocity-scaled
    float    level        = 0.85f;

    // --- FX sends (consumed by the shared FxBus, same as the siren) ---
    float    delay_send   = 0.30f;
    float    reverb_send  = 0.15f;
};

// --- tiny one-shot percussion envelope: linear attack, exponential decay, retriggerable ---
class PercEnv
{
  public:
    void  Init(float sr) { sr_ = sr; stage_ = 0; val_ = 0.f; SetTimes(1.f, 300.f); }
    void  SetTimes(float atk_ms, float dec_ms)
    {
        atkInc_ = (atk_ms <= 0.f) ? 1.f : 1.f / (0.001f * atk_ms * sr_);
        float ds = 0.001f * dec_ms * sr_;
        if(ds < 1.f) ds = 1.f;
        decCoef_ = expf(-9.210340f / ds);   // reach -80 dB (~1e-4) in dec_ms
    }
    void  Trig() { stage_ = 1; }             // start attack from current value (click-free retrig)
    float Value() const { return val_; }
    bool  Active() const { return stage_ != 0; }
    float Process()
    {
        if(stage_ == 1)                       // attack
        {
            val_ += atkInc_;
            if(val_ >= 1.f) { val_ = 1.f; stage_ = 2; }
        }
        else if(stage_ == 2)                  // exponential decay
        {
            val_ *= decCoef_;
            if(val_ < 1.0e-4f) { val_ = 0.f; stage_ = 0; }
        }
        return val_;
    }

  private:
    float sr_ = 48000.f, val_ = 0.f, atkInc_ = 1.f, decCoef_ = 0.999f;
    int   stage_ = 0;                         // 0 idle, 1 attack, 2 decay
};

class SynareVoice
{
  public:
    void Init(float samplerate)
    {
        sr_        = samplerate;
        maxCutoff_ = (sr_ * 0.33f < 18000.f) ? sr_ * 0.33f : 18000.f;
        osc1_.Init(sr_); osc1_.SetWaveform(daisysp::Oscillator::WAVE_SIN);
        osc2_.Init(sr_); osc2_.SetWaveform(daisysp::Oscillator::WAVE_SIN);
        noise_.Init();   noise_.SetAmp(1.f);
        filt_.Init(sr_);
        lfo_.Init(sr_);
        ampEnv_.Init(sr_);
        pitchEnv_.Init(sr_);
        filtEnv_.Init(sr_);
        // click decays to ~zero in ~2 ms
        clickCoef_ = expf(-9.210340f / (0.002f * sr_));
    }

    void SetParams(const SynareParams& p) { p_ = p; }

    // Fire one hit. vel 0..1 (pads/MIDI); note_hz > 0 overrides Tune for chromatic play.
    void Trig(float vel = 1.f, float note_hz = 0.f)
    {
        vel_     = daisysp::fclamp(vel, 0.f, 1.f);
        noteHz_  = (note_hz > 0.f) ? note_hz : 0.f;
        ampEnv_.SetTimes(p_.attack_ms, p_.decay_ms);
        pitchEnv_.SetTimes(0.5f, p_.sweep_ms);               // ~instant attack, decaying sweep
        filtEnv_.SetTimes(0.5f, (p_.filt_ms > 0.f) ? p_.filt_ms : p_.decay_ms);
        ampEnv_.Trig(); pitchEnv_.Trig(); filtEnv_.Trig();
        click_ = p_.click_level;
    }

    // Convenience edge-detector for a held button/gate: fires once on the rising edge.
    void Gate(bool g, float vel = 1.f, float note_hz = 0.f)
    {
        if(g && !gate_) Trig(vel, note_hz);
        gate_ = g;
    }

    float Process()
    {
        float amp   = ampEnv_.Process();
        float penv  = pitchEnv_.Process();                   // 1 -> 0
        float fenv  = filtEnv_.Process();                    // 1 -> 0
        float lfo   = (p_.lfo_depth_oct > 0.f)
                        ? lfo_.Process(p_.lfo_rate_hz, p_.lfo_wave, 0.f, 0.f) : 0.f;

        // ---- pitch: base * sweep * lfo ----
        float base = (noteHz_ > 0.f) ? noteHz_ : p_.tune_hz;
        float octs = p_.sweep_oct * penv + p_.lfo_depth_oct * lfo;
        float f1   = daisysp::fclamp(base * exp2f(octs), 10.f, 18000.f);

        osc1_.SetFreq(f1);
        setWave_(osc1_, p_.osc_wave);
        float s = p_.osc1_level * osc1_.Process();

        if(p_.osc2_level > 0.f)
        {
            float f2 = daisysp::fclamp(f1 * exp2f(p_.osc2_semi / 12.f), 10.f, 18000.f);
            osc2_.SetFreq(f2);
            setWave_(osc2_, p_.osc_wave);
            s += p_.osc2_level * osc2_.Process();
        }

        // ---- noise bed + attack click ----
        if(p_.noise_level > 0.f) s += p_.noise_level * noise_.Process();
        if(click_ > 1.0e-4f)     { s += click_ * noise_.Process(); click_ *= clickCoef_; }

        // ---- drive ----
        if(p_.drive > 0.f) s = tanhf(s * (1.f + p_.drive * 8.f));

        // ---- resonant filter (bipolar env -> cutoff, up to +/-4 oct) ----
        // Fast path: when filt_env_amt is ~0 (the default), skip the per-sample exp2f entirely.
        // exp2f is one of the biggest per-sample costs in this voice; this shaves measurable CPU
        // when several voices are ringing at once.
        float co = (fabsf(p_.filt_env_amt) < 1e-4f)
                    ? p_.cutoff_hz
                    : p_.cutoff_hz * exp2f(p_.filt_env_amt * fenv * 4.f);
        co = daisysp::fclamp(co, 20.f, maxCutoff_);
        filt_.SetFreq(co);
        filt_.SetRes(daisysp::fclamp(p_.resonance, 0.f, 0.92f));
        filt_.Process(s);
        switch(p_.filt_mode)
        {
            case FiltMode::LP: s = filt_.Low();  break;
            case FiltMode::BP: s = filt_.Band(); break;
            case FiltMode::HP: s = filt_.High(); break;
            default:           s = filt_.Low();  break;
        }
        if(!std::isfinite(s)) { filt_.Init(sr_); s = 0.f; }  // high-res blow-up safety net

        // ---- VCA (amp env + velocity) ----
        float vscale = (1.f - p_.vel_amount) + p_.vel_amount * vel_;
        s *= amp * vscale;

        // gentle always-on softclip (discrete-VCA warmth, matches the siren voice)
        s = tanhf(s * 1.3f) * (1.f / 1.3f);
        return s * p_.level;
    }

    // Host hooks (LED / CV mirror / FX bus), same shape as the siren voice.
    float EnvValue()   const { return ampEnv_.Value(); }     // 0..1 — CV OUT / S-pad LED / sounding
    bool  Sounding()   const { return ampEnv_.Active(); }
    float DelaySend()  const { return p_.delay_send; }
    float ReverbSend() const { return p_.reverb_send; }

  private:
    static void setWave_(daisysp::Oscillator& o, SynWave w)
    {
        switch(w)
        {
            case SynWave::Sine:   o.SetWaveform(daisysp::Oscillator::WAVE_SIN);            break;
            case SynWave::Tri:    o.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_TRI);   break;
            case SynWave::Square: o.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);break;
            case SynWave::Saw:    o.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SAW);   break;
            default:              o.SetWaveform(daisysp::Oscillator::WAVE_SIN);            break;
        }
    }

    SynareParams p_;
    daisysp::Oscillator osc1_, osc2_;
    daisysp::WhiteNoise noise_;
    daisysp::Svf        filt_;
    SirenLfo            lfo_;
    PercEnv             ampEnv_, pitchEnv_, filtEnv_;
    float sr_ = 48000.f, maxCutoff_ = 15000.f;
    float vel_ = 1.f, noteHz_ = 0.f;
    float click_ = 0.f, clickCoef_ = 0.99f;
    bool  gate_ = false;
};
} // namespace dubsauce
