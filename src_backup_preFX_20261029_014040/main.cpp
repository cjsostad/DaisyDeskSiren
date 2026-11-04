/** DubSauce — Seed3 Desktop dub siren, bench build v0.2
 *
 *  Adds the control shell's paging core on top of the v0.1 voice:
 *    - SHIFT pad (S9, bottom-left) cycles the 3 pot pages: LIVE -> SHAPE -> DEEP -> ...
 *    - All 24 parameters are now live on the knobs (knob PICKUP prevents jumps on page flip)
 *    - Bottom-row LED count shows the page: Live=1 (S9), Shape=2, Deep=3, lit from the left
 *
 *  Trigger is unchanged from v0.1 (deliberately — keyboard + PLAY/PERFORM come in v0.3):
 *    SW18  UP momentary / CENTER mute / DOWN latch
 *    S1    trigger pad       GATE IN 1  external trigger      MIDI note  pitch + gate
 *    S1 LED lit while sounding          CV OUT 1 mirrors the wail
 *
 *  POT PAGES (turn SHIFT = pad S13 to change page):
 *    LIVE : VR1 Tune  VR2 WailRate  VR3 WailDepth  VR4 Sweep  VR5 Decay  VR6 Tone  VR7 DelaySend* VR8 ReverbSend*
 *    SHAPE: VR1 OscWave VR2 PW/Blend VR3 Sub  VR4 Drive  VR5 Noise  VR6 LFOWave  VR7 Swing  VR8 Resonance
 *    DEEP : VR1 Glide  VR2 SweepTime VR3 Attack VR4 FiltEnv VR5 LFOFade VR6 ChopDepth VR7 ChopRate VR8 PhaseOff
 *    (*DelaySend/ReverbSend are silent until the FX bus stage.)
 */
#include "desktop_devkit.h"
#include "DubSirenVoice.h"
#include "ControlShell.h"

using namespace daisy;
using namespace daisy::desktop_devkit;
using namespace dubsauce;

// Pad indices (S1..S16 -> 0..15). Layout is TWO ROWS OF EIGHT:
//   top row  S1..S8  = indices 0..7
//   bottom   S9..S16 = indices 8..15
static constexpr int PAD_TRIGGER = 0;   // S1  (top-left)
static constexpr int PAD_SHIFT   = 8;   // S9  (bottom-left)
static constexpr int BOTTOM_ROW  = 8;   // first index of the bottom row

Hardware       hw;
DubSirenVoice  siren;
ControlShell   shell;
DubSirenParams params;

static bool           latched  = false;
static volatile bool  midiGate = false;
static volatile float midiFreq = 220.f;
static volatile bool  sounding = false;
static volatile int   pageLed  = 0; // 0 Live,1 Shape,2 Deep (for main-loop LED)

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.UpdateAllControls();

    // ---- gather pots, run paging + pickup ----
    float pots[8];
    for(int i = 0; i < 8; i++) pots[i] = hw.pot[i].Value();
    if(hw.button[PAD_SHIFT].RisingEdge()) shell.NextPage(pots);
    shell.ProcessPots(pots);
    shell.BuildParams(params);
    pageLed = (int)shell.CurrentPage();

    // ---- trigger (v0.1 behaviour) ----
    int  tpos = hw.tog_sw18.Read();
    bool held = hw.button[PAD_TRIGGER].Pressed() || hw.gate_in[0].State();
    bool edge = hw.button[PAD_TRIGGER].RisingEdge() || hw.gate_in[0].Trig();
    bool gate = false;
    switch(tpos)
    {
        case Switch3::POS_UP:   gate = held; break;                        // momentary
        case Switch3::POS_DOWN: if(edge) latched = !latched; gate = latched; break; // latch
        case Switch3::POS_CENTER:
        default:                gate = false; latched = false; break;      // mute
    }

    bool  g = gate || midiGate;
    float f = midiGate ? midiFreq : params.tune_hz;
    params.trig_mode = TrigMode::Gate;

    siren.SetParams(params);
    siren.Gate(g, f);
    sounding = g;

    hw.SetDacOut(DacHandle::Channel::ONE,
                 (uint16_t)daisysp::fclamp((siren.LfoValue() * 0.5f + 0.5f) * 4095.f, 0.f, 4095.f));

    for(size_t i = 0; i < size; i++)
    {
        float s   = siren.Process();
        out[0][i] = s;
        out[1][i] = s;
    }
}

int main(void)
{
    hw.Init();
    System::Delay(50);
    siren.Init(hw.seed.AudioSampleRate());
    shell.Init();

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

        uint32_t now = System::GetNow();
        if(now - last_led >= 16) // ~60 Hz LED refresh
        {
            // Clear everything first so no stray LEDs linger.
            for(int i = 0; i < 16; i++) hw.SetLed(i, 0.f);
            // Page indicator: count of lit LEDs from the bottom-left = page+1.
            //   Live = S9 · Shape = S9+S10 · Deep = S9+S10+S11
            for(int i = 0; i <= pageLed; i++) hw.SetLed(BOTTOM_ROW + i, 0.6f);
            // Sounding indicator on S1 (top-left)
            hw.SetLed(PAD_TRIGGER, sounding ? 1.f : 0.f);
            hw.UpdateLeds();
            last_led = now;
        }
        System::Delay(1);
    }
}
