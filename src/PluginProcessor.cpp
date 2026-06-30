// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Lock-free running-max accumulate for the meters: the audio thread bumps the peak, the UI
    // read-resets it (exchange to 0). No locks, bounded by (near-zero) contention.
    inline void accMax (std::atomic<float>& a, float v) noexcept
    {
        float prev = a.load (std::memory_order_relaxed);
        while (v > prev && ! a.compare_exchange_weak (prev, v, std::memory_order_relaxed)) {}
    }

    // Multi-channel de-zippered gain — the JUCE-free equivalent of juce::LinearSmoothedValue::applyGain
    // (AudioBuffer overload): one ramp step per sample, applied to every channel (bit-exact same numerics).
    inline void applyGainRamp (teq::LinearSmoother& sm, juce::AudioBuffer<float>& buf, int n) noexcept
    {
        const int nch = buf.getNumChannels();
        for (int i = 0; i < n; ++i)
        {
            const float g = sm.getNextValue();
            for (int ch = 0; ch < nch; ++ch) buf.getWritePointer (ch)[i] *= g;
        }
    }
}

TabbyEqAudioProcessor::TabbyEqAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", tabby::createParameterLayout())
{
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        auto& p = bands[(size_t) b];
        p.on    = apvts.getRawParameterValue (tabby::bandId (b, "on"));
        p.type  = apvts.getRawParameterValue (tabby::bandId (b, "type"));
        p.freq  = apvts.getRawParameterValue (tabby::bandId (b, "freq"));
        p.q     = apvts.getRawParameterValue (tabby::bandId (b, "q"));
        p.gain  = apvts.getRawParameterValue (tabby::bandId (b, "gain"));
        p.slope = apvts.getRawParameterValue (tabby::bandId (b, "slope"));
        p.swept = apvts.getRawParameterValue (tabby::bandId (b, "swept"));
        p.bypass = apvts.getRawParameterValue (tabby::bandId (b, "bypass"));
        p.ms     = apvts.getRawParameterValue (tabby::bandId (b, "ms"));
        p.sOn    = apvts.getRawParameterValue (tabby::bandId (b, "sOn"));
        p.sType  = apvts.getRawParameterValue (tabby::bandId (b, "sType"));
        p.sFreq  = apvts.getRawParameterValue (tabby::bandId (b, "sFreq"));
        p.sQ     = apvts.getRawParameterValue (tabby::bandId (b, "sQ"));
        p.sGain  = apvts.getRawParameterValue (tabby::bandId (b, "sGain"));
        p.sSlope = apvts.getRawParameterValue (tabby::bandId (b, "sSlope"));
        p.sBypass= apvts.getRawParameterValue (tabby::bandId (b, "sBypass"));
    }
    outputGain = apvts.getRawParameterValue ("output");
    phaseMode  = apvts.getRawParameterValue ("phaseMode");
    lpQuality  = apvts.getRawParameterValue ("lpQuality");
    lpUpdater.startTimerHz (30);   // coalesces param edits into background FIR rebuilds
}

void TabbyEqAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());   // engine clamps to teq::kMaxChannels
    outputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));   // start at the saved trim — no ramp on load
    soloFilter.prepare (sampleRate, getTotalNumOutputChannels());

    // Linear-phase path: build the initial FIR from the current params so it's ready immediately.
    lpPrepared = false;
    std::array<teq::BandParams, tabby::kNumBands> snap;
    for (int b = 0; b < tabby::kNumBands; ++b) snap[(size_t) b] = readBand (b);
    lp.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels(),
                (int) lpQuality->load(), snap.data(), tabby::kNumBands);
    lastQuality = (int) lpQuality->load();
    lastLinear  = phaseMode->load() > 1.5f;   // index 2 = Linear Phase (FIR); index 0/1 = matched IIR
    setLatencySamples (lastLinear ? lp.latencySamples() : 0);
    lpPrepared = true;
}

// Generic any-channel support up to the engine's cap: any MATCHED layout (mono, stereo, 5.1, 7.1,
// 7.1.4 Atmos, ambisonics, … up to teq::kMaxChannels), plus the one conventional convenience up-mix
// mono->stereo. We deliberately don't fan a mono source out onto surround/ambisonic buses — copying
// mono into B-format components or an LFE is a panner/encoder's job, not an EQ's. No down-mix either.
bool TabbyEqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out.isDisabled() || in.isDisabled()) return false;
    if (out.size() < 1 || out.size() > teq::EqEngine::kMaxChannels) return false;
    if (in == out) return true;                                                                  // matched: mono..16ch
    return in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo();         // mono->stereo only
}

void TabbyEqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;   // FTZ/DAZ on the audio thread (engine also flushes per block)

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n      = buffer.getNumSamples();
    const int nc     = juce::jmin (numOut, teq::EqEngine::kMaxChannels);   // channels we EQ
    if (nc <= 0) return;

    // Up-mix a mono input into stereo (the only non-matched layout we accept) => identical L/R.
    for (int c = juce::jmax (1, numIn); c < nc; ++c) buffer.copyFrom (c, 0, buffer, 0, 0, n);

    // IN/OUT level metering — only while a UI is attached. Accumulate the block peak; clip is sticky
    // (until the user clicks the meter). meterOutput() is called just before each return path.
    const bool meter = analyzerRefs.load (std::memory_order_relaxed) > 0;
    if (meter)
    {
        const float inPk = buffer.getMagnitude (0, n);
        accMax (inPeak, inPk);
        if (inPk >= 1.0f) inClip.store (true, std::memory_order_relaxed);
    }
    auto meterOutput = [this, &buffer, n, nc, meter]() noexcept
    {
        if (! meter) return;
        const float pk = buffer.getMagnitude (0, n);
        accMax (outPeak, pk);
        if (pk >= 1.0f) outClip.store (true, std::memory_order_relaxed);

        float corr = 1.0f;                                                  // L/R phase correlation (mono => 1)
        if (nc >= 2)
        {
            const float* L = buffer.getReadPointer (0);
            const float* R = buffer.getReadPointer (1);
            double sLR = 0.0, sLL = 0.0, sRR = 0.0;
            for (int s = 0; s < n; ++s) { sLR += (double) L[s] * R[s]; sLL += (double) L[s] * L[s]; sRR += (double) R[s] * R[s]; }
            corr = (sLL > 1e-12 && sRR > 1e-12) ? (float) (sLR / std::sqrt (sLL * sRR)) : 0.0f;
        }
        corrState = 0.85f * corrState + 0.15f * corr;                       // light smoothing for a steady meter
        correlation.store (corrState, std::memory_order_relaxed);
    };

    // Feed the analyzer FIFOs the chosen domain (Stereo ch0 / Mid / Side) — replaces the engine's
    // old internal ch0 push, so the analyzer can show the M/S content.
    auto pushDomain = [this, &buffer, n, nc] (teq::SpectrumTap& tap) noexcept
    {
        const int dom = spectrumDomain.load (std::memory_order_relaxed);
        const float* L = buffer.getReadPointer (0);
        const float* R = nc > 1 ? buffer.getReadPointer (1) : L;
        if      (nc < 2 || dom == 0) for (int s = 0; s < n; ++s) tap.push (L[s]);
        else if (dom == 1)           for (int s = 0; s < n; ++s) tap.push (0.5f * (L[s] + R[s]));
        else                         for (int s = 0; s < n; ++s) tap.push (0.5f * (L[s] - R[s]));
    };

    // Drag-audition: a narrow band-pass at an arbitrary frequency (search-by-ear while dragging),
    // independent of the band list. Takes precedence over the normal path and per-band solo.
    if (auditionOn.load (std::memory_order_relaxed))
    {
        soloFilter.setParams (teq::FilterType::BandPass,
                              juce::jlimit (20.0, 20000.0, (double) auditionFreq.load (std::memory_order_relaxed)),
                              juce::jlimit (0.5, 18.0,     (double) auditionQ.load   (std::memory_order_relaxed)), 0.0);
        for (int c = 0; c < nc; ++c)
        {
            float* d = buffer.getWritePointer (c);
            for (int s = 0; s < n; ++s) d[s] = soloFilter.processSample (c, d[s]);
        }
        soloFilter.flushDenormals();
        outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
        applyGainRamp (outputGainSmoothed, buffer, n);
        meterOutput();
        return;
    }

    // Solo (band-listen): replace the output with a band-pass of the input at the soloed band's
    // freq/Q, so you hear only that region. Skips the normal EQ.
    const int solo = soloBand.load (std::memory_order_relaxed);
    if (solo >= 0 && solo < tabby::kNumBands)
    {
        const auto bp = readBand (solo);
        soloFilter.setParams (teq::FilterType::BandPass, bp.freq, juce::jlimit (0.5, 12.0, bp.Q), 0.0);
        for (int c = 0; c < nc; ++c)
        {
            float* d = buffer.getWritePointer (c);
            for (int s = 0; s < n; ++s) d[s] = soloFilter.processSample (c, d[s]);
        }
        soloFilter.flushDenormals();
        outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
        applyGainRamp (outputGainSmoothed, buffer, n);
        meterOutput();
        return;
    }

    // Main EQ path. Switching modes (Zero-Latency IIR <-> Linear FIR) is a deliberate, rare click, so a
    // hard cut with a seam is fine — and the reported latency itself changes (0 <-> N/2), so the host
    // re-aligns anyway. What MUST stay artifact-free is editing a band WHILE in a mode: the IIR engine
    // smooths its coefficients, and the FIR path swaps its impulse click-free inside the convolver
    // (lpTick coalesces edits -> ConvolutionEngine crossfade). There is no cross-path blend here — that
    // fragile audio-thread state, reset under the host's latency-change re-prepare, was the silent-Natural bug.
    const bool linear = phaseMode->load (std::memory_order_relaxed) > 1.5f;   // index 2 = Linear Phase (FIR)
    if (linear)
    {
        // The FIR is rebuilt off the audio thread (lpTick); here we only convolve and hand the analyzer
        // its pre/post samples (engine.process would normally do that). We deliberately do NOT reset the
        // convolver on resume: reset() rewinds it to the empty bank (active=0, state=Idle) and strands the
        // IR that prepare() loaded into the inactive bank as a Pending crossfade — that was the "Linear is
        // silent until you nudge a band" bug. Letting it run consumes the Pending fade and primes the IR.
        if (meter) pushDomain (engine.inputTap());
        lp.process (buffer, nc);
        if (meter) pushDomain (engine.outputTap());
    }
    else
    {
        // Feed each band's params to the engine HERE (audio thread) so setBand/process share a thread
        // — the engine's contract. Smoothing + recompute-skip live inside the engine.
        if (meter) pushDomain (engine.inputTap());
        for (int b = 0; b < tabby::kNumBands; ++b)
            engine.setBand (b, readBand (b));
        engine.process (buffer.getArrayOfWritePointers(), nc, n);
        if (meter) pushDomain (engine.outputTap());
    }

    outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
    applyGainRamp (outputGainSmoothed, buffer, n);   // de-zippered output trim
    meterOutput();
}

// Message-thread (30 Hz): push the current params to the linear-phase builder and react to mode /
// quality changes (rebuild the FIR, re-report latency for the host's plugin-delay compensation).
void TabbyEqAudioProcessor::lpTick()
{
    if (! lpPrepared) return;

    const int  q   = (int) lpQuality->load();
    const bool lin = phaseMode->load() > 1.5f;   // index 2 = Linear Phase (FIR)

    if (q != lastQuality)   { lastQuality = q; lp.setQuality (q); if (lin) setLatencySamples (lp.latencySamples()); }
    if (lin != lastLinear)  { lastLinear = lin; setLatencySamples (lin ? lp.latencySamples() : 0); }

    if (lin)
    {
        std::array<teq::BandParams, tabby::kNumBands> snap;
        for (int b = 0; b < tabby::kNumBands; ++b) snap[(size_t) b] = readBand (b);
        lp.updateSnapshot (snap.data(), tabby::kNumBands);
    }
}

void TabbyEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TabbyEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    // `stateVersion` is available here for future migrations; v1 needs none.
}

teq::BandParams TabbyEqAudioProcessor::readBand (int b) const noexcept
{
    const auto& p = bands[(size_t) b];
    teq::BandParams bp;
    bp.on     = p.on->load()    > 0.5f;
    bp.type   = tabby::filterTypeFromChoice ((int) p.type->load());
    bp.freq   = (double) p.freq->load();
    bp.Q      = (double) p.q->load();
    bp.gainDb = (double) p.gain->load();
    static constexpr int kSlopeDb[] = { 6, 12, 24, 36, 48, 72, 96 };
    bp.slope  = kSlopeDb[juce::jlimit (0, 6, (int) p.slope->load())];
    bp.swept  = p.swept->load() > 0.5f;
    bp.bypass = p.bypass->load() > 0.5f;

    bp.ms      = p.ms->load()  > 0.5f;
    bp.sOn     = p.sOn->load() > 0.5f;
    bp.sType   = tabby::filterTypeFromChoice ((int) p.sType->load());
    bp.sFreq   = (double) p.sFreq->load();
    bp.sQ      = (double) p.sQ->load();
    bp.sGainDb = (double) p.sGain->load();
    bp.sSlope  = kSlopeDb[juce::jlimit (0, 6, (int) p.sSlope->load())];
    bp.sBypass = p.sBypass->load() > 0.5f;
    return bp;
}

juce::AudioProcessorEditor* TabbyEqAudioProcessor::createEditor()
{
    return new TabbyEqEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TabbyEqAudioProcessor();
}
