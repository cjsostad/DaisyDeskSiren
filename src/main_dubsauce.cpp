/** DubSauce — Seed3 Desktop MULTI-ENGINE dub instrument, v0.5 (stage 1)
 *
 *  Siren + Synare in one firmware, both summing into the shared tape-echo/reverb FxBus.
 *    * SIREN  — one monophonic wail voice.
 *    * SYNARE — a pool of NUM_SYN voices so drum hits overlap and ring out (polyphonic).
 *  A preset is "which engine + its settings". 8 slots, each either a siren or a Synare sound;
 *  tap any of the 8 pads on the PRESET page to play it, whatever engine it holds.
 *
 *  STAGE 1: presets live in RAM (reset on power-off). Stage 2 adds QSPI-flash persistence.
 *
 *  CONTROLS
 *    SW17 (2-pos)  = EDIT ENGINE: up = Siren, down = Synare (which engine pages 1-4 tune).
 *    SHIFT pad S9  = cycle the 5 pages (tap). Bottom-row LED count = page.
 *                    pages 1-4 tune the current engine (top-row LEDs = knob positions);
 *                    page 5 = PRESETS.
 *    SW18 (3-pos)  = trigger mode for AUDITION on edit pages:
 *                    siren  -> up momentary / center mute / down latch
 *                    synare -> up single hit / center mute / down roll (at LFO rate)
 *                    center also = global mute/panic on the preset page.
 *    S1 pad        = audition the current edit sound (edit pages).
 *    PRESET page (5): tap pad S1-S8 = launch that slot (hold a siren pad to sustain the wail).
 *                     HOLD S9 + tap a pad = SAVE the current edit sound into that slot (LEDs
 *                     flash). Works on either engine; siren slot latches if SW18 down at save.
 *                     HOLD a preset pad >400ms = PERFORM MODE: knobs morph the sound live with
 *                     pickup/offset (preset itself is unchanged). Release to exit cleanly.
 *                       siren perform  : VR1 Tune  VR2 WailRate  VR3 WailDepth  VR4 DelayTime  VR5 DelayFb
 *                       synare perform : VR1 Tune  VR2 Sweep     VR3 DelayTime VR4 DelayFb (slow-rolls)
 *    GATE IN 1 / MIDI note = trigger/play the current edit engine (velocity on MIDI).
 *    CV OUT 1 mirrors the siren wail. Preset-page LEDs: empty off / loaded dim / siren-held bright.
 */
#include "desktop_devkit.h"
#include "MergedShell.h"
#include "FxBus.h"
#include <cstring>

using namespace daisy;
using namespace daisy::desktop_devkit;
using namespace dubsauce;

static constexpr int NUM_SYN     = 4;  // Synare polyphony (6 was tight on CPU during fast rolls)
static constexpr int PAD_SHIFT   = 8;  // S9 (bottom-left)
static constexpr int BOTTOM_ROW  = 8;

Hardware        hw;
MergedShell     shell;
DubSirenVoice   siren;
SynareVoice     synPool[NUM_SYN];
DubSirenParams  sirenEdit;             // live-dialed siren sound
SynareParams    synEdit;               // live-dialed synare sound
FxParams        fxp;

struct Preset
{
    uint8_t        engine = 0;         // 0 empty, 1 siren, 2 synare
    uint8_t        latch  = 0;         // siren: 0 momentary, 1 latch
    DubSirenParams siren;
    SynareParams   synare;
};

// --- STAGE 2: the 8-slot bank persisted to the Seed3's QSPI flash ---
// Stored at a high offset well clear of the BOOT_SRAM app image (which lives at 0x90040000).
// Bump BANK_MAGIC whenever Preset's layout changes, so stale flash from an old build is ignored.
static constexpr uint32_t BANK_MAGIC    = 0xDB5A0001;
static constexpr uint32_t BANK_OFFSET   = 0x400000;   // ~4 MB into QSPI, far from the app

struct PresetBank
{
    uint32_t magic;
    Preset   slots[8];
    bool operator!=(const PresetBank& o) const { return memcmp(this, &o, sizeof(PresetBank)) != 0; }
    bool operator==(const PresetBank& o) const { return !(*this != o); }
};

static daisy::PersistentStorage<PresetBank> gBank(hw.seed.qspi);
static volatile bool savePending_ = false;   // callback sets it; main loop flushes to flash
static Preset* slots() { return gBank.GetSettings().slots; }   // live RAM copy of the bank

// siren control state
enum SirenSrc { SRC_EDIT, SRC_PRESET };
static SirenSrc       sirenSrc_     = SRC_EDIT;
static DubSirenParams sirenLive_;                 // params currently feeding the siren voice
static bool           sirenGate_    = false;      // held/latched gate (non-MIDI)
static bool           sirenLatch_   = false;      // edit-page latch toggle
static int            activeSlot_   = -1;         // which preset slot owns the siren (-1 none)

// synare pool allocator
static int      synRR_ = 0;
static uint32_t rollCount_ = 0;

// SHIFT(S9) is the save modifier on the preset page. The page advances on S9 RELEASE (not
// press), so "hold S9 + tap a pad to save" never pages away first. s9Save_ remembers that the
// hold was used for a save, which suppresses the page advance on release. A pad pressed on its
// own (no S9) always plays and never saves — so holding a pad to sustain a wail is safe.
static bool     prevS9_         = false;
static bool     s9Save_         = false;
static uint32_t saveFlashUntil_ = 0;

// --- PERFORM MODE: hold a preset pad >400ms to morph the live sound with the 8 knobs ---
// Pickup/offset style: knob positions at the moment perform engages become the reference;
// subsequent knob movement adds/subtracts from the preset's stored values. Release = vanish,
// preset itself never changes. Perform works for both engines (Synare slow-rolls while held
// so knob changes are audible on each re-trigger).
static constexpr uint32_t PERF_HOLD_MS    = 400;
static constexpr uint32_t SYN_PERF_ROLLMS = 500;   // retrigger rate while holding a Synare preset
static uint32_t padPressAt_[8]  = {0};
static int      perfSlot_       = -1;              // which slot's perform mode is active (-1 none)
static float    perfKnobRef_[8] = {0.f};           // knob positions captured when perform entered
static uint32_t synPerfNext_    = 0;               // next retrigger time for Synare perform

// MIDI handoff
static volatile bool  midiOn_      = false;   // note held (for the siren)
static volatile bool  midiTrigSyn_ = false;   // a note-on waiting to fire a synare hit
static volatile float midiFreq_    = 220.f;
static volatile float midiVel_     = 1.f;

// UI mirror
static volatile bool  sirenSounding_ = false;
static volatile float potDisp_[8]    = {0.f};

static int allocSyn()
{
    for(int i = 0; i < NUM_SYN; i++) if(!synPool[i].Sounding()) return i;
    int v = synRR_; synRR_ = (synRR_ + 1) % NUM_SYN; return v;   // steal round-robin
}
static void trigSyn(const SynareParams& p, float vel, float note_hz)
{
    int v = allocSyn();
    synPool[v].SetParams(p);
    synPool[v].Trig(vel, note_hz);
}
static void savePreset(int x)
{
    Preset& p = slots()[x];                               // write the RAM copy here (callback-safe)
    p.engine = (shell.Engine() == ENG_SIREN) ? 1 : 2;
    p.latch  = (hw.tog_sw18.Read() == Switch3::POS_DOWN) ? 1 : 0;
    p.siren  = sirenEdit;
    p.synare = synEdit;
    savePending_ = true;                                  // flash write happens in the main loop
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.UpdateAllControls();

    float pots[8];
    for(int i = 0; i < 8; i++) { pots[i] = hw.pot[i].Value(); potDisp_[i] = pots[i]; }

    // SW17 -> edit engine
    shell.SetEngine(hw.tog_sw17.Pressed() ? ENG_SYNARE : ENG_SIREN, pots);

    // SHIFT (S9): tap alone = next page; hold + tap a pad = save (handled in the preset branch).
    bool s9 = hw.button[PAD_SHIFT].Pressed();
    if(s9 && !prevS9_)             s9Save_ = false;         // fresh S9 hold
    if(!s9 && prevS9_ && !s9Save_) shell.NextPage(pots);    // S9 released, not used to save

    shell.ProcessPots(pots);
    shell.BuildSiren(sirenEdit);
    shell.BuildSynare(synEdit);
    shell.BuildFx(fxp);

    const int  sw18 = hw.tog_sw18.Read();
    const bool mute = (sw18 == Switch3::POS_CENTER);

    // ---- triggering ----
    bool s1edge = hw.button[0].RisingEdge() || hw.gate_in[0].Trig();
    bool s1held = hw.button[0].Pressed()    || hw.gate_in[0].State();

    if(mute)
    {
        sirenGate_ = false; sirenLatch_ = false; activeSlot_ = -1; perfSlot_ = -1;
    }
    else if(!shell.OnPresetPage())
    {
        // ---- AUDITION the engine being edited ----
        if(shell.Engine() == ENG_SIREN)
        {
            sirenSrc_ = SRC_EDIT;
            if(sw18 == Switch3::POS_UP)        sirenGate_ = s1held;                 // momentary
            else if(sw18 == Switch3::POS_DOWN) { if(s1edge) sirenLatch_ = !sirenLatch_; sirenGate_ = sirenLatch_; }
        }
        else // SYNARE
        {
            if(sw18 == Switch3::POS_UP) { if(s1edge) trigSyn(synEdit, 1.f, 0.f); }
            else if(sw18 == Switch3::POS_DOWN) // roll at the LFO rate
            {
                float    rate = (synEdit.lfo_rate_hz < 0.1f) ? 0.1f : synEdit.lfo_rate_hz;
                uint32_t ival = (uint32_t)(hw.seed.AudioSampleRate() / rate);
                if(s1edge) { trigSyn(synEdit, 1.f, 0.f); rollCount_ = 0; }
                else if(s1held) { rollCount_ += size; if(rollCount_ >= ival) { trigSyn(synEdit, 1.f, 0.f); rollCount_ = 0; } }
            }
        }
    }
    else
    {
        // ---- PRESET PAGE: tap a pad = launch its slot (hold to sustain a siren wail);
        //      hold S9 + tap a pad = save the current sound there (either engine). ----
        uint32_t tnow = System::GetNow();
        for(int x = 0; x < 8; x++)
        {
            bool edge = hw.button[x].RisingEdge();
            bool held = hw.button[x].Pressed();

            if(s9)                                   // S9 held => pad SAVES, doesn't play
            {
                if(edge) { savePreset(x); s9Save_ = true; saveFlashUntil_ = tnow + 150; }
                continue;
            }

            if(edge) padPressAt_[x] = tnow;

            // Enter PERFORM mode once this pad has been held past the threshold.
            if(held && perfSlot_ == -1 && slots()[x].engine != 0
               && (tnow - padPressAt_[x]) > PERF_HOLD_MS)
            {
                perfSlot_ = x;
                for(int k = 0; k < 8; k++) perfKnobRef_[k] = pots[k];
                synPerfNext_ = tnow + SYN_PERF_ROLLMS;
            }
            // Leave perform mode as soon as the pad owning it is released.
            if(!held && perfSlot_ == x) perfSlot_ = -1;

            Preset& pr = slots()[x];
            if(pr.engine == 2)                              // synare: one-shot
            {
                if(edge) trigSyn(pr.synare, 1.f, 0.f);
                // While perform is on this slot, slow-roll with the knob-modified params so
                // tweaks become audible as you move the knobs.
                if(held && perfSlot_ == x && tnow >= synPerfNext_)
                {
                    SynareParams sp = pr.synare;
                    // VR1 Tune (-0.5..+0.5 -> ±1 octave); VR2 Sweep (±2 oct); FX handled below
                    sp.tune_hz  *= exp2f((pots[0] - perfKnobRef_[0]) * 2.f);
                    sp.sweep_oct += (pots[1] - perfKnobRef_[1]) * 4.f;
                    trigSyn(sp, 1.f, 0.f);
                    synPerfNext_ = tnow + SYN_PERF_ROLLMS;
                }
            }
            else if(pr.engine == 1)                         // siren: hold or latch
            {
                if(pr.latch)
                {
                    if(edge)
                    {
                        if(activeSlot_ == x) { sirenGate_ = false; activeSlot_ = -1; }
                        else { sirenSrc_ = SRC_PRESET; sirenLive_ = pr.siren; sirenGate_ = true; activeSlot_ = x; }
                    }
                }
                else // momentary
                {
                    if(edge) { sirenSrc_ = SRC_PRESET; sirenLive_ = pr.siren; sirenGate_ = true; activeSlot_ = x; }
                    else if(activeSlot_ == x && !held) { sirenGate_ = false; activeSlot_ = -1; }
                }
            }
        }

        // Apply PERFORM offsets. CRITICAL: rebuild sirenLive_ from the stored preset every
        // callback first — otherwise the per-callback multiplication would stack at 48 kHz and
        // ramp the sound off a cliff within milliseconds. fxp is already rebuilt by shell.BuildFx
        // above each callback, so applying offsets to it once here is safe.
        if(perfSlot_ >= 0)
        {
            const Preset& pr = slots()[perfSlot_];
            if(pr.engine == 1)                                     // siren
            {
                sirenLive_             = pr.siren;                 // reset to stored each frame
                sirenLive_.tune_hz    *= exp2f((pots[0] - perfKnobRef_[0]) * 2.f); // VR1 ±1 oct
                sirenLive_.lfo_rate_hz *= exp2f((pots[1] - perfKnobRef_[1]) * 3.f); // VR2 wail rate
                sirenLive_.lfo_depth_oct = daisysp::fclamp(
                    sirenLive_.lfo_depth_oct + (pots[2] - perfKnobRef_[2]) * 3.f, 0.f, 3.f); // VR3
                fxp.delay_time_ms *= exp2f((pots[3] - perfKnobRef_[3]) * 2.f);     // VR4
                fxp.feedback = daisysp::fclamp(fxp.feedback + (pots[4] - perfKnobRef_[4]) * 0.5f,
                                               0.f, 1.1f);                          // VR5
            }
            else                                                   // synare FX overlay (VR3/VR4)
            {
                fxp.delay_time_ms *= exp2f((pots[2] - perfKnobRef_[2]) * 2.f);
                fxp.feedback = daisysp::fclamp(fxp.feedback + (pots[3] - perfKnobRef_[3]) * 0.5f,
                                               0.f, 1.1f);
            }
            // guard: a user-induced blow-up of tune_hz would hang the oscillator, so clamp here
            sirenLive_.tune_hz = daisysp::fclamp(sirenLive_.tune_hz, 10.f, 9000.f);
        }
    }
    prevS9_ = s9;

    // ---- drive the siren voice ----
    if(sirenSrc_ == SRC_EDIT) sirenLive_ = sirenEdit;       // live knob tracking while auditioning
    siren.SetParams(sirenLive_);
    bool  midiToSiren = midiOn_ && (shell.Engine() == ENG_SIREN) && !mute;
    bool  g = sirenGate_ || midiToSiren;
    float f = midiToSiren ? midiFreq_ : sirenLive_.tune_hz;
    siren.Gate(g, f);
    sirenSounding_ = g;

    // MIDI note -> synare hit (when editing synare)
    if(midiTrigSyn_)
    {
        if(shell.Engine() == ENG_SYNARE && !mute) trigSyn(synEdit, midiVel_, midiFreq_);
        midiTrigSyn_ = false;
    }

    hw.SetDacOut(DacHandle::Channel::ONE,
                 (uint16_t)daisysp::fclamp((siren.LfoValue() * 0.5f + 0.5f) * 4095.f, 0.f, 4095.f));

    // ---- render: siren + synare pool -> shared FX, each voice with its own sends ----
    for(size_t i = 0; i < size; i++)
    {
        float s   = siren.Process();
        float dry = s;
        float sD  = s * sirenLive_.delay_send;
        float sR  = s * sirenLive_.reverb_send;
        for(int v = 0; v < NUM_SYN; v++)
        {
            if(!synPool[v].Sounding()) continue;   // idle voices cost nothing (big CPU save)
            float x = synPool[v].Process();
            dry += x;
            sD  += x * synPool[v].DelaySend();
            sR  += x * synPool[v].ReverbSend();
        }
        float oL, oR;
        FxProcessMix(dry, sD, sR, in[0][i], in[1][i], fxp, oL, oR);
        out[0][i] = oL;
        out[1][i] = oR;
    }
}

int main(void)
{
    hw.Init();
    System::Delay(50);
    float sr = hw.seed.AudioSampleRate();
    siren.Init(sr);
    for(int v = 0; v < NUM_SYN; v++) synPool[v].Init(sr);
    shell.Init();
    FxInit(sr);

    // load the preset bank from QSPI flash (or write empty defaults on first-ever boot / layout change)
    PresetBank defaults;
    defaults.magic = BANK_MAGIC;
    for(int i = 0; i < 8; i++) defaults.slots[i] = Preset();   // empty slots
    gBank.Init(defaults, BANK_OFFSET);
    if(gBank.GetSettings().magic != BANK_MAGIC) gBank.RestoreDefaults();

    // brief boot beacon: all 16 LEDs ~0.6s, so you can confirm startup finished
    for(int i = 0; i < 16; i++) hw.SetLed(i, 1.f);
    hw.UpdateLeds();
    System::Delay(600);

    hw.seed.StartAudio(AudioCallback);

    uint32_t last_led = System::GetNow();
    while(true)
    {
        // flush a pending preset save to flash here (NEVER in the audio callback)
        if(savePending_) { gBank.Save(); savePending_ = false; }

        while(hw.midi.HasEvents())
        {
            auto m = hw.midi.PopEvent();
            if(m.type == NoteOn)
            {
                if(m.data[1] > 0)
                {
                    midiFreq_    = daisysp::mtof((float)m.data[0]);
                    midiVel_     = (float)m.data[1] / 127.f;
                    midiOn_      = true;
                    midiTrigSyn_ = true;
                }
                else midiOn_ = false;
            }
            else if(m.type == NoteOff) midiOn_ = false;
        }

        uint32_t now = System::GetNow();
        if(now - last_led >= 16)
        {
            for(int i = 0; i < 16; i++) hw.SetLed(i, 0.f);
            int page = shell.Page();

            if(page < EDIT_PAGES)
            {
                // top row = knob positions; bottom-right = edit engine (dim siren / bright synare)
                for(int i = 0; i < 8; i++) hw.SetLed(i, daisysp::fclamp(potDisp_[i], 0.f, 1.f));
                hw.SetLed(15, shell.Engine() == ENG_SYNARE ? 0.9f : 0.25f);
            }
            else
            {
                // preset page: slot state — empty off / loaded dim / active siren slot bright.
                // In PERFORM mode, swap the top row to live knob positions so you can see what
                // you're doing with the overlay knobs.
                if(perfSlot_ >= 0)
                    for(int i = 0; i < 8; i++) hw.SetLed(i, daisysp::fclamp(potDisp_[i], 0.f, 1.f));
                else
                    for(int x = 0; x < 8; x++)
                    {
                        float b = 0.f;
                        if(slots()[x].engine != 0) b = 0.25f;
                        if(activeSlot_ == x && sirenSounding_) b = 1.f;
                        hw.SetLed(x, b);
                    }
            }
            for(int i = 0; i <= page; i++) hw.SetLed(BOTTOM_ROW + i, 0.6f);  // page indicator
            if(now < saveFlashUntil_) for(int i = 0; i < 16; i++) hw.SetLed(i, 1.f); // "saved" flash
            hw.UpdateLeds();
            last_led = now;
        }
        System::Delay(1);
    }
}
