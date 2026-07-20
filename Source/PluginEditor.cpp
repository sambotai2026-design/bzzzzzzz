#include "PluginEditor.h"
#include "BinaryData.h"

BzzzEditor::BzzzEditor (BzzzProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    auto dataDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("BZzzFrequency");
    dataDir.createDirectory();

    juce::WebBrowserComponent::Options opt;
    opt = opt.withNativeIntegrationEnabled()
             .withKeepPageLoadedWhenBrowserIsHidden()
             .withResourceProvider ([] (const juce::String& url)
                 -> std::optional<juce::WebBrowserComponent::Resource>
             {
                 if (url == "/" || url == "/index.html")
                 {
                     std::vector<std::byte> data ((size_t) BinaryData::index_htmlSize);
                     std::memcpy (data.data(), BinaryData::index_html, (size_t) BinaryData::index_htmlSize);
                     return juce::WebBrowserComponent::Resource { std::move (data), "text/html" };
                 }
                 return std::nullopt;
             })
             .withEventListener ("sync", [this] (juce::var v) { proc.applySnapshot (v); })
             .withEventListener ("pad", [this] (juce::var v)
                 { const int t = (int) v.getProperty ("t", -1);
                   if (t >= 0 && t < 12) proc.previewPad (t); })
             .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                          .withUserDataFolder (dataDir));

    web = std::make_unique<juce::WebBrowserComponent> (opt);
    addAndMakeVisible (*web);
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (700, 480, 3000, 2200);
    setSize (juce::jlimit (700, 3000, proc.edW.load()), juce::jlimit (480, 2200, proc.edH.load()));
    startTimerHz (30);
}

void BzzzEditor::timerCallback()
{
    // --- spectre 96 bandes depuis le buffer du processeur (pour le reacteur) ---
    float buf[1024];
    const int wp = proc.visWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < 1024; ++i)
        buf[i] = proc.visRing[(wp - 1024 + i + BzzzProcessor::kVisSize) & (BzzzProcessor::kVisSize - 1)];
    juce::Array<juce::var> spec;
    spec.ensureStorageAllocated (96);
    for (int b = 0; b < 96; ++b)
    {
        const float f = 40.f * std::pow (11000.f / 40.f, b / 95.f);
        const float w = juce::MathConstants<float>::twoPi * f / 48000.f;
        const float cw = 2.f * std::cos (w);
        float s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < 1024; i += 2) { s0 = buf[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
        float mag = std::sqrt (juce::jmax (0.f, s1*s1 + s2*s2 - cw*s1*s2)) / 200.f;
        mag = juce::jlimit (0.f, 1.f, mag);
        bins[b] += (mag - bins[b]) * (mag > bins[b] ? .6f : .25f);
        spec.add ((int) (bins[b] * 255.f));
    }

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("step", proc.playStep.load());
    obj->setProperty ("kick", proc.meterKick.load());
    obj->setProperty ("bpm",  proc.hostBpm.load());
    obj->setProperty ("spec", juce::var (spec));
    web->emitEventIfBrowserIsVisible ("rt", juce::var (obj));

    // --- MIDI de l'hote vers le MIDI LEARN de la page ---
    int tl = proc.ccTail.load();
    while (tl != proc.ccHead.load())
    {
        const int pk = proc.ccFifo[tl].load();
        tl = (tl + 1) & 63;
        proc.ccTail.store (tl);
        auto* m = new juce::DynamicObject();
        m->setProperty ("d0", (pk >> 16) & 0xFF);
        m->setProperty ("d1", (pk >> 8) & 0xFF);
        m->setProperty ("d2", pk & 0xFF);
        web->emitEventIfBrowserIsVisible ("midi", juce::var (m));
    }
}

void BzzzEditor::resized()
{
    web->setBounds (getLocalBounds());
    proc.edW.store (getWidth());
    proc.edH.store (getHeight());
}
