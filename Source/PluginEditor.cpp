#include "PluginEditor.h"
#include "BinaryData.h"

namespace {
class BridgeWebView : public juce::WebBrowserComponent {
public:
    BridgeWebView(const Options& opts, std::function<void(const juce::String&)> cb)
        : juce::WebBrowserComponent(opts), onBridge(std::move(cb)) {}
    bool pageAboutToLoad(const juce::String& newURL) override {
        if (newURL.startsWith("https://juce.bridge/")) {
            if (onBridge) onBridge(newURL);
            return false;
        }
        return true;
    }
private:
    std::function<void(const juce::String&)> onBridge;
};
}

PsyKickBassProEditor::PsyKickBassProEditor(PsyKickBassProProcessor& p)
    : AudioProcessorEditor(&p), processor(p) {
    auto opts = juce::WebBrowserComponent::Options{}
        .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options(
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory))
        );
    webView = std::make_unique<BridgeWebView>(opts, [this](const juce::String& url) {
        auto jsonStr = juce::URL::removeEscapeChars(url.fromFirstOccurrenceOf("data=", false, false));
        processor.handleBridgeCommand(jsonStr);
    });
    addAndMakeVisible(*webView);

    auto html = juce::String::fromUTF8(BinaryData::ui_html, BinaryData::ui_htmlSize);
    auto tempHtml = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("psy_kick_bass_pro_v6_ui.html");
    tempHtml.replaceWithText(html);
    webView->goToURL(juce::URL(tempHtml).toString(false));

    setResizable(true, true);
    setSize(1060, 620);
    startTimerHz(25);
}

void PsyKickBassProEditor::resized() {
    if (webView) webView->setBounds(getLocalBounds());
}

void PsyKickBassProEditor::timerCallback() {
    if (webView) {
        auto telem = processor.getTelemetryJson();
        webView->evaluateJavascript("if(window.updateTelemetry) window.updateTelemetry(" + telem + ");");
    }
}
