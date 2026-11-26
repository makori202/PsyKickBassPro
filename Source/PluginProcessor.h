#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>

class PsyKickBassProProcessor : public juce::AudioProcessor {
public:
    PsyKickBassProProcessor();
    ~PsyKickBassProProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PsyKickBassPro"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void handleBridgeCommand(const juce::String& jsonStr);
    juce::String getTelemetryJson();

private:
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void reset() { z1 = z2 = 0.0f; }
        float process(float x) {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void setLowpass(double sr, float fc, float q = 0.70710678f);
        void setHighpass(double sr, float fc, float q = 0.70710678f);
    };

    struct AllPass1 {
        float a = 0.0f;
        float x1 = 0.0f, y1 = 0.0f;
        void reset() { x1 = y1 = 0.0f; }
        void setFreq(double sr, float fc) {
            float t = std::tan(3.14159265f * juce::jlimit(10.0f, 1500.0f, fc) / (float)sr);
            a = (t - 1.0f) / (t + 1.0f);
        }
        float process(float x) {
            float y = a * x + x1 - a * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    void updateFilterCoeffs();
    void runAutoPhaseLock();

    double currentSampleRate = 48000.0;

    // Parameters
    std::atomic<int> bassStyle{0};       // 0:KBBB, 1:Offbeat, 2:Gallop, 3:Triplet, 4:SubReese
    std::atomic<int> scMode{0};          // 0:Auto (SC+BPM fallback), 1:Sidechain Only, 2:BPM Groove Only
    std::atomic<float> subDuckPct{92.0f};
    std::atomic<float> topDuckPct{40.0f};
    std::atomic<float> holdMs{18.0f};
    std::atomic<float> releaseMs{58.0f};
    std::atomic<float> xoverHz{140.0f};
    std::atomic<float> monoSubHz{120.0f};
    std::atomic<float> phaseDeg{0.0f};   // -180..180
    std::atomic<float> microDelayMs{0.0f}; // 0..12
    std::atomic<bool> invert180{false};
    std::atomic<float> scThreshDb{-28.0f};

    // LR4 Crossover (2 cascaded 2nd-order per band per channel)
    Biquad lp1[2], lp2[2], hp1[2], hp2[2];
    Biquad monoLp1[2], monoLp2[2], monoHp1[2], monoHp2[2];
    AllPass1 apPhase[2];

    // Micro-delay line (up to 20ms at 192kHz -> 4096 samples)
    static constexpr int kDelayBufSize = 4096;
    std::array<std::vector<float>, 2> delayBuf;
    int delayWriteIdx = 0;

    // Sidechain envelope state
    float scEnv = 0.0f;
    int holdCounter = 0;
    double lastPpq = -1.0;

    // Telemetry & Capture buffers
    static constexpr int kCaptureSize = 16384;
    static constexpr int kScopeSize = 1024;
    std::array<float, kCaptureSize> capKick{};
    std::array<float, kCaptureSize> capBass{};
    int capWriteIdx = 0;

    std::array<float, kScopeSize> scopeKick{};
    std::array<float, kScopeSize> scopeBass{};
    std::array<float, kScopeSize> scopeSum{};
    int scopeWriteIdx = 0;

    juce::SpinLock dataLock;
    std::atomic<float> liveCorr{0.0f};
    std::atomic<float> liveGrDb{0.0f};
    std::atomic<bool> scSignalPresent{false};
    std::atomic<bool> bassSignalPresent{false};
    std::atomic<bool> syncUiFlag{false};
    juce::String autoLockStatus{"Ready — route Kick to Sidechain or use BPM Groove."};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PsyKickBassProProcessor)
};
