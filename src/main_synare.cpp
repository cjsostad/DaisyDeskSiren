/** DubSauce — Seed3 Desktop SYNARE drum engine, bench build v0.1
 *
 *  Standalone bring-up harness for SynareVoice (Star Instruments Synare 3 / Troots TR7 model),
 *  mirroring the siren's bench main so you can flash it, hit a pad and dial the voice. It runs
 *  the Synare through the SAME lightweight FxBus (tape echo + reverb) the siren uses, so hits
 *  land dub-ready; the board's line inputs feed the same sends.
 *
 *  This REPLACES the siren app on the Seed3 while it's flashed (BOOT_SRAM holds one app). Flash
 *  the siren's `make program-dfu` again to switch back. The merged multi-engine build comes next.
 *
 *  SHIFT pad (S9) cycles two pot pages: VOICE -> SHAPE. Bottom-row LED count shows the page.
 *
 *  Trigger (percussion is ONE-SHOT — each hit rings out over Decay):
 *    SW18 up     = normal: one hit per rising edge (S1 pad / GATE IN 1 / MIDI note)
 *    SW18 center = mute (ignore triggers)
 *    SW18 down   = roll: auto-retrigger at the LFO rate while S1 / GATE held (dub rolls)
 *  S1 LED lit while the hit sounds · CV OUT 1 mirrors the amp envelope (trigger/duck other gear).
 *  MIDI note-on plays it chromatically with velocity.
 *
 *  POT PAGES:
 *    VOICE: VR1 Tune  VR2 Sweep(ctr=none,CCW dive/CW up)  VR3 SweepTime  VR4 Decay
 *           VR5 Cutoff  VR6 Resonance  VR7 FilterEnv(ctr=none)  VR8 Noise
 *    SHAPE: VR1 OscWave(Sine/Tri/Sqr/Saw)  VR2 Osc2 Interval(ctr=unison)  VR3 Osc2 Level
 *           VR4 LFO Rate  VR5 LFO Depth  VR6 Click/Snap  VR7 DelaySend  VR8 ReverbSend
 */
#include "desktop_devkit.h"
#include "SynareVoice.h"
#include "FxBus.h"

using namespace daisy;
using namespace daisy::desktop_devkit;
using namespace dubsauce;

static constexpr int PAD_TRIGGER = 0; // S1 (top-left)
static constexpr int PAD_SHIFT   = 8; // S9 (bottom-left)
static constexpr int BOTTOM_ROW  = 8;

Hardware     hw;
SynareVoice  synare;
SynareParams params;
FxParams     fxp;

static int  page_    = 0;            // 0 = VOICE, 1 = SHAPE
static const int SYN_PAGE_COUNT = 2;

// --- default boot patch (normalized 0..1 per page/pot) ---
// Loaded at power-up regardless of physical knob positions; a knob re-captures its
// parameter only once moved (catch-on-move pickup), same as the siren's ControlShell.
//   VOICE: VR1 Tune  VR2 Sweep  VR3 SweepTime  VR4 Decay  VR5 Cutoff  VR6 Res  VR7 FiltEnv  VR8 Noise
//   SHAPE: VR1 Wave  VR2 Osc2Int  VR3 Osc2Lvl  VR4 LFORate  VR5 LFODepth  VR6 Click  VR7 DlySend  VR8 RevSend
static const float kSynDefaults[SYN_PAGE_COUNT][8] = {
    { 0.60f, 0.60f, 0.80f, 0.50f, 0.60f, 0.80f, 0.50f, 0.00f }, // VOICE
    { 0.00f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f }, // SHAPE
};

// knob-pickup state
static float valN_[SYN_PAGE_COUNT][8];   // the live patch values
static bool  caught_[SYN_PAGE_COUNT][8]; // has this knob been grabbed on this page?
static float entry_[SYN_PAGE_COUNT][8];  // pot reading when the page was entered
static bool  firstRead_ = true;
static constexpr float kCatchMove = 0.03f;

// MIDI -> audio-callback handoff (parsed in the main loop, consumed in the callback)
static volatile bool  midiReq  = false;
static volatile float midiFreq = 220.f;
static volatile float midiVel  = 1.f;

// roll state
static uint32_t rollCount = 0;

static volatile bool  sounding = false;
static volatile float potDisp[8] = {0.f};   // live knob positions, for the knob LEDs

// --- small mapping helpers (match ControlShell's maths) ---
static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float expmap(float x, float lo, float hi)
{
    x = clamp01(x);
    return lo * powf(hi / lo, x);
}
static inline int quant(float x, int n)
{
    int i = (int)(clamp01(x) * n);
    return (i >= n) ? n - 1 : i;
}

// Catch-on-move pickup: idle knobs hold the stored patch; a knob grabs its parameter
// once nudged past the threshold from where it sat when the page was entered.
static void ProcessPots(const float pots[8])
{
    if(firstRead_)
    {
        for(int k = 0; k < 8; k++) entry_[page_][k] = pots[k];
        firstRead_ = false;
    }
    for(int k = 0; k < 8; k++)
    {
        if(!caught_[page_][k] && fabsf(pots[k] - entry_[page_][k]) > kCatchMove)
            caught_[page_][k] = true;
        if(caught_[page_][k]) valN_[page_][k] = pots[k];
    }
}

static void NextPage(const float pots[8])   // re-arm pickup for the page just entered
{
    page_ = (page_ + 1) % SYN_PAGE_COUNT;
    for(int k = 0; k < 8; k++) { caught_[page_][k] = false; entry_[page_][k] = pots[k]; }
}

// Map both pages of stored knob values into the voice parameters.
static void BuildParams()
{
    const float* V = valN_[0]; // VOICE
    const float* S = valN_[1]; // SHAPE

    params.tune_hz      = expmap(V[0], 30.f, 4000.f);
    params.sweep_oct    = (V[1] - 0.5f) * 8.f;          // -4 .. +4 oct, center = none
    params.sweep_ms     = expmap(V[2], 1.f, 2000.f);
    params.decay_ms     = expmap(V[3], 10.f, 3000.f);
    params.cutoff_hz    = expmap(V[4], 120.f, 18000.f);
    params.resonance    = V[5] * 0.92f;
    params.filt_env_amt = (V[6] - 0.5f) * 2.f;          // -1 .. +1, center = none
    params.noise_level  = V[7];

    params.osc_wave     = (SynWave)quant(S[0], (int)SynWave::COUNT);
    params.osc2_semi    = (S[1] - 0.5f) * 48.f;         // -24 .. +24 semitones
    params.osc2_level   = S[2];
    params.lfo_rate_hz  = expmap(S[3], 0.1f, 40.f);
    params.lfo_depth_oct= S[4] * 2.f;
    params.click_level  = S[5];
    params.delay_send   = S[6];
    params.reverb_send  = S[7];
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.UpdateAllControls();

    float pots[8];
    for(int i = 0; i < 8; i++) { pots[i] = hw.pot[i].Value(); potDisp[i] = pots[i]; }
    if(hw.button[PAD_SHIFT].RisingEdge()) NextPage(pots);
    ProcessPots(pots);
    BuildParams();
    synare.SetParams(params);

    // --- triggers ---
    int  tpos    = hw.tog_sw18.Read();
    bool padEdge = hw.button[PAD_TRIGGER].RisingEdge() || hw.gate_in[0].Trig();
    bool padHeld = hw.button[PAD_TRIGGER].Pressed()    || hw.gate_in[0].State();

    switch(tpos)
    {
        case Switch3::POS_UP:
            if(padEdge) synare.Trig(1.f);
            break;
        case Switch3::POS_DOWN: // roll while held, at the LFO rate
        {
            float    rate = (params.lfo_rate_hz < 0.1f) ? 0.1f : params.lfo_rate_hz;
            uint32_t ival = (uint32_t)(hw.seed.AudioSampleRate() / rate);
            if(padEdge) { synare.Trig(1.f); rollCount = 0; }
            else if(padHeld)
            {
                rollCount += size;
                if(rollCount >= ival) { synare.Trig(1.f); rollCount = 0; }
            }
            break;
        }
        default: break; // center = mute
    }

    // MIDI note (edge-triggered one-shot, from the main loop)
    if(midiReq) { synare.Trig(midiVel, midiFreq); midiReq = false; }

    sounding = synare.Sounding();
    hw.SetDacOut(DacHandle::Channel::ONE,
                 (uint16_t)daisysp::fclamp(synare.EnvValue() * 4095.f, 0.f, 4095.f));

    for(size_t i = 0; i < size; i++)
    {
        float s = synare.Process();
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
    System::Delay(50);
    synare.Init(hw.seed.AudioSampleRate());
    FxInit(hw.seed.AudioSampleRate());

    // load the default boot patch; knobs stay released until moved
    for(int pg = 0; pg < SYN_PAGE_COUNT; pg++)
        for(int k = 0; k < 8; k++)
        {
            valN_[pg][k]   = kSynDefaults[pg][k];
            caught_[pg][k] = false;
            entry_[pg][k]  = -1.f;
        }
    firstRead_ = true;

    // fixed dub-ish FX bed for the bench (sends are live on VR7/VR8)
    fxp.delay_time_ms = 400.f;
    fxp.feedback      = 0.50f;
    fxp.tape_tone     = 3500.f;
    fxp.flutter       = 0.002f;
    fxp.rev_decay     = 0.85f;
    fxp.rev_tone      = 8000.f;
    fxp.drive         = 0.15f;

    hw.seed.StartAudio(AudioCallback);

    uint32_t last_led = System::GetNow();
    while(true)
    {
        while(hw.midi.HasEvents())
        {
            auto m = hw.midi.PopEvent();
            if(m.type == NoteOn && m.data[1] > 0)
            {
                midiFreq = daisysp::mtof((float)m.data[0]);
                midiVel  = (float)m.data[1] / 127.f;
                midiReq  = true;
            }
        }

        uint32_t now = System::GetNow();
        if(now - last_led >= 16)
        {
            for(int i = 0; i < 16; i++) hw.SetLed(i, 0.f);
            // top row (0..7): knob positions — full CCW = off, full CW = brightest
            for(int i = 0; i < 8; i++) hw.SetLed(i, daisysp::fclamp(potDisp[i], 0.f, 1.f));
            // bottom row (8..15): page indicator + sounding blink (bottom-right)
            for(int i = 0; i <= page_; i++) hw.SetLed(BOTTOM_ROW + i, 0.6f);
            hw.SetLed(15, sounding ? 1.f : 0.f);
            hw.UpdateLeds();
            last_led = now;
        }
        System::Delay(1);
    }
}
