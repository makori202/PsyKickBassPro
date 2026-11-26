#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

void PsyKickBassProProcessor::Biquad::setLowpass(double sr, float fc, float q) {
    float w0 = 2.0f * 3.14159265f * juce::jlimit(20.0f, (float)(sr * 0.45), fc) / (float)sr;
    float cs = std::cos(w0), sn = std::sin(w0);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = ((1.0f - cs) * 0.5f) / a0;
    b1 = (1.0f - cs) / a0;
    b2 = b0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

void PsyKickBassProProcessor::Biquad::setHighpass(double sr, float fc, float q) {
    float w0 = 2.0f * 3.14159265f * juce::jlimit(20.0f, (float)(sr * 0.45), fc) / (float)sr;
    float cs = std::cos(w0), sn = std::sin(w0);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = ((1.0f + cs) * 0.5f) / a0;
    b1 = -(1.0f + cs) / a0;
    b2 = b0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

PsyKickBassProProcessor::PsyKickBassProProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)) {
    for (int ch = 0; ch < 2; ++ch)
        delayBuf[ch].assign(kDelayBufSize, 0.0f);
}

bool PsyKickBassProProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.inputBuses.size() > 1) {
        auto sc = layouts.inputBuses[1];
        if (!sc.isDisabled() && sc != juce::AudioChannelSet::stereo() && sc != juce::AudioChannelSet::mono())
            return false;
    }
    return true;
}

void PsyKickBassProProcessor::prepareToPlay(double sampleRate, int) {
    currentSampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
    for (int ch = 0; ch < 2; ++ch) {
        lp1[ch].reset(); lp2[ch].reset(); hp1[ch].reset(); hp2[ch].reset();
        monoLp1[ch].reset(); monoLp2[ch].reset(); monoHp1[ch].reset(); monoHp2[ch].reset();
        apPhase[ch].reset();
        std::fill(delayBuf[ch].begin(), delayBuf[ch].end(), 0.0f);
    }
    delayWriteIdx = 0;
    scEnv = 0.0f;
    holdCounter = 0;
    updateFilterCoeffs();
}

void PsyKickBassProProcessor::releaseResources() {}

void PsyKickBassProProcessor::updateFilterCoeffs() {
    float xo = xoverHz.load();
    float ms = monoSubHz.load();
    float deg = phaseDeg.load();
    // Map |deg| (0..180) to all-pass center frequency 18..420 Hz for smooth 65Hz sub rotation
    float normDeg = std::abs(deg) / 180.0f;
    float apFc = 18.0f * std::pow(24.0f, normDeg); // 18Hz -> 432Hz

    for (int ch = 0; ch < 2; ++ch) {
        lp1[ch].setLowpass(currentSampleRate, xo);
        lp2[ch].setLowpass(currentSampleRate, xo);
        hp1[ch].setHighpass(currentSampleRate, xo);
        hp2[ch].setHighpass(currentSampleRate, xo);

        monoLp1[ch].setLowpass(currentSampleRate, ms);
        monoLp2[ch].setLowpass(currentSampleRate, ms);
        monoHp1[ch].setHighpass(currentSampleRate, ms);
        monoHp2[ch].setHighpass(currentSampleRate, ms);

        apPhase[ch].setFreq(currentSampleRate, apFc);
    }
}

void PsyKickBassProProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    updateFilterCoeffs();

    auto mainBus = getBusBuffer(buffer, true, 0);
    int numSamples = mainBus.getNumSamples();
    int numCh = juce::jmin(2, mainBus.getNumChannels());
    if (numSamples == 0 || numCh == 0) return;

    bool hasScBus = (getBusCount(true) > 1);
    juce::AudioBuffer<float> scBus;
    if (hasScBus) scBus = getBusBuffer(buffer, true, 1);
    int scCh = hasScBus ? scBus.getNumChannels() : 0;

    float scPeak = 0.0f;
    if (scCh > 0) {
        for (int c = 0; c < scCh; ++c)
            scPeak = juce::jmax(scPeak, scBus.getMagnitude(c, 0, numSamples));
    }
    float bassPeak = 0.0f;
    for (int c = 0; c < numCh; ++c)
        bassPeak = juce::jmax(bassPeak, mainBus.getMagnitude(c, 0, numSamples));

    scSignalPresent.store(scPeak > 0.0008f);
    bassSignalPresent.store(bassPeak > 0.0008f);

    int mode = scMode.load();
    bool useBpm = (mode == 2) || (mode == 0 && scPeak <= 0.0008f);

    double bpm = 142.0;
    double ppqStart = 0.0;
    bool isPlaying = false;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto p = pos->getPpqPosition()) ppqStart = *p;
            isPlaying = pos->getIsPlaying();
        }
    }

    float subDuck = subDuckPct.load() * 0.01f;
    float topDuck = topDuckPct.load() * 0.01f;
    float threshLin = juce::Decibels::decibelsToGain(scThreshDb.load());
    int holdSamp = (int)(holdMs.load() * 0.001f * currentSampleRate);
    float relCoeff = std::exp(-1.0f / juce::jmax(1.0f, releaseMs.load() * 0.001f * (float)currentSampleRate));
    float attCoeff = std::exp(-1.0f / juce::jmax(1.0f, 0.0015f * (float)currentSampleRate));

    float delaySampF = juce::jlimit(0.0f, 15.0f, microDelayMs.load()) * 0.001f * (float)currentSampleRate;
    int dInt = (int)delaySampF;
    float dFrac = delaySampF - (float)dInt;

    bool inv = invert180.load();
    float pSign = (phaseDeg.load() < 0.0f) ? -1.0f : 1.0f;
    bool useAp = std::abs(phaseDeg.load()) > 2.0f;

    float maxGr = 0.0f;
    double dotSum = 0.0, eK = 0.0, eB = 0.0;

    float* outL = mainBus.getWritePointer(0);
    float* outR = (numCh > 1) ? mainBus.getWritePointer(1) : outL;
    const float* scL = (scCh > 0) ? scBus.getReadPointer(0) : nullptr;
    const float* scR = (scCh > 1) ? scBus.getReadPointer(1) : scL;

    double beatsPerSample = (bpm / 60.0) / currentSampleRate;

    const juce::SpinLock::ScopedTryLockType lock(dataLock);

    for (int i = 0; i < numSamples; ++i) {
        float kMono = scL ? (0.5f * (scL[i] + (scR ? scR[i] : scL[i]))) : 0.0f;
        float bInL = outL[i];
        float bInR = outR[i];
        float bDryMono = 0.5f * (bInL + bInR);

        // 1. Sidechain trigger (Real Audio or BPM Groove)
        float trig = 0.0f;
        if (!useBpm && scL != nullptr) {
            float absK = std::abs(kMono);
            if (absK > threshLin) {
                float norm = juce::jlimit(0.0f, 1.0f, (absK - threshLin) / juce::jmax(0.05f, 1.0f - threshLin));
                trig = juce::jmax(0.85f, norm);
                holdCounter = holdSamp;
            }
        } else if (useBpm && isPlaying) {
            double beatPos = std::fmod(ppqStart + (double)i * beatsPerSample, 1.0);
            if (beatPos < 0.0) beatPos += 1.0;
            double beatMs = beatPos * (60000.0 / bpm);
            if (beatMs < (double)holdMs.load()) trig = 1.0f;
        }

        if (trig > scEnv) {
            scEnv = scEnv * attCoeff + trig * (1.0f - attCoeff);
        } else if (holdCounter > 0) {
            --holdCounter;
        } else {
            scEnv *= relCoeff;
        }

        // 2. Micro-delay + Phase Rotation + Polarity Invert on Bass
        float chans[2] = { bInL, bInR };
        for (int c = 0; c < 2; ++c) {
            delayBuf[c][delayWriteIdx] = chans[c];
            int idx0 = (delayWriteIdx - dInt + kDelayBufSize) % kDelayBufSize;
            int idx1 = (idx0 - 1 + kDelayBufSize) % kDelayBufSize;
            float delayed = delayBuf[c][idx0] * (1.0f - dFrac) + delayBuf[c][idx1] * dFrac;
            if (useAp) delayed = apPhase[c].process(delayed) * (pSign < 0.0f ? -1.0f : 1.0f);
            if (inv) delayed = -delayed;
            chans[c] = delayed;
        }
        delayWriteIdx = (delayWriteIdx + 1) % kDelayBufSize;

        // 3. LR4 Crossover & Independent Sub/Top Sidechain Ducking
        float subL = lp2[0].process(lp1[0].process(chans[0]));
        float subR = lp2[1].process(lp1[1].process(chans[1]));
        float topL = hp2[0].process(hp1[0].process(chans[0]));
        float topR = hp2[1].process(hp1[1].process(chans[1]));

        float gSub = 1.0f - scEnv * subDuck;
        float gTop = 1.0f - scEnv * topDuck;
        maxGr = juce::jmax(maxGr, 1.0f - gSub);

        float procL = subL * gSub + topL * gTop;
        float procR = subR * gSub + topR * gTop;

        // 4. Mono Sub (< monoSubHz)
        float mSubL = monoLp2[0].process(monoLp1[0].process(procL));
        float mSubR = monoLp2[1].process(monoLp1[1].process(procR));
        float mTopL = monoHp2[0].process(monoHp1[0].process(procL));
        float mTopR = monoHp2[1].process(monoHp1[1].process(procR));
        float mSubMono = 0.5f * (mSubL + mSubR);

        outL[i] = mSubMono + mTopL;
        if (numCh > 1) outR[i] = mSubMono + mTopR;

        float bProcMono = 0.5f * (outL[i] + (numCh > 1 ? outR[i] : outL[i]));
        dotSum += (double)kMono * (double)bProcMono;
        eK += (double)kMono * (double)kMono;
        eB += (double)bProcMono * (double)bProcMono;

        if (lock.isLocked()) {
            capKick[capWriteIdx] = kMono;
            capBass[capWriteIdx] = bDryMono;
            capWriteIdx = (capWriteIdx + 1) % kCaptureSize;

            scopeKick[scopeWriteIdx] = kMono;
            scopeBass[scopeWriteIdx] = bProcMono;
            scopeSum[scopeWriteIdx] = kMono + bProcMono;
            scopeWriteIdx = (scopeWriteIdx + 1) % kScopeSize;
        }
    }

    if (eK > 1e-7 && eB > 1e-7) {
        float c = (float)(dotSum / std::sqrt(eK * eB));
        float prev = liveCorr.load();
        liveCorr.store(prev * 0.7f + c * 0.3f);
    }
    liveGrDb.store(juce::Decibels::gainToDecibels(juce::jmax(0.01f, 1.0f - maxGr)));
}

void PsyKickBassProProcessor::runAutoPhaseLock() {
    std::array<float, kCaptureSize> kBuf, bBuf;
    {
        const juce::SpinLock::ScopedLockType lock(dataLock);
        for (int i = 0; i < kCaptureSize; ++i) {
            int idx = (capWriteIdx + i) % kCaptureSize;
            kBuf[i] = capKick[idx];
            bBuf[i] = capBass[idx];
        }
    }

    // Lowpass both < 120Hz to optimize sub-bass phase alignment
    Biquad klp1, klp2, blp1, blp2;
    klp1.setLowpass(currentSampleRate, 120.0f);
    klp2.setLowpass(currentSampleRate, 120.0f);
    blp1.setLowpass(currentSampleRate, 120.0f);
    blp2.setLowpass(currentSampleRate, 120.0f);

    double kE = 0.0, bE = 0.0;
    for (int i = 0; i < kCaptureSize; ++i) {
        kBuf[i] = klp2.process(klp1.process(kBuf[i]));
        bBuf[i] = blp2.process(blp1.process(bBuf[i]));
        kE += kBuf[i] * kBuf[i];
        bE += bBuf[i] * bBuf[i];
    }

    if (kE < 1e-5 || bE < 1e-5) {
        autoLockStatus = "Play Kick (Sidechain) + Bass in Ableton first!";
        syncUiFlag.store(true);
        return;
    }

    double bestScore = -1e9;
    bool bestInv = false;
    float bestPhase = 0.0f;
    float bestDelay = 0.0f;

    // Search over Invert, Phase Angle (-150..+150 step 30), Micro-Delay (0..8ms step 0.5ms)
    for (int invIdx = 0; invIdx < 2; ++invIdx) {
        bool candInv = (invIdx == 1);
        for (int pDeg = -150; pDeg <= 150; pDeg += 30) {
            AllPass1 ap;
            float normDeg = std::abs((float)pDeg) / 180.0f;
            float apFc = 18.0f * std::pow(24.0f, normDeg);
            ap.setFreq(currentSampleRate, apFc);
            bool useAp = std::abs(pDeg) > 2;
            float pSign = (pDeg < 0) ? -1.0f : 1.0f;

            std::vector<float> rotB(kCaptureSize);
            for (int i = 0; i < kCaptureSize; ++i) {
                float v = bBuf[i];
                if (useAp) v = ap.process(v) * pSign;
                if (candInv) v = -v;
                rotB[i] = v;
            }

            for (int dStep = 0; dStep <= 16; ++dStep) {
                float dMs = dStep * 0.5f;
                int dSamp = (int)std::round(dMs * 0.001f * currentSampleRate);
                double dot = 0.0, eCand = 0.0;
                for (int i = dSamp; i < kCaptureSize; ++i) {
                    float bv = rotB[i - dSamp];
                    dot += (double)kBuf[i] * (double)bv;
                    eCand += (double)bv * (double)bv;
                }
                double corr = dot / (std::sqrt(kE * eCand) + 1e-9);
                if (corr > bestScore) {
                    bestScore = corr;
                    bestInv = candInv;
                    bestPhase = (float)pDeg;
                    bestDelay = dMs;
                }
            }
        }
    }

    invert180.store(bestInv);
    phaseDeg.store(bestPhase);
    microDelayMs.store(bestDelay);
    autoLockStatus = "LOCKED! Sub Corr: " + juce::String(bestScore, 2)
        + " | Phase: " + juce::String((int)bestPhase) + "deg"
        + " | Delay: " + juce::String(bestDelay, 1) + "ms"
        + (bestInv ? " | 180 INV" : "");
    syncUiFlag.store(true);
}

void PsyKickBassProProcessor::handleBridgeCommand(const juce::String& jsonStr) {
    auto var = juce::JSON::parse(jsonStr);
    if (!var.isObject()) return;
    auto* obj = var.getDynamicObject();
    if (!obj) return;

    if (obj->hasProperty("cmd") && obj->getProperty("cmd").toString() == "auto_lock") {
        runAutoPhaseLock();
        return;
    }

    if (obj->hasProperty("bassStyle")) bassStyle.store((int)obj->getProperty("bassStyle"));
    if (obj->hasProperty("scMode")) scMode.store((int)obj->getProperty("scMode"));
    if (obj->hasProperty("subDuck")) subDuckPct.store((float)obj->getProperty("subDuck"));
    if (obj->hasProperty("topDuck")) topDuckPct.store((float)obj->getProperty("topDuck"));
    if (obj->hasProperty("holdMs")) holdMs.store((float)obj->getProperty("holdMs"));
    if (obj->hasProperty("releaseMs")) releaseMs.store((float)obj->getProperty("releaseMs"));
    if (obj->hasProperty("xoverHz")) xoverHz.store((float)obj->getProperty("xoverHz"));
    if (obj->hasProperty("monoSubHz")) monoSubHz.store((float)obj->getProperty("monoSubHz"));
    if (obj->hasProperty("phaseDeg")) phaseDeg.store((float)obj->getProperty("phaseDeg"));
    if (obj->hasProperty("microDelayMs")) microDelayMs.store((float)obj->getProperty("microDelayMs"));
    if (obj->hasProperty("invert180")) invert180.store((bool)obj->getProperty("invert180"));
    if (obj->hasProperty("scThreshDb")) scThreshDb.store((float)obj->getProperty("scThreshDb"));
}

juce::String PsyKickBassProProcessor::getTelemetryJson() {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("corr", liveCorr.load());
    root->setProperty("grDb", liveGrDb.load());
    root->setProperty("scActive", scSignalPresent.load());
    root->setProperty("bassActive", bassSignalPresent.load());
    root->setProperty("lockMsg", autoLockStatus);

    bool doSync = syncUiFlag.exchange(false);
    root->setProperty("syncParams", doSync);
    if (doSync) {
        root->setProperty("phaseDeg", phaseDeg.load());
        root->setProperty("microDelayMs", microDelayMs.load());
        root->setProperty("invert180", invert180.load());
    }

    juce::Array<juce::var> kArr, bArr, sArr;
    {
        const juce::SpinLock::ScopedLockType lock(dataLock);
        const int pts = 80;
        const int step = kScopeSize / pts;
        for (int i = 0; i < pts; ++i) {
            int idx = (scopeWriteIdx + i * step) % kScopeSize;
            kArr.add(scopeKick[idx]);
            bArr.add(scopeBass[idx]);
            sArr.add(scopeSum[idx]);
        }
    }
    root->setProperty("kick", kArr);
    root->setProperty("bass", bArr);
    root->setProperty("sum", sArr);

    return juce::JSON::toString(juce::var(root.get()), true);
}

void PsyKickBassProProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("bassStyle", bassStyle.load());
    obj->setProperty("scMode", scMode.load());
    obj->setProperty("subDuck", subDuckPct.load());
    obj->setProperty("topDuck", topDuckPct.load());
    obj->setProperty("holdMs", holdMs.load());
    obj->setProperty("releaseMs", releaseMs.load());
    obj->setProperty("xoverHz", xoverHz.load());
    obj->setProperty("monoSubHz", monoSubHz.load());
    obj->setProperty("phaseDeg", phaseDeg.load());
    obj->setProperty("microDelayMs", microDelayMs.load());
    obj->setProperty("invert180", invert180.load());
    obj->setProperty("scThreshDb", scThreshDb.load());
    auto str = juce::JSON::toString(juce::var(obj.get()));
    juce::MemoryOutputStream mos(destData, true);
    mos.writeString(str);
}

void PsyKickBassProProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto str = juce::String::createStringFromData(data, sizeInBytes);
    handleBridgeCommand(str);
    syncUiFlag.store(true);
}

juce::AudioProcessorEditor* PsyKickBassProProcessor::createEditor() {
    return new PsyKickBassProEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PsyKickBassProProcessor();
}
