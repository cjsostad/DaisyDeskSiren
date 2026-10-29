// DubSirenVoice.h — DubSauce dub siren engine (v0.1.1, hardware-independent)
// Target: Daisy Seed3 / libDaisy v9.0.0 + DaisySP. Header-only so it drops in beside main.cpp.
//
// This is the VOICE only: it renders a mono siren sample and exposes two FX-send
// scalars (delay/reverb) for the shared bus we build later. No hardware I/O here.
//
// Staging (see DubSiren_Seed3_ControlMap.md):
//   v0.1 : osc select + sub + noise + drive, 8-shape LFO wail w/ slew/swing,
//          pitch-env (Sweep), amp env (gate/one-shot), SVF filter, glide, chopper.
//   next : true variable-PW PolyBLEP osc, tempo-sync divisions, preset serialise.
//
// v0.1.1 fixes vs v0.1: per-sample oscillator waveform is now set explicitly for
//   every mode (no stale carryover between Sine/Pulse/Tri), and amp-env settings are
//   applied *before* the envelope is processed.
//
// NOTE: not compiled here — written against the DaisySP API to compile/iterate on your Mac.

#pragma once
#include "daisysp.h"
#include <cmath>

namespace dubsauce
{
enum class OscWave  { Sine, Pulse, Saw, Tri, Blend, RingFM, COUNT };
enum class LfoWave  { Sine, Tri, Square, RampUp, RampDn, SampHold, ExpEnv, LogEnv, COUNT };
enum class FiltMode { LP, BP, HP, COUNT };
enum class TrigMode { Gate, OneShot };

struct DubSirenParams
{
    // --- Oscillator / voice ---
    float    tune_hz      = 220.f;   // 30..9000, exp-mapped upstream
    OscWave  osc_wave     = OscWave::Pulse;   // Benidub DS71 core: square oscillator
    float    pulse_width  = 0.50f;   // Pulse: width (0.5=square). Blend: square/saw mix
    float    sub_level    = 0.30f;   // -1 oct, the fullness lever
    float    detune_cents = 0.f;     // 2nd osc; 0 = off (reserved; not yet summed in v0.1)
    float    noise_level  = 0.0f;
    float    drive        = 0.0f;    // 0..1 -> tanh into fold
    float    glide_ms     = 0.0f;

    // --- LFO (wail) ---
    float    lfo_rate_hz  = 5.0f;    // free; sync handled upstream later
    float    lfo_depth_oct= 1.0f;    // 0..3
    LfoWave  lfo_wave     = LfoWave::Sine;
    float    lfo_swing    = 0.0f;    // 0..0.75
    float    lfo_slew     = 0.0f;    // 0..1
    bool     lfo_retrig   = true;
    float    lfo_phase    = 0.0f;    // retrig start phase 0..1
    float    lfo_fade_ms  = 0.0f;    // depth fade-in

    // --- Chopper (LFO2 gate) ---
    float    chop_depth   = 0.0f;    // 0=off..1=full gate
    float    chop_ratio   = 2.0f;    // xLFO rate

    // --- Pitch env (Sweep / Synare dive) ---
    float    sweep_oct    = 0.0f;    // +/- octaves
    float    sweep_ms     = 120.f;

    // --- Amp env ---
    float    attack_ms    = 2.0f;
    float    decay_ms     = 400.f;   // = DS71 "Decay" (tail)
    TrigMode trig_mode    = TrigMode::Gate;

    // --- Filter ---
    float    cutoff_hz    = 12000.f;
    float    resonance    = 0.2f;    // 0..1 (Svf res)
    FiltMode filt_mode    = FiltMode::LP;
    float    filt_env_amt = 0.0f;    // 0..1 amp-env -> cutoff

    // --- FX sends + level ---
    float    delay_send   = 0.35f;
    float    reverb_send  = 0.15f;
    float    level        = 0.8f;
};

// --- small LFO: 8 discrete shapes, retrig, swing, slew ---
class SirenLfo
{
  public:
    void  Init(float sr) { sr_ = sr; phase_ = 0.f; held_ = 0.f; last_ = 0.f; }
    void  Retrig(float startPhase) { phase_ = startPhase - floorf(startPhase); }
    float PeekLast() const { return last_; }
    float Process(float rate_hz, LfoWave w, float swing, float slew)
    {
        phase_ += rate_hz / sr_;
        if(phase_ >= 1.f) { phase_ -= 1.f; held_ = frand_(); } // S&H latches at wrap
        float p = phase_;
        if(swing > 0.f) // compress first half, expand second (bounded 0..1)
        {
            float s = swing * 0.5f;
            p = (p < 0.5f) ? p * (1.f - s) : 0.5f + (p - 0.5f) * (1.f + s);
            p = daisysp::fclamp(p, 0.f, 0.999999f);
        }
        float v = 0.f;
        switch(w)
        {
            case LfoWave::Sine:     v = sinf(p * TWOPI_); break;
            case LfoWave::Tri:      v = (p < 0.5f) ? (4.f * p - 1.f) : (3.f - 4.f * p); break;
            case LfoWave::Square:   v = (p < 0.5f) ? 1.f : -1.f; break;
            case LfoWave::RampUp:   v = 2.f * p - 1.f; break;
            case LfoWave::RampDn:   v = 1.f - 2.f * p; break;
            case LfoWave::SampHold: v = held_; break;
            case LfoWave::ExpEnv:   v = 2.f * (1.f - p) * (1.f - p) - 1.f; break; // falling exp-ish
            case LfoWave::LogEnv:   v = 2.f * sqrtf(1.f - p) - 1.f; break;        // falling log-ish
            default: break;
        }
        float coeff = 1.f - slew * 0.9995f; // slew-limit (fonepole-style)
        last_ += coeff * (v - last_);
        return last_;
    }

  private:
    static constexpr float TWOPI_ = 6.28318530718f;
    float frand_() { seed_ = seed_ * 1664525u + 1013904223u; return (seed_ >> 8) * (2.f / 16777216.f) - 1.f; }
    float sr_ = 48000.f, phase_ = 0.f, held_ = 0.f, last_ = 0.f;
    unsigned seed_ = 0x1234567u;
};

class DubSirenVoice
{
  public:
    void Init(float samplerate)
    {
        sr_ = samplerate;
        // Keep filter cutoff inside the SVF's stable band (< ~sr/3), and never above 18 kHz.
        maxCutoff_ = (sr_ * 0.33f < 18000.f) ? sr_ * 0.33f : 18000.f;
        sq_.Init(sr_);   sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
        saw_.Init(sr_);  saw_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SAW);
        sub_.Init(sr_);  sub_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
        mod_.Init(sr_);  mod_.SetWaveform(daisysp::Oscillator::WAVE_SIN);   // ring/FM modulator
        chop_.Init(sr_);
        noise_.Init();
        lfo_.Init(sr_);
        filt_.Init(sr_);
        amp_.Init(sr_);
        fenv_.Init(sr_);
        glideFreq_ = 220.f;
    }

    // Rising edge = (re)trigger. Pass the note frequency the key/MIDI/Tune selected.
    void Gate(bool g, float target_hz)
    {
        targetHz_ = target_hz;
        if(g && !gate_)
        {
            if(p_.lfo_retrig) lfo_.Retrig(p_.lfo_phase);
            penvPos_ = 0.f;                                   // restart pitch env
            fade_    = (p_.lfo_fade_ms <= 0.f) ? 1.f : 0.f;   // restart depth fade-in
        }
        gate_ = g;
    }

    void SetParams(const DubSirenParams& p) { p_ = p; }

    float Process()
    {
        // ---- glide base pitch ----
        float gc = (p_.glide_ms <= 0.f) ? 1.f : 1.f - expf(-1.f / (0.001f * p_.glide_ms * sr_));
        glideFreq_ += gc * (targetHz_ - glideFreq_);

        // ---- LFO wail + fade-in ----
        if(fade_ < 1.f && p_.lfo_fade_ms > 0.f) fade_ += 1.f / (0.001f * p_.lfo_fade_ms * sr_);
        fade_ = daisysp::fclamp(fade_, 0.f, 1.f);
        float lfo = lfo_.Process(p_.lfo_rate_hz, p_.lfo_wave, p_.lfo_swing, p_.lfo_slew);

        // ---- pitch env (one-shot fall 1->0) ----
        float penv = 0.f;
        if(p_.sweep_ms > 0.f)
        {
            penv = 1.f - penvPos_;
            if(penv < 0.f) penv = 0.f;
            penvPos_ += 1.f / (0.001f * p_.sweep_ms * sr_);
        }

        // ---- combine to frequency (octave modulation) ----
        float octs = lfo * p_.lfo_depth_oct * fade_ + penv * p_.sweep_oct;
        float freq = daisysp::fclamp(glideFreq_ * exp2f(octs), 10.f, 18000.f);

        // ---- oscillator core (waveform set explicitly every sample) ----
        sq_.SetFreq(freq);
        saw_.SetFreq(freq);
        float s;
        switch(p_.osc_wave)
        {
            case OscWave::Sine:
                sq_.SetWaveform(daisysp::Oscillator::WAVE_SIN);
                s = sq_.Process();
                break;
            case OscWave::Tri:
                sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_TRI);
                s = sq_.Process();
                break;
            case OscWave::Pulse: // true variable-width pulse: 0.5 = square, off-centre = narrow
                sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
                sq_.SetPw(p_.pulse_width);
                s = sq_.Process();
                break;
            case OscWave::Saw:
                s = saw_.Process();
                break;
            case OscWave::Blend: // square + saw mix (optional extra voice)
                sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
                sq_.SetPw(0.5f);
                s = p_.pulse_width * sq_.Process() + (1.f - p_.pulse_width) * saw_.Process();
                break;
            case OscWave::RingFM:
                sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
                mod_.SetFreq(freq * 1.5f);
                s = sq_.Process() * mod_.Process();
                break;
            default:
                sq_.SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
                s = sq_.Process();
                break;
        }

        // ---- sub + noise ----
        if(p_.sub_level > 0.f)   { sub_.SetFreq(freq * 0.5f); s += p_.sub_level * sub_.Process(); }
        if(p_.noise_level > 0.f) { noise_.SetAmp(p_.noise_level); s += noise_.Process(); }

        // ---- drive -> fold ----
        if(p_.drive > 0.f)
        {
            s = tanhf(s * (1.f + p_.drive * 8.f));
            if(p_.drive > 0.7f) s = sinf(s * (1.f + (p_.drive - 0.7f) * 3.f)); // light fold at the top
        }

        // ---- amp env (settings first, then process) ----
        applyAmpSettings_();
        float envA = amp_.Process(gate_);

        // ---- dedicated filter envelope: an AD "pluck" that fires on trigger and falls
        //      independently of the amp sustain, so Filter Env Amount produces a real sweep ----
        fenv_.SetAttackTime(0.002f);
        fenv_.SetDecayTime(daisysp::fclamp(0.001f * p_.decay_ms, 0.01f, 1.5f));
        fenv_.SetSustainLevel(0.f);
        fenv_.SetReleaseTime(0.05f);
        float fe = fenv_.Process(gate_);

        // ---- filter (filter envelope opens the cutoff) ----
        float co = daisysp::fclamp(p_.cutoff_hz * (1.f + p_.filt_env_amt * fe * 3.f), 20.f, maxCutoff_);
        filt_.SetFreq(co);
        filt_.SetRes(daisysp::fclamp(p_.resonance, 0.f, 0.90f));
        filt_.Process(s);
        switch(p_.filt_mode)
        {
            case FiltMode::LP: s = filt_.Low();  break;
            case FiltMode::BP: s = filt_.Band(); break;
            case FiltMode::HP: s = filt_.High(); break;
            default:           s = filt_.Low();  break;
        }
        // Safety net: if the resonant filter ever goes non-finite (NaN/Inf), reset it so a
        // high-resonance blow-up can't silence the whole voice until a reboot.
        if(!std::isfinite(s)) { filt_.Init(sr_); s = 0.f; }

        // ---- VCA + chopper ----
        s *= envA;
        if(p_.chop_depth > 0.f)
        {
            float c = 0.5f * (chop_.Process(p_.lfo_rate_hz * p_.chop_ratio, LfoWave::Square, 0.f, 0.f) + 1.f);
            s *= (1.f - p_.chop_depth) + p_.chop_depth * c;
        }
        // Gentle always-on analog-style softclip — emulates the DS71's discrete-transistor
        // VCA warmth so the square doesn't read as sterile/digital. Near-unity for small
        // signals, soft compression on peaks.
        s = tanhf(s * 1.3f) * (1.f / 1.3f);
        return s * p_.level;
    }

    // FX-send scalars for the shared bus (built later). Voice stays dry.
    float DelaySend()  const { return p_.delay_send; }
    float ReverbSend() const { return p_.reverb_send; }
    // Expose the latest wail value (bipolar) so the host can mirror it to a CV out.
    float LfoValue()   const { return lfo_.PeekLast(); }

  private:
    void applyAmpSettings_()
    {
        amp_.SetAttackTime(0.001f * p_.attack_ms);
        if(p_.trig_mode == TrigMode::Gate)
        {
            amp_.SetDecayTime(0.005f);
            amp_.SetSustainLevel(1.f);
            amp_.SetReleaseTime(0.001f * p_.decay_ms); // tail on release
        }
        else // OneShot: decay to zero regardless of hold
        {
            amp_.SetDecayTime(0.001f * p_.decay_ms);
            amp_.SetSustainLevel(0.f);
            amp_.SetReleaseTime(0.001f * p_.decay_ms);
        }
    }

    DubSirenParams p_;
    daisysp::Oscillator sq_, saw_, sub_, mod_;
    daisysp::WhiteNoise noise_;
    daisysp::Svf        filt_;
    daisysp::Adsr       amp_;
    daisysp::Adsr       fenv_;   // dedicated filter envelope
    SirenLfo            lfo_, chop_;
    float sr_ = 48000.f, glideFreq_ = 220.f, targetHz_ = 220.f;
    float penvPos_ = 1.f, fade_ = 1.f, maxCutoff_ = 15000.f;
    bool  gate_ = false;
};
} // namespace dubsauce
