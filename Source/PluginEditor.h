#pragma once
#include "PluginProcessor.h"

// L'interface du plugin EST le fichier HTML original, rendu dans une WebView.
// Le C++ joue le son ; la page pousse son etat (sync) et recoit le temps reel (rt) + le MIDI.
class BzzzEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BzzzEditor (BzzzProcessor&);
    ~BzzzEditor() override = default;
    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff050508)); }

private:
    void timerCallback() override;
    BzzzProcessor& proc;
    std::unique_ptr<juce::WebBrowserComponent> web;
    float bins [96] {};
    int lastStepSent = -2;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BzzzEditor)
};
