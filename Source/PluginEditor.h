#pragma once
#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class PsyKickBassProEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PsyKickBassProEditor(PsyKickBassProProcessor&);
    ~PsyKickBassProEditor() override = default;
    void resized() override;
private:
    void timerCallback() override;
    PsyKickBassProProcessor& processor;
    std::unique_ptr<juce::WebBrowserComponent> webView;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PsyKickBassProEditor)
};
