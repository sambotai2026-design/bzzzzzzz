#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "DrumEngine.h"
#include "SynthEngine.h"
#include "Presets.h"

class BzzzProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kTracks = 12, kSteps = 16, kSlots = 8, kMaxSong = 32;

    BzzzProcessor();
    ~BzzzProcessor() override = default;

    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                  { return true; }
    const juce::String getName() const override      { return "B'Zzz Frequency"; }
    bool acceptsMidi() const override                { return true; }
    bool producesMidi() const override               { return false; }
    double getTailLengthSeconds() const override     { return 3.0; }
    int getNumPrograms() override                    { return 1; }
    int getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ================= etat partage UI <-> audio =================
    std::atomic<int> grid [kTracks][kSteps];             // 0/1/2
    std::atomic<int> rollNote [kSteps];                  // -100 = vide
    std::atomic<int> rollAcc [kSteps], rollSlide [kSteps];

    struct Slot { int g[kTracks][kSteps]; int n[kSteps]; int a[kSteps]; int s[kSteps]; };
    Slot  slots [kSlots];
    std::atomic<int> curSlot { 0 };

    std::atomic<int> songLen { 0 }, songOn { 0 }, songPos { 0 };
    std::atomic<int> songSlot [kMaxSong], songReps [kMaxSong];

    bzzz::ChannelCfg chCfg [kTracks];
    std::atomic<int> trackMute [kTracks];

    bzzz::SynCfg synCfg;
    std::atomic<int> synthPower { 1 };
    std::atomic<int> synOct { 2 };
    std::atomic<int> styleIdx { 0 };
    std::atomic<int> curKit { 0 }, curPatch { 0 };

    std::atomic<int> playStep { -1 };
    std::atomic<float> meterKick { 0 };
    std::atomic<float> hostBpm { 132.0f };

    // buffer circulaire pour le reacteur (spectre)
    static constexpr int kVisSize = 4096;
    float visRing [kVisSize] {};
    std::atomic<int> visWrite { 0 };

    // notes clavier UI -> thread audio
    void queueUiNote (int note, bool on);

    // taille de fenetre persistee
    std::atomic<int> edW { 1400 }, edH { 940 };

    // ===== MIDI LEARN universel (comme le plugin web) =====
    // cibles : 1000 + piste*16 + param (tranche drums) / 3000 + param (synthe) / 5000 + idx (macros hote)
    std::atomic<int> midiMap [128];        // cc -> cible (-1 = libre)
    std::atomic<int> learnMode { 0 };      // mode LEARN actif
    std::atomic<int> learnArmed { -1 };    // cible armee en attente d'un CC
    void armLearn (int target)             { learnArmed.store (target); }
    bool targetBound (int target) const
    { for (auto& m : midiMap) if (m.load() == target) return true; return false; }
    void clearMidiMap()
    { for (auto& m : midiMap) m.store (-1); learnArmed.store (-1); }
    void applyCC (int cc, int val);        // thread audio
    static constexpr const char* kMacroIds[14] =
    { "cutoff","res","sc","drive","dlymix","revmix","volume","swing",
      "scut","sres","senv","slfor","slfoa","svol" };

    // ===== pont WebView : etat pousse par l'interface HTML =====
    void applySnapshot (const juce::var& v);   // thread message

    // file MIDI -> interface web (paquets st<<16|d1<<8|d2)
    std::atomic<int> ccFifo [64];
    std::atomic<int> ccHead { 0 }, ccTail { 0 };
    void pushCC (int st, int d1, int d2)
    {
        const int hd = ccHead.load(); const int nx = (hd + 1) & 63;
        if (nx != ccTail.load()) { ccFifo[hd].store ((st << 16) | (d1 << 8) | d2); ccHead.store (nx); }
    }

    void loadKit (int i);
    void loadPatch (int i);
    void applyStyleSeqs (int s);
    void clearPattern();
    void randomPattern();
    void storeCurToSlot();
    void gotoSlot (int i, bool store = true);
    void copySlotTo (int dest);
    void previewPad (int track);
    juce::CriticalSection cfgLock;
    juce::SpinLock uiNoteLock;
    std::vector<std::pair<int,bool>> uiNotes;

    juce::AudioProcessorValueTreeState apvts;

private:
    bzzz::Engine engine;
    bzzz::SynthEngine synth;
    bzzz::SVF masterF;
    juce::Reverb reverb;

    std::vector<float> dlyL, dlyR;
    int dlyPos = 0, dlyLenL = 12000, dlyLenR = 18000, dlyBuf = 0;

    double sr = 48000.0;
    int  lastStep = -1;
    bool wasPlaying = false;
    int  songBar = 0;
    float comp = 1.0f;

    std::atomic<float>* pCut{}, *pRes{}, *pDrv{}, *pVol{}, *pSwing{},
                      * pDlyMix{}, *pRev{}, *pSC{},
                      * pSCut{}, *pSRes{}, *pSEnv{}, *pSLfoR{}, *pSLfoA{}, *pSVol{};

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void fireStep (int stepMod);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BzzzProcessor)
};
