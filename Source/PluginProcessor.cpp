#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;
using namespace bzzz;

static int gmToTrack (int note)
{
    switch (note)
    {
        case 35: case 36: return 0;  case 48: return 1;
        case 38: case 40: return 2;  case 39: return 3;
        case 42: case 44: return 4;  case 46: return 5;
        case 51: case 59: return 6;  case 37: case 56: return 7;
        case 41: case 43: case 45: case 47: return 8;
        case 50: return 9;  case 52: return 10;
        case 49: case 57: return 11;
        default: break;
    }
    if (note >= 36 && note <= 59) return (note - 36) % 12;
    return -1;
}

APVTS::ParameterLayout BzzzProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    using FR = juce::NormalisableRange<float>;
    auto add = [&p] (const char* id, const char* nm, FR r, float def)
    { p.push_back (std::make_unique<juce::AudioParameterFloat> (id, nm, r, def)); };

    add ("cutoff", "Master Cutoff", FR (40.f, 18000.f, 0.f, .25f), 16000.f);
    add ("res",    "Master Reso",   FR (.3f, 12.f, 0.f, .4f), .9f);
    add ("drive",  "Master Drive",  FR (0.f, 1.f), .18f);
    add ("volume", "Master Volume", FR (0.f, 1.f), .85f);
    add ("swing",  "Swing",         FR (0.f, 60.f), 6.f);
    add ("dlymix", "Delay Mix",     FR (0.f, .6f), .14f);
    add ("revmix", "Reverb Mix",    FR (0.f, .8f), .16f);
    add ("sc",     "Sidechain",     FR (0.f, 1.f), .35f);
    add ("scut",   "Synth Cutoff",  FR (60.f, 12000.f, 0.f, .3f), 600.f);
    add ("sres",   "Synth Reso",    FR (.3f, 18.f, 0.f, .5f), 6.f);
    add ("senv",   "Synth Env Amt", FR (0.f, 1.f), .75f);
    add ("slfor",  "Synth LFO Rate",FR (.1f, 12.f, 0.f, .5f), 5.f);
    add ("slfoa",  "Synth LFO Amt", FR (0.f, 1.f), 0.f);
    add ("svol",   "Synth Volume",  FR (0.f, 1.f), .6f);
    return { p.begin(), p.end() };
}

BzzzProcessor::BzzzProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BZZZ2", createLayout())
{
    pCut = apvts.getRawParameterValue ("cutoff");   pRes = apvts.getRawParameterValue ("res");
    pDrv = apvts.getRawParameterValue ("drive");    pVol = apvts.getRawParameterValue ("volume");
    pSwing = apvts.getRawParameterValue ("swing");
    pDlyMix = apvts.getRawParameterValue ("dlymix");
    pRev = apvts.getRawParameterValue ("revmix");   pSC = apvts.getRawParameterValue ("sc");
    pSCut = apvts.getRawParameterValue ("scut");    pSRes = apvts.getRawParameterValue ("sres");
    pSEnv = apvts.getRawParameterValue ("senv");    pSLfoR = apvts.getRawParameterValue ("slfor");
    pSLfoA = apvts.getRawParameterValue ("slfoa");  pSVol = apvts.getRawParameterValue ("svol");

    for (auto& row : grid) for (auto& c : row) c.store (0);
    for (int s = 0; s < kSteps; ++s) { rollNote[s].store (-100); rollAcc[s].store (0); rollSlide[s].store (0); }
    for (auto& m : trackMute) m.store (0);
    for (int i = 0; i < kMaxSong; ++i) { songSlot[i].store (0); songReps[i].store (1); }
    for (auto& m : midiMap) m.store (-1);

    applyStyleSeqs (0);                    // berlin
    for (int i = 0; i < kSlots; ++i)       // slots = copie du pattern initial
    {
        for (int tI = 0; tI < kTracks; ++tI) for (int s = 0; s < kSteps; ++s) slots[i].g[tI][s] = grid[tI][s].load();
        for (int s = 0; s < kSteps; ++s) { slots[i].n[s] = rollNote[s].load(); slots[i].a[s] = rollAcc[s].load(); slots[i].s[s] = rollSlide[s].load(); }
    }
    loadPatch (0);
}

// ---------------- actions ----------------
void BzzzProcessor::applyStyleSeqs (int s)
{
    s = juce::jlimit (0, 5, s);
    styleIdx.store (s);
    int g[kTracks][kSteps];
    stylePattern (s, g);
    for (int tI = 0; tI < kTracks; ++tI) for (int st = 0; st < kSteps; ++st) grid[tI][st].store (g[tI][st]);
    const auto& rl = kStyleRolls[s];
    for (int st = 0; st < kSteps; ++st)
    { rollNote[st].store (rl.note[st]); rollAcc[st].store (rl.acc[st]); rollSlide[st].store (rl.slide[st]); }
}

void BzzzProcessor::clearPattern()
{ for (auto& row : grid) for (auto& c : row) c.store (0); }

void BzzzProcessor::randomPattern()
{
    static const float dens[kTracks] = {.35f,.28f,.1f,.14f,.6f,.25f,.08f,.2f,.08f,.3f,.12f,.05f};
    auto& rng = juce::Random::getSystemRandom();
    for (int tI = 0; tI < kTracks; ++tI)
        for (int s = 0; s < kSteps; ++s)
            grid[tI][s].store (rng.nextFloat() < dens[tI] ? (rng.nextFloat() < .25f ? 2 : 1) : 0);
    grid[0][0].store (2);
}

void BzzzProcessor::storeCurToSlot()
{
    auto& sl = slots[curSlot.load()];
    for (int tI = 0; tI < kTracks; ++tI) for (int s = 0; s < kSteps; ++s) sl.g[tI][s] = grid[tI][s].load();
    for (int s = 0; s < kSteps; ++s) { sl.n[s] = rollNote[s].load(); sl.a[s] = rollAcc[s].load(); sl.s[s] = rollSlide[s].load(); }
}
void BzzzProcessor::gotoSlot (int i, bool store)
{
    i = juce::jlimit (0, kSlots - 1, i);
    if (store) storeCurToSlot();
    curSlot.store (i);
    const auto& sl = slots[i];
    for (int tI = 0; tI < kTracks; ++tI) for (int s = 0; s < kSteps; ++s) grid[tI][s].store (sl.g[tI][s]);
    for (int s = 0; s < kSteps; ++s) { rollNote[s].store (sl.n[s]); rollAcc[s].store (sl.a[s]); rollSlide[s].store (sl.s[s]); }
}
void BzzzProcessor::copySlotTo (int dest)
{
    storeCurToSlot();
    slots[juce::jlimit (0, kSlots - 1, dest)] = slots[curSlot.load()];
    gotoSlot (dest, false);
}

void BzzzProcessor::loadKit (int i)
{
    i = ((i % kNumKits) + kNumKits) % kNumKits;
    curKit.store (i);
    const Kit k = genKit (i);
    styleIdx.store (k.style);
    for (int tI = 0; tI < kTracks; ++tI)
        for (int s = 0; s < kSteps; ++s) grid[tI][s].store (k.grid[tI][s]);
    { const juce::ScopedLock sl (cfgLock);
      for (int tI = 0; tI < kTracks; ++tI) chCfg[tI] = k.ch[tI]; }
    // le tempo appartient a l'hote : jamais touche (comme la version web)
}

void BzzzProcessor::loadPatch (int i)
{
    i = ((i % kNumPatches) + kNumPatches) % kNumPatches;
    curPatch.store (i);
    const Patch p = genPatch (i);
    { const juce::ScopedLock sl (cfgLock); synCfg = p.syn; }
    for (int s = 0; s < kSteps; ++s)
    { rollNote[s].store (p.note[s]); rollAcc[s].store (p.acc[s]); rollSlide[s].store (p.slide[s]); }
    // reflete les macros synthe dans les parametres automatisables
    auto setP = [this] (const char* id, float v)
    { if (auto* par = apvts.getParameter (id)) par->setValueNotifyingHost (par->convertTo0to1 (v)); };
    setP ("scut", p.syn.cutoff); setP ("sres", p.syn.res); setP ("senv", p.syn.envAmt);
    setP ("slfor", p.syn.lfoRate); setP ("slfoa", p.syn.lfoAmt); setP ("svol", p.syn.vol);
}

void BzzzProcessor::previewPad (int track) { engine.trigger (track, 1.0f); }

// plages {min,max,log} identiques aux knobs de l'interface
struct PRange { float mn, mx; bool lg; };
static const PRange kChR[10] = {
    {30,18000,true},{.05f,24,true},{0,1,false},{0,1,false},{.3f,2.5f,false},
    {-12,12,false},{-1,1,false},{0,1,false},{0,1,false},{0,1,false} };
static const PRange kSyR[19] = {
    {0,1,false},{-24,24,false},{0,40,false},{1,7,false},{0,1,false},{0,1,false},
    {0,1,false},{0,.3f,false},{.03f,1,false},{0,1,false},{0,1,false},{0,1,false},
    {.001f,1,true},{.02f,1.5f,false},{0,1,false},{.02f,2,false},{0,1,false},{0,1,false},{0,1,false} };
static float unmap (const PRange& r, float p)
{ return r.lg ? r.mn * std::pow (r.mx / r.mn, p) : r.mn + p * (r.mx - r.mn); }

void BzzzProcessor::applyCC (int cc, int val)
{
    if (cc < 0 || cc > 127) return;
    const float p = (float) val / 127.0f;
    // mode LEARN : le prochain CC se lie a la cible armee
    const int armed = learnArmed.load();
    if (armed >= 0)
    {
        for (auto& m : midiMap) if (m.load() == armed) m.store (-1);   // une cible = un cc
        midiMap[cc].store (armed);
        learnArmed.store (-1);
        return;
    }
    int t = midiMap[cc].load();
    if (t < 0)
    {
        if (cc == 74 || cc == 1) t = 5000;      // repli web : CC74/CC1 -> cutoff master
        else return;
    }
    if (t >= 5000 && t < 5000 + 14)             // macros hote
    {
        if (auto* par = apvts.getParameter (kMacroIds[t - 5000]))
            par->setValueNotifyingHost (p);
        return;
    }
    const juce::ScopedTryLock sl (cfgLock);
    if (! sl.isLocked()) return;
    if (t >= 3000 && t < 3000 + 19)             // synthe
    {
        const float v = unmap (kSyR[t - 3000], p);
        auto& sc2 = synCfg;
        switch (t - 3000)
        { case 0: sc2.mix=v; break;  case 1: sc2.osc2Pitch=(int)std::round(v); break;
          case 2: sc2.detune=v; break; case 3: sc2.unison=(int)std::round(v); break;
          case 4: sc2.fm=v; break;   case 5: sc2.sub=v; break;
          case 6: sc2.noise=v; break; case 7: sc2.glide=v; break;
          case 8: sc2.fDec=v; break;  case 9: sc2.drive=v; break;
          case 10: sc2.crush=v; break; case 11: sc2.lfoPitch=v; break;
          case 12: sc2.aA=v; break;  case 13: sc2.aD=v; break;
          case 14: sc2.aS=v; break;  case 15: sc2.aR=v; break;
          case 16: sc2.width=v; break; case 17: sc2.sendD=v; break; case 18: sc2.sendR=v; break; }
        return;
    }
    if (t >= 1000 && t < 1000 + 12 * 16)        // tranche drums
    {
        const int track = (t - 1000) / 16, pi = (t - 1000) % 16;
        if (pi >= 10) return;
        const float v = unmap (kChR[pi], p);
        auto& c = chCfg[track];
        switch (pi)
        { case 0: c.cutoff=v; c.fon=true; break; case 1: c.res=v; break;
          case 2: c.drive=v; break; case 3: c.crush=v; break;
          case 4: c.decay=v; break; case 5: c.pitch=v; break;
          case 6: c.pan=v; break;  case 7: c.sendD=v; break;
          case 8: c.sendR=v; break; case 9: c.vol=v; break; }
    }
}

void BzzzProcessor::queueUiNote (int note, bool on)
{
    const juce::SpinLock::ScopedLockType sl (uiNoteLock);
    if (uiNotes.size() < 64) uiNotes.push_back ({ note, on });
}

// ---------------- audio ----------------
void BzzzProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    engine.prepare (sampleRate);
    synth.prepare (sampleRate);
    masterF.reset();
    juce::Reverb::Parameters rp; rp.roomSize = .82f; rp.damping = .35f; rp.width = 1.f; rp.wetLevel = 1.f; rp.dryLevel = 0.f;
    reverb.setParameters (rp); reverb.setSampleRate (sampleRate); reverb.reset();
    const int cap = (int)(sampleRate * 2.5);
    dlyL.assign ((size_t) cap, 0.f); dlyR.assign ((size_t) cap, 0.f);
    dlyBuf = cap; dlyPos = 0;
    lastStep = -1;
}

bool BzzzProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void BzzzProcessor::fireStep (int stepMod)
{
    for (int tI = 0; tI < kTracks; ++tI)
    {
        if (trackMute[tI].load()) continue;
        const int s = grid[tI][stepMod].load();
        if (s > 0) engine.trigger (tI, s == 2 ? 1.0f : 0.68f);
    }
    if (synthPower.load())
    {
        const int n = rollNote[stepMod].load();
        if (n > -50)
        {
            const int root = kStyles[styleIdx.load()].root;
            const int midiN = root + n + (synOct.load() - 2) * 12;
            const float hz = 440.0f * std::pow (2.0f, (midiN - 69) / 12.0f);
            synth.seqNote (hz, rollAcc[stepMod].load() != 0, rollSlide[stepMod].load() != 0);
        }
        else
        {
            // pas vide : release sauf si le pas precedent etait un slide
            const int prev = (stepMod + kSteps - 1) % kSteps;
            if (rollSlide[prev].load() == 0) synth.seqRelease();
        }
    }
}

void BzzzProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    buffer.clear();

    // copie de config thread-safe (une fois par bloc)
    { const juce::ScopedTryLock sl (cfgLock);
      if (sl.isLocked())
      {
          for (int i = 0; i < kTracks; ++i) engine.ch[(size_t) i] = chCfg[i];
          synth.cfg = synCfg;
      } }
    // macros synthe automatisables ecrasent les champs correspondants
    synth.cfg.cutoff = pSCut->load(); synth.cfg.res = pSRes->load();
    synth.cfg.envAmt = pSEnv->load(); synth.cfg.lfoRate = pSLfoR->load();
    synth.cfg.lfoAmt = pSLfoA->load(); synth.cfg.vol = pSVol->load();

    // notes venant du clavier de l'interface
    {
        const juce::SpinLock::ScopedTryLockType sl (uiNoteLock);
        if (sl.isLocked())
        {
            for (auto& [note, on] : uiNotes)
                on ? synth.noteOn (note, 0.85f) : synth.noteOff (note);
            uiNotes.clear();
        }
    }

    // MIDI
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
        {
            const int ch = m.getChannel();
            const int tI = (ch == 10) ? gmToTrack (m.getNoteNumber()) : -1;
            if (tI >= 0) engine.trigger (tI, juce::jmax (.25f, m.getFloatVelocity()));
            else if (ch != 10)
            {
                const int t2 = gmToTrack (m.getNoteNumber());
                if (m.getNoteNumber() < 60 && t2 >= 0 && ! synthPower.load())
                    engine.trigger (t2, juce::jmax (.25f, m.getFloatVelocity()));
                else
                    synth.noteOn (m.getNoteNumber(), m.getFloatVelocity());
            }
        }
        else if (m.isNoteOff()) synth.noteOff (m.getNoteNumber());
        else if (m.isController()) pushCC (0xB0, m.getControllerNumber(), m.getControllerValue());
        else if (m.isAllNotesOff()) synth.allOff();
    }

    // transport hote
    bool playing = false; double ppq = 0, bpm = 120;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto p = pos->getPpqPosition()) ppq = *p;
            if (auto b = pos->getBpm())         bpm = *b;
        }
    hostBpm.store ((float) bpm);
    if (playing && ! wasPlaying)
    {
        lastStep = -1;
        if (songOn.load() && songLen.load() > 0)
        { songPos.store (0); songBar = 0; gotoSlot (songSlot[0].load(), true); }
    }
    if (! playing && wasPlaying) synth.seqRelease();
    wasPlaying = playing;

    const int n = buffer.getNumSamples();
    const double ppqPerSample = bpm / 60.0 / sr;
    const double swingFrac = pSwing->load() / 100.0 * 0.9;

    // temps du delay ping-pong : 0.375 beat (L) et x1.5 (R), synchro tempo
    const double beatSec = 60.0 / bpm;
    dlyLenL = juce::jlimit (32, dlyBuf - 1, (int) (0.375 * beatSec * sr));
    dlyLenR = juce::jlimit (32, dlyBuf - 1, (int) (0.5625 * beatSec * sr));

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    const float cut = pCut->load(), res = pRes->load(), drv = pDrv->load(), vol = pVol->load();
    const float dlyMix = pDlyMix->load(), revMix = pRev->load(), scAmt = pSC->load();
    masterF.set (cut, res, (float) sr);

    double curPpq = ppq;
    // buffers temporaires pour la reverb (bloc entier)
    juce::AudioBuffer<float> revBuf (2, n);
    revBuf.clear();
    float* rvL = revBuf.getWritePointer (0);
    float* rvR = revBuf.getWritePointer (1);

    for (int i = 0; i < n; ++i)
    {
        if (playing)
        {
            const double pos16 = curPpq * 4.0;
            int stepIdx = (int) std::floor (pos16);
            const double frac = pos16 - stepIdx;
            const int stepMod = ((stepIdx % kSteps) + kSteps) % kSteps;
            const bool odd = (stepMod & 1) != 0;
            const bool fire = odd ? (frac >= swingFrac) : true;
            if (fire && stepIdx != lastStep)
            {
                lastStep = stepIdx;
                playStep.store (stepMod);
                if (stepMod == 0 && songOn.load() && songLen.load() > 0 && lastStep > 0)
                {
                    ++songBar;
                    const int sp = songPos.load();
                    if (songBar >= songReps[sp].load())
                    {
                        songBar = 0;
                        const int np = (sp + 1) % songLen.load();
                        songPos.store (np);
                        gotoSlot (songSlot[np].load(), true);
                    }
                }
                fireStep (stepMod);
            }
            curPpq += ppqPerSample;
        }
        else playStep.store (-1);

        float dL, dR, sd, srv;
        engine.render (dL, dR, sd, srv);

        float syL = 0, syR = 0;
        if (synthPower.load()) synth.render (syL, syR);

        // sidechain : le kick duck le synthe + les FX
        const float duck = 1.0f - scAmt * engine.kickPulse;
        syL *= duck; syR *= duck;

        // sends
        const float sendDly = sd + (syL + syR) * 0.5f * synth.cfg.sendD;
        const float sendRev = srv + (syL + syR) * 0.5f * synth.cfg.sendR;

        // ping-pong delay
        const int rpL = (dlyPos - dlyLenL + dlyBuf) % dlyBuf;
        const int rpR = (dlyPos - dlyLenR + dlyBuf) % dlyBuf;
        const float tapL = dlyL[(size_t) rpL], tapR = dlyR[(size_t) rpR];
        dlyL[(size_t) dlyPos] = sendDly + tapR * 0.45f;
        dlyR[(size_t) dlyPos] = tapL * 0.45f;
        dlyPos = (dlyPos + 1) % dlyBuf;
        const float wetDlyL = tapL * duck, wetDlyR = tapR * duck;

        rvL[i] = sendRev; rvR[i] = sendRev;

        float ml = dL + syL + wetDlyL * dlyMix;
        float mr = dR + syR + wetDlyR * dlyMix;

        // master : filtre stereo (approx mono-coef), drive, glue, limiter
        float mono = (ml + mr) * 0.5f;
        const float side = (mr - ml) * 0.5f;
        mono = masterF.lowpass (mono);
        ml = mono - side; mr = mono + side;
        ml = bzzz::drive (ml, drv); mr = bzzz::drive (mr, drv);

        // glue : compression feed-forward douce
        const float lev = std::fabs (ml) > std::fabs (mr) ? std::fabs (ml) : std::fabs (mr);
        const float target = lev > .6f ? .6f / lev : 1.0f;
        comp += (target - comp) * (target < comp ? 0.008f : 0.0009f);
        ml *= comp; mr *= comp;

        L[i] = std::tanh (ml * 1.2f) * 0.9f * vol;
        R[i] = std::tanh (mr * 1.2f) * 0.9f * vol;

        const int vw = visWrite.load (std::memory_order_relaxed);
        visRing[vw] = (L[i] + R[i]) * 0.5f;
        visWrite.store ((vw + 1) & (kVisSize - 1), std::memory_order_relaxed);
    }

    // reverb sur le bus de send, puis mix
    reverb.processStereo (rvL, rvR, n);
    const float duckNow = 1.0f - scAmt * engine.kickPulse;
    for (int i = 0; i < n; ++i)
    {
        L[i] += rvL[i] * revMix * duckNow * 0.9f;
        R[i] += rvR[i] * revMix * duckNow * 0.9f;
    }

    meterKick.store (engine.kickPulse);
    midi.clear();
}

// ---------------- etat ----------------
static void writeArr (juce::ValueTree& vt, const juce::String& key, const int* a, int nvals)
{
    juce::String s2; for (int i = 0; i < nvals; ++i) { s2 << a[i]; if (i < nvals-1) s2 << ","; }
    vt.setProperty (key, s2, nullptr);
}
static void readArr (const juce::ValueTree& vt, const juce::String& key, int* a, int nvals, int def)
{
    auto s2 = vt.getProperty (key).toString();
    auto tok = juce::StringArray::fromTokens (s2, ",", "");
    for (int i = 0; i < nvals; ++i) a[i] = (i < tok.size()) ? tok[i].getIntValue() : def;
}

void BzzzProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    storeCurToSlot();
    auto state = apvts.copyState();
    state.setProperty ("curSlot", curSlot.load(), nullptr);
    state.setProperty ("style",   styleIdx.load(), nullptr);
    state.setProperty ("kit",     curKit.load(), nullptr);
    state.setProperty ("patch",   curPatch.load(), nullptr);
    state.setProperty ("synOct",  synOct.load(), nullptr);
    state.setProperty ("synPwr",  synthPower.load(), nullptr);
    state.setProperty ("songOn",  songOn.load(), nullptr);
    state.setProperty ("songLen", songLen.load(), nullptr);
    state.setProperty ("edW", edW.load(), nullptr);
    state.setProperty ("edH", edH.load(), nullptr);
    { int mm[128]; for (int i = 0; i < 128; ++i) mm[i] = midiMap[i].load();
      writeArr (state, "midiMap", mm, 128); }

    int tmp[kTracks * kSteps];
    for (int sl = 0; sl < kSlots; ++sl)
    {
        for (int tI = 0; tI < kTracks; ++tI) for (int s = 0; s < kSteps; ++s) tmp[tI*kSteps+s] = slots[sl].g[tI][s];
        writeArr (state, "slotG" + juce::String (sl), tmp, kTracks*kSteps);
        writeArr (state, "slotN" + juce::String (sl), slots[sl].n, kSteps);
        writeArr (state, "slotA" + juce::String (sl), slots[sl].a, kSteps);
        writeArr (state, "slotS" + juce::String (sl), slots[sl].s, kSteps);
    }
    int sg[kMaxSong], srp[kMaxSong];
    for (int i = 0; i < kMaxSong; ++i) { sg[i] = songSlot[i].load(); srp[i] = songReps[i].load(); }
    writeArr (state, "songSlots", sg, kMaxSong);
    writeArr (state, "songReps",  srp, kMaxSong);
    int mu[kTracks]; for (int i = 0; i < kTracks; ++i) mu[i] = trackMute[i].load();
    writeArr (state, "mutes", mu, kTracks);

    // tranche par piste + synthe : serialisation compacte
    juce::String chs;
    { const juce::ScopedLock l (cfgLock);
      for (int i = 0; i < kTracks; ++i)
      { const auto& c = chCfg[i];
        chs << (c.fon?1:0) << "," << c.ftype << "," << c.cutoff << "," << c.res << "," << c.drive << ","
            << c.crush << "," << c.decay << "," << c.pitch << "," << c.pan << ","
            << c.sendD << "," << c.sendR << "," << c.vol << ";"; }
      const auto& s2 = synCfg;
      juce::String ss;
      ss << s2.osc1 << "," << s2.osc2 << "," << s2.mix << "," << s2.detune << "," << s2.unison << ","
         << s2.osc2Pitch << "," << s2.fm << "," << s2.sub << "," << s2.noise << "," << s2.glide << ","
         << s2.fmode << "," << s2.fDec << "," << s2.drive << "," << s2.crush << "," << s2.lfoPitch << ","
         << s2.lfoShape << "," << s2.aA << "," << s2.aD << "," << s2.aS << "," << s2.aR << ","
         << s2.width << "," << s2.sendD << "," << s2.sendR;
      state.setProperty ("syn", ss, nullptr);
    }
    state.setProperty ("channels", chs, nullptr);

    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void BzzzProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;
    apvts.replaceState (state);

    styleIdx.store ((int) state.getProperty ("style", 0));
    curKit.store ((int) state.getProperty ("kit", 0));
    curPatch.store ((int) state.getProperty ("patch", 0));
    synOct.store ((int) state.getProperty ("synOct", 2));
    synthPower.store ((int) state.getProperty ("synPwr", 1));
    songOn.store ((int) state.getProperty ("songOn", 0));
    songLen.store ((int) state.getProperty ("songLen", 0));
    edW.store ((int) state.getProperty ("edW", 1400));
    edH.store ((int) state.getProperty ("edH", 940));
    { int mm[128]; readArr (state, "midiMap", mm, 128, -1);
      for (int i = 0; i < 128; ++i) midiMap[i].store (mm[i]); }

    int tmp[kTracks * kSteps];
    for (int sl = 0; sl < kSlots; ++sl)
    {
        readArr (state, "slotG" + juce::String (sl), tmp, kTracks*kSteps, 0);
        for (int tI = 0; tI < kTracks; ++tI) for (int s = 0; s < kSteps; ++s) slots[sl].g[tI][s] = tmp[tI*kSteps+s];
        readArr (state, "slotN" + juce::String (sl), slots[sl].n, kSteps, -100);
        readArr (state, "slotA" + juce::String (sl), slots[sl].a, kSteps, 0);
        readArr (state, "slotS" + juce::String (sl), slots[sl].s, kSteps, 0);
    }
    int sg[kMaxSong], srp[kMaxSong];
    readArr (state, "songSlots", sg, kMaxSong, 0);
    readArr (state, "songReps",  srp, kMaxSong, 1);
    for (int i = 0; i < kMaxSong; ++i) { songSlot[i].store (sg[i]); songReps[i].store (srp[i]); }
    int mu[kTracks]; readArr (state, "mutes", mu, kTracks, 0);
    for (int i = 0; i < kTracks; ++i) trackMute[i].store (mu[i]);

    { const juce::ScopedLock l (cfgLock);
      auto rows = juce::StringArray::fromTokens (state.getProperty ("channels").toString(), ";", "");
      for (int i = 0; i < kTracks && i < rows.size(); ++i)
      {
          auto f = juce::StringArray::fromTokens (rows[i], ",", "");
          if (f.size() >= 12)
          { auto& c = chCfg[i];
            c.fon = f[0].getIntValue() != 0; c.ftype = f[1].getIntValue();
            c.cutoff = f[2].getFloatValue(); c.res = f[3].getFloatValue();
            c.drive = f[4].getFloatValue();  c.crush = f[5].getFloatValue();
            c.decay = f[6].getFloatValue();  c.pitch = f[7].getFloatValue();
            c.pan = f[8].getFloatValue();    c.sendD = f[9].getFloatValue();
            c.sendR = f[10].getFloatValue(); c.vol = f[11].getFloatValue(); }
      }
      auto sf = juce::StringArray::fromTokens (state.getProperty ("syn").toString(), ",", "");
      if (sf.size() >= 23)
      { auto& s2 = synCfg;
        s2.osc1 = sf[0].getIntValue(); s2.osc2 = sf[1].getIntValue(); s2.mix = sf[2].getFloatValue();
        s2.detune = sf[3].getFloatValue(); s2.unison = sf[4].getIntValue(); s2.osc2Pitch = sf[5].getIntValue();
        s2.fm = sf[6].getFloatValue(); s2.sub = sf[7].getFloatValue(); s2.noise = sf[8].getFloatValue();
        s2.glide = sf[9].getFloatValue(); s2.fmode = sf[10].getIntValue(); s2.fDec = sf[11].getFloatValue();
        s2.drive = sf[12].getFloatValue(); s2.crush = sf[13].getFloatValue(); s2.lfoPitch = sf[14].getFloatValue();
        s2.lfoShape = sf[15].getIntValue(); s2.aA = sf[16].getFloatValue(); s2.aD = sf[17].getFloatValue();
        s2.aS = sf[18].getFloatValue(); s2.aR = sf[19].getFloatValue(); s2.width = sf[20].getFloatValue();
        s2.sendD = sf[21].getFloatValue(); s2.sendR = sf[22].getFloatValue(); }
    }
    gotoSlot ((int) state.getProperty ("curSlot", 0), false);
}

// ---------------- pont WebView ----------------
static int oscIdx (const juce::String& s)
{ return s == "square" ? 1 : s == "sine" ? 2 : s == "tri" ? 3 : 0; }
static int fmodeIdx (const juce::String& s)
{ static const char* n[6] = { "lp24","lp12","hp","bp","notch","formant" };
  for (int i = 0; i < 6; ++i) if (s == n[i]) return i; return 0; }
static int lfoIdx (const juce::String& s)
{ return s == "square" ? 1 : s == "saw" ? 2 : 0; }
static int ftypeIdx (const juce::String& s)
{ static const char* n[5] = { "lowpass","highpass","bandpass","notch","peaking" };
  for (int i = 0; i < 5; ++i) if (s == n[i]) return i; return 0; }
static float fProp (const juce::var& o, const char* k, float def)
{ auto p = o.getProperty (k, def); return p.isDouble() || p.isInt() || p.isInt64() ? (float)(double) p : def; }

void BzzzProcessor::applySnapshot (const juce::var& v)
{
    if (! v.isObject()) return;
    auto setP = [this] (const char* id, float val)
    { if (auto* par = apvts.getParameter (id))
      { auto& r = static_cast<juce::RangedAudioParameter*> (par)->getNormalisableRange();
        par->setValueNotifyingHost (r.convertTo0to1 (juce::jlimit (r.start, r.end, val))); } };

    // style / swing / master
    const juce::String styleName = v.getProperty ("style", "berlin").toString();
    for (int i = 0; i < 6; ++i) if (styleName == kStyleNames[i]) styleIdx.store (i);
    setP ("swing", fProp (v, "swing", 6));
    if (auto* mp = v.getProperty ("master", juce::var()).getDynamicObject())
    {
        const juce::var m (mp);
        setP ("cutoff", fProp (m, "cutoff", 16000)); setP ("res",    fProp (m, "res", .9f));
        setP ("drive",  fProp (m, "drive", .18f));   setP ("dlymix", fProp (m, "dlyMix", .14f));
        setP ("revmix", fProp (m, "rev", .16f));     setP ("sc",     fProp (m, "sc", .35f));
        setP ("volume", fProp (m, "vol", .85f));
    }

    // grille + tranche + mutes (ordre TID)
    const juce::var gr = v.getProperty ("grid", juce::var());
    const juce::var ch = v.getProperty ("ch", juce::var());
    const juce::var mu = v.getProperty ("mute", juce::var());
    {
        const juce::ScopedLock l (cfgLock);
        for (int t = 0; t < kTracks; ++t)
        {
            const juce::Identifier tid (bzzz::kTID[t]);
            if (auto* row = gr.getProperty (tid, juce::var()).getArray())
                for (int s2 = 0; s2 < kSteps && s2 < row->size(); ++s2)
                    grid[t][s2].store ((int) (*row)[s2]);
            const juce::var cc = ch.getProperty (tid, juce::var());
            if (cc.isObject())
            {
                auto& c2 = chCfg[t];
                c2.fon    = (bool) cc.getProperty ("fon", c2.fon);
                c2.ftype  = ftypeIdx (cc.getProperty ("ftype", "lowpass").toString());
                c2.cutoff = fProp (cc, "cutoff", c2.cutoff); c2.res   = fProp (cc, "res", c2.res);
                c2.drive  = fProp (cc, "drive", c2.drive);   c2.crush = fProp (cc, "crush", c2.crush);
                c2.decay  = fProp (cc, "decay", c2.decay);   c2.pitch = fProp (cc, "pitch", c2.pitch);
                c2.pan    = fProp (cc, "pan", c2.pan);       c2.sendD = fProp (cc, "sendD", c2.sendD);
                c2.sendR  = fProp (cc, "sendR", c2.sendR);   c2.vol   = fProp (cc, "vol", c2.vol);
            }
            trackMute[t].store ((bool) mu.getProperty (tid, false) ? 1 : 0);
        }

        // synthe : macros -> parametres, reste -> synCfg
        const juce::var sy = v.getProperty ("syn", juce::var());
        if (sy.isObject())
        {
            setP ("scut", fProp (sy, "cutoff", 600)); setP ("sres", fProp (sy, "res", 6));
            setP ("senv", fProp (sy, "envAmt", .75f)); setP ("slfor", fProp (sy, "lfoRate", 5));
            setP ("slfoa", fProp (sy, "lfoAmt", 0));   setP ("svol",  fProp (sy, "vol", .6f));
            auto& s2 = synCfg;
            s2.osc1 = oscIdx (sy.getProperty ("osc1", "saw").toString());
            s2.osc2 = oscIdx (sy.getProperty ("osc2", "square").toString());
            s2.mix = fProp (sy, "mix", s2.mix); s2.detune = fProp (sy, "detune", s2.detune);
            s2.unison = (int) fProp (sy, "unison", (float) s2.unison);
            s2.osc2Pitch = (int) fProp (sy, "osc2Pitch", (float) s2.osc2Pitch);
            s2.fm = fProp (sy, "fm", s2.fm); s2.sub = fProp (sy, "sub", s2.sub);
            s2.noise = fProp (sy, "noise", s2.noise); s2.glide = fProp (sy, "glide", s2.glide);
            s2.fmode = fmodeIdx (sy.getProperty ("fmode", "lp24").toString());
            s2.fDec = fProp (sy, "fDec", s2.fDec); s2.drive = fProp (sy, "drive", s2.drive);
            s2.crush = fProp (sy, "crush", s2.crush); s2.lfoPitch = fProp (sy, "lfoPitch", s2.lfoPitch);
            s2.lfoShape = lfoIdx (sy.getProperty ("lfoShape", "sine").toString());
            s2.aA = fProp (sy, "aA", s2.aA); s2.aD = fProp (sy, "aD", s2.aD);
            s2.aS = fProp (sy, "aS", s2.aS); s2.aR = fProp (sy, "aR", s2.aR);
            s2.width = fProp (sy, "width", s2.width);
            s2.sendD = fProp (sy, "sendD", s2.sendD); s2.sendR = fProp (sy, "sendR", s2.sendR);
        }
    }

    // roll synthe (null -> -100)
    auto readRoll = [&] (const char* key, std::atomic<int>* dst, int nullVal)
    { if (auto* a = v.getProperty (key, juce::var()).getArray())
        for (int s2 = 0; s2 < kSteps && s2 < a->size(); ++s2)
        { const juce::var& e = (*a)[s2];
          dst[s2].store (e.isVoid() ? nullVal : (int) e); } };
    readRoll ("synNote", rollNote, -100);
    readRoll ("synAcc", rollAcc, 0);
    readRoll ("synSlide", rollSlide, 0);
    synOct.store ((int) fProp (v, "synOct", 2));
    synthPower.store ((bool) v.getProperty ("synthPower", true) ? 1 : 0);
    curKit.store ((int) fProp (v, "curKit", 0));
    curPatch.store ((int) fProp (v, "curPat", 0));

    // patterns A-H
    if (auto* sl = v.getProperty ("slots", juce::var()).getArray())
        for (int i = 0; i < kSlots && i < sl->size(); ++i)
        {
            const juce::var& so = (*sl)[i];
            const juce::var g2 = so.getProperty ("g", juce::var());
            for (int t = 0; t < kTracks; ++t)
                if (auto* row = g2.getProperty (juce::Identifier (bzzz::kTID[t]), juce::var()).getArray())
                    for (int s2 = 0; s2 < kSteps && s2 < row->size(); ++s2)
                        slots[i].g[t][s2] = (int) (*row)[s2];
            auto rdA = [&] (const char* k, int* dst, int nullVal)
            { if (auto* a = so.getProperty (k, juce::var()).getArray())
                for (int s2 = 0; s2 < kSteps && s2 < a->size(); ++s2)
                { const juce::var& e = (*a)[s2]; dst[s2] = e.isVoid() ? nullVal : (int) e; } };
            rdA ("n", slots[i].n, -100); rdA ("a", slots[i].a, 0); rdA ("s", slots[i].s, 0);
        }
    curSlot.store ((int) fProp (v, "curSlot", 0));

    // SONG
    if (auto* sg = v.getProperty ("song", juce::var()).getArray())
    {
        const int n2 = juce::jmin ((int) sg->size(), kMaxSong);
        for (int i = 0; i < n2; ++i)
        { songSlot[i].store ((int) fProp ((*sg)[i], "slot", 0));
          songReps[i].store ((int) fProp ((*sg)[i], "reps", 1)); }
        songLen.store (n2);
    }
    else songLen.store (0);
    songOn.store ((bool) v.getProperty ("songOn", false) ? 1 : 0);
}

juce::AudioProcessorEditor* BzzzProcessor::createEditor() { return new BzzzEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()  { return new BzzzProcessor(); }
