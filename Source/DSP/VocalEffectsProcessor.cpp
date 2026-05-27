#include "VocalEffectsProcessor.h"
#include <cmath>
#include <algorithm>

VocalEffectsProcessor::VocalEffectsProcessor() {}
VocalEffectsProcessor::~VocalEffectsProcessor() {}

void VocalEffectsProcessor::prepare(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    numChans = kMaxChans;
    blockSize = samplesPerBlock;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = 1;

    auto crossoverCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 3500.0f);
    auto harmonicCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 4000.0f);

    for (int c = 0; c < numChans; ++c) {
        sibilantsFilter[c].prepare(spec);
        sibilantsFilter[c].reset();

        exciterCrossover[c].prepare(spec);
        *exciterCrossover[c].coefficients = *crossoverCoeffs;
        exciterCrossover[c].reset();

        exciterHarmonicFilter[c].prepare(spec);
        *exciterHarmonicFilter[c].coefficients = *harmonicCoeffs;
        exciterHarmonicFilter[c].reset();

        popLow[c].prepare(spec);
        popHigh[c].prepare(spec);
        popLow[c].setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
        popHigh[c].setType(juce::dsp::LinkwitzRileyFilterType::highpass);
        popLow[c].setCutoffFrequency(kPopCrossoverHz);
        popHigh[c].setCutoffFrequency(kPopCrossoverHz);
        popLow[c].reset();
        popHigh[c].reset();
    }

    currentSibilantsDb = 9999.0f;
    setToneShaping(0.0f, 0.0f);

    popFastAlpha    = 1.0f - std::exp(-1.0f / (0.003f * (float)currentSampleRate));
    popSlowAlpha    = 1.0f - std::exp(-1.0f / (0.150f * (float)currentSampleRate));
    popAttackAlpha  = 1.0f - std::exp(-1.0f / (0.005f * (float)currentSampleRate));
    popReleaseAlpha = 1.0f - std::exp(-1.0f / (0.080f * (float)currentSampleRate));
    popFastEnv = popSlowEnv = 0.0f;
    popGain = 1.0f;
    popThresholdDb = 0.0f;
    popActivity.store(0.0f);

    popBassTemp.setSize(numChans, samplesPerBlock + 16, false, true, true);
    popHighTemp.setSize(numChans, samplesPerBlock + 16, false, true, true);
    popBassTemp.clear();
    popHighTemp.clear();

    float breathAttackMs = 10.0f;
    float breathReleaseMs = 40.0f;
    breathAttackAlpha = 1.0f - std::exp(-1.0f / (breathAttackMs * 0.001f * currentSampleRate));
    breathReleaseAlpha = 1.0f - std::exp(-1.0f / (breathReleaseMs * 0.001f * currentSampleRate));
    breathGain = 1.0f;
    breathThresholdDb = 0.0f;
    isBreathActive = false;
    breathActivity.store(0.0f);

    breathInputRms.assign(256, 0.0f);
    breathGateDelay.assign(256, false);
    breathBlockIndex = 0;
}

void VocalEffectsProcessor::reset()
{
    for (int c = 0; c < numChans; ++c) {
        sibilantsFilter[c].reset();
        exciterCrossover[c].reset();
        exciterHarmonicFilter[c].reset();
        popLow[c].reset();
        popHigh[c].reset();
    }
    popFastEnv = popSlowEnv = 0.0f;
    popGain = 1.0f;
    popActivity.store(0.0f);

    breathGain = 1.0f;
    isBreathActive = false;
    breathActivity.store(0.0f);
    breathInputRms.assign(256, 0.0f);
    breathGateDelay.assign(256, false);
    breathBlockIndex = 0;
}

void VocalEffectsProcessor::setToneShaping(float sibilantsDb, float breathDb)
{
    juce::ignoreUnused(breathDb);
    if (std::abs(sibilantsDb - currentSibilantsDb) > 0.01f) {
        currentSibilantsDb = sibilantsDb;
        float gain = std::pow(10.0f, sibilantsDb / 20.0f);
        sibilantsCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            currentSampleRate, kSibilantsHz, 0.7071f, gain);
        for (int c = 0; c < numChans; ++c) {
            *sibilantsFilter[c].coefficients = *sibilantsCoeffs;
        }
    }
}

void VocalEffectsProcessor::setExciter(float exciterDb, bool isConsonant)
{
    if (exciterDb > 6.0f) exciterDb = 6.0f;
    if (exciterDb < 0.0f) exciterDb = 0.0f;
    this->exciterDb = exciterDb;
    this->isConsonantActive = isConsonant;
}

void VocalEffectsProcessor::setPopFilter(float thresholdDb)
{
    if (thresholdDb > 0.0f)   thresholdDb = 0.0f;
    if (thresholdDb < -36.0f) thresholdDb = -36.0f;
    popThresholdDb = thresholdDb;
}

void VocalEffectsProcessor::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    if (thresholdDb > 0.0f)   thresholdDb = 0.0f;
    if (thresholdDb < -48.0f) thresholdDb = -48.0f;
    breathThresholdDb = thresholdDb;
    isBreathActive = isBreathDetected;
}

void VocalEffectsProcessor::processPrePitch(juce::AudioBuffer<float>& buffer, int engineLatencySamples)
{
    int numSamples = buffer.getNumSamples();
    int hostChans = buffer.getNumChannels();
    int procChans = juce::jmin(numChans, hostChans > 0 ? hostChans : 1);

    auto* L = buffer.getWritePointer(0);
    auto* R = (hostChans > 1) ? buffer.getWritePointer(1) : L;

    // ----- 0) Smart breath gate - Dry input RMS calculation -----
    int N = breathBlockIndex;
    breathBlockIndex = (breathBlockIndex + 1) % 256;

    float rms = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float sumSq = 0.0f;
        for (int c = 0; c < procChans; ++c) {
            float s = (c == 0) ? L[i] : R[i];
            sumSq += s * s;
        }
        rms += sumSq / (float)procChans;
    }
    rms = std::sqrt(rms / (float)numSamples);
    breathInputRms[(size_t)N] = rms;

    int delayBlocks = (blockSize > 0) ? (int)std::round((float)engineLatencySamples / (float)blockSize) : 0;
    float blockDuration = (float)numSamples / (float)juce::jmax(1.0, currentSampleRate);
    int minBreathBlocks = (int)std::round(0.138f / juce::jmax(0.0001f, blockDuration));
    if (minBreathBlocks < 2) minBreathBlocks = 2;

    if (breathThresholdDb < -0.01f) {
        if (isBreathActive) {
            float threshLin = std::pow(10.0f, breathThresholdDb / 20.0f);
            for (int b = N - minBreathBlocks + 1; b <= N; ++b) {
                int idx = (b + 512) % 256;
                if (breathInputRms[(size_t)idx] > threshLin) {
                    breathGateDelay[(size_t)((b + delayBlocks + 512) % 256)] = true;
                }
            }
        } else {
            breathGateDelay[(size_t)((N + delayBlocks + 256) % 256)] = false;
        }
    }

    // ----- 0b) Adaptive pop filter -----
    if (popThresholdDb < -0.01f) {
        if (popBassTemp.getNumSamples() < numSamples) {
            popBassTemp.setSize(numChans, numSamples + 16, false, true, true);
            popHighTemp.setSize(numChans, numSamples + 16, false, true, true);
        }
        for (int c = 0; c < procChans; ++c) {
            const float* src = (c == 0) ? L : R;
            std::copy_n(src, numSamples, popBassTemp.getWritePointer(c));
            std::copy_n(src, numSamples, popHighTemp.getWritePointer(c));
            float* bp = popBassTemp.getWritePointer(c);
            float* hp = popHighTemp.getWritePointer(c);
            juce::dsp::AudioBlock<float> bBlock(&bp, 1, (size_t)numSamples);
            juce::dsp::AudioBlock<float> hBlock(&hp, 1, (size_t)numSamples);
            juce::dsp::ProcessContextReplacing<float> bCtx(bBlock);
            juce::dsp::ProcessContextReplacing<float> hCtx(hBlock);
            popLow[c].process(bCtx);
            popHigh[c].process(hCtx);
        }

        float threshLin   = std::pow(10.0f, popThresholdDb / 20.0f);
        float duckGainLin = std::pow(10.0f, kPopDuckDb     / 20.0f);

        for (int i = 0; i < numSamples; ++i) {
            float bassMag = 0.0f;
            for (int c = 0; c < procChans; ++c)
                bassMag += std::abs(popBassTemp.getSample(c, i));
            bassMag /= (float)procChans;

            popFastEnv += popFastAlpha * (bassMag - popFastEnv);
            popSlowEnv += popSlowAlpha * (bassMag - popSlowEnv);
            bool trigger = (popFastEnv > threshLin) && (popFastEnv > popSlowEnv * 2.0f);
            float targetGain = trigger ? duckGainLin : 1.0f;
            float a = (targetGain < popGain) ? popAttackAlpha : popReleaseAlpha;
            popGain += a * (targetGain - popGain);

            for (int c = 0; c < procChans; ++c) {
                popBassTemp.getWritePointer(c)[i] *= popGain;
            }
        }
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            const float* lo = popBassTemp.getReadPointer(c);
            const float* hi = popHighTemp.getReadPointer(c);
            for (int i = 0; i < numSamples; ++i) dst[i] = lo[i] + hi[i];
        }

        float activity = juce::jlimit(0.0f, 1.0f,
                                      (1.0f - popGain) / (1.0f - duckGainLin));
        popActivity.store(activity);
    } else {
        popFastEnv = popSlowEnv = 0.0f;
        popGain = 1.0f;
        for (int c = 0; c < procChans; ++c) {
            popLow[c].reset();
            popHigh[c].reset();
        }
        popActivity.store(0.0f);
    }
}

void VocalEffectsProcessor::processPostPitch(juce::AudioBuffer<float>& buffer)
{
    int numSamples = buffer.getNumSamples();
    int hostChans = buffer.getNumChannels();
    int procChans = juce::jmin(numChans, hostChans > 0 ? hostChans : 1);

    auto* L = buffer.getWritePointer(0);
    auto* R = (hostChans > 1) ? buffer.getWritePointer(1) : L;

    // ----- 1) Exciter -----
    if (exciterDb > 0.01f) {
        float consonantScale = isConsonantActive ? 0.0f : 1.0f;
        float exciterGain = consonantScale * std::pow(10.0f, (exciterDb - 12.0f) / 20.0f);
        
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            for (int i = 0; i < numSamples; ++i) {
                float x = dst[i];
                float xHigh = exciterCrossover[c].processSample(x);
                
                float xDrive = xHigh * 4.0f;
                float f_pos = (xDrive + 1.0f) / (1.0f + std::abs(xDrive + 1.0f)) - 0.5f;
                float f_neg = (-xDrive + 1.0f) / (1.0f + std::abs(-xDrive + 1.0f)) - 0.5f;
                
                float xEven = 0.5f * (f_pos + f_neg);
                float xOdd  = 0.5f * (f_pos - f_neg);
                float xHarm = xEven - xOdd;
                
                float xHarmFiltered = exciterHarmonicFilter[c].processSample(xHarm);
                dst[i] += exciterGain * xHarmFiltered;
            }
        }
    } else {
        for (int c = 0; c < procChans; ++c) {
            exciterCrossover[c].reset();
            exciterHarmonicFilter[c].reset();
        }
    }

    // ----- 2) Tone EQ (post) -----
    if (std::abs(currentSibilantsDb) > 0.01f) {
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            juce::dsp::AudioBlock<float> tBlock(&dst, 1, (size_t)numSamples);
            juce::dsp::ProcessContextReplacing<float> tCtx(tBlock);
            sibilantsFilter[c].process(tCtx);
        }
    } else {
        for (int c = 0; c < procChans; ++c) {
            sibilantsFilter[c].reset();
        }
    }

    // ----- 3) Smart breath gate application -----
    int N = breathBlockIndex - 1;
    if (N < 0) N += 256;

    if (breathThresholdDb < -0.01f) {
        bool trigger = breathGateDelay[(size_t)N];
        float duckGainLin = std::pow(10.0f, kBreathDuckDb / 20.0f);
        float targetGain = trigger ? duckGainLin : 1.0f;
        
        float a = (targetGain < breathGain) ? breathAttackAlpha : breathReleaseAlpha;
        for (int i = 0; i < numSamples; ++i) {
            breathGain += a * (targetGain - breathGain);
            for (int c = 0; c < procChans; ++c) {
                float* dst = (c == 0) ? L : R;
                dst[i] *= breathGain;
            }
        }
        breathActivity.store(1.0f - breathGain);
    } else {
        breathGain = 1.0f;
        breathActivity.store(0.0f);
    }
    breathGateDelay[(size_t)N] = false;
}
