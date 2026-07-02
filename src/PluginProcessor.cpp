// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

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

    static constexpr int kSlopeDb[] = { 6, 12, 24, 36, 48, 72, 96 };   // slope choice index -> dB/oct

    // ---- v2 -> v3 state migration (see docs/LANES.md "State migration") ----------------------------------
    // v2 was a flat "Mid/main" band + an M/S "Side" lane, gated by `ms`; v3 is a shared point (type/swept)
    // split across five placement lanes. Translate BY VALUE. Two documented lossy corners: sType fission
    // when the free-slot pool is full (coerce + migrationNote), and the point-bypass semantics (v2's flat
    // bypass was a Mid-lane bypass; the new point bypass has no v2 ancestor -> always false).
    juce::ValueTree migrateV2toV3 (const juce::ValueTree& in, const juce::Identifier& stateType)
    {
        using namespace tabby;

        auto oldReal = [&in] (const juce::String& id, double fb) -> double
        {
            for (int i = 0; i < in.getNumChildren(); ++i)
            {
                const auto ch = in.getChild (i);
                if (ch.getProperty ("id").toString() == id) return (double) ch.getProperty ("value", fb);
            }
            return fb;
        };
        auto oldBool = [&] (const juce::String& id, bool fb) { return oldReal (id, fb ? 1.0 : 0.0) > 0.5; };
        auto oldIdx  = [&] (const juce::String& id, int hi, double fb) { return juce::jlimit (0, hi, (int) oldReal (id, fb)); };

        struct MigLane { bool on; double freq, q, gain; int slope; bool byp; };
        struct MigBand { bool on = false; int type = 0; bool swept = false; MigLane lane[5]; bool linkFq = false; };

        std::array<MigBand, kNumBands> mig;
        for (auto& m : mig)
            for (int L = 0; L < kNumLanes; ++L)
                m.lane[L] = { L == 0, 1000.0, 1.0, 0.0, 1, false };   // fresh: ST lane on, rest off

        const bool msFreqLink = oldBool ("msFreqLink", false)          // was a ValueTree property in v2 (not a param)
                             || (bool) in.getProperty ("msFreqLink", false);
        std::array<bool, kNumBands> consumed {};
        bool migrationNote = false;

        auto oldOn    = [&] (int b) { return oldBool (bandId (b, "on"), false); };
        auto nextFree = [&] () -> int { for (int i = 0; i < kNumBands; ++i) if (! oldOn (i) && ! consumed[(size_t) i]) return i; return -1; };

        for (int b = 0; b < kNumBands; ++b)
        {
            if (consumed[(size_t) b]) continue;   // a fission target claimed this slot (t>b) — leave it intact

            mig[(size_t) b].on    = oldOn (b);
            mig[(size_t) b].type  = oldIdx (bandId (b, "type"), 8, 0.0);
            mig[(size_t) b].swept = oldBool (bandId (b, "swept"), false);

            const double oFreq  = oldReal (bandId (b, "freq"), 1000.0);
            const double oQ     = oldReal (bandId (b, "q"), 1.0);
            const double oGain  = oldReal (bandId (b, "gain"), 0.0);
            const int    oSlope = oldIdx  (bandId (b, "slope"), 6, 1.0);
            const bool   oByp   = oldBool (bandId (b, "bypass"), false);
            const bool   oMs    = oldBool (bandId (b, "ms"), false);

            if (! oMs)
            {
                mig[(size_t) b].lane[0] = { true, oFreq, oQ, oGain, oSlope, oByp };   // ST <- flat (bypass -> ST lane byp)
            }
            else
            {
                const int    sType  = oldIdx  (bandId (b, "sType"), 8, 0.0);
                const bool   sOn    = oldBool (bandId (b, "sOn"), true);
                const double sFreq  = oldReal (bandId (b, "sFreq"), 1000.0);
                const double sQ     = oldReal (bandId (b, "sQ"), 1.0);
                const double sGain  = oldReal (bandId (b, "sGain"), 0.0);
                const int    sSlope = oldIdx  (bandId (b, "sSlope"), 6, 1.0);
                const bool   sByp   = oldBool (bandId (b, "sBypass"), false);

                mig[(size_t) b].lane[0].on = false;                                      // ST off
                mig[(size_t) b].lane[3]    = { true, oFreq, oQ, oGain, oSlope, oByp };   // Mid <- flat (bypass -> Mid lane byp)

                // Fission ONLY for a LIVE band. A deleted (off) v2 band can carry stale ms/sType data;
                // fissioning that would RESURRECT the deleted Side as a new, audible ON point. An off
                // ms-band migrates in place ({m,s} lanes on band b) with sType silently coerced to the
                // shared type — inaudible (the point is off), so no migrationNote either.
                const bool live  = mig[(size_t) b].on;
                const int  freeT = (live && sType != mig[(size_t) b].type) ? nextFree() : -1;
                if (sType == mig[(size_t) b].type || freeT < 0)
                {
                    mig[(size_t) b].lane[4] = { sOn, sFreq, sQ, sGain, sSlope, sByp };   // Side on band b
                    if (live && sType != mig[(size_t) b].type) migrationNote = true;     // pool full on a LIVE band -> audible coercion
                    mig[(size_t) b].linkFq = msFreqLink && std::abs (oFreq - sFreq) < 0.01;
                }
                else   // fission: Side moves to the first free slot as an {s}-only point of type=sType
                {
                    mig[(size_t) b].linkFq = false;
                    consumed[(size_t) freeT] = true;
                    // Pristine slot FIRST: a target with index < b may already hold an OFF band's
                    // in-place-migrated lanes (stale Mid/Side of a DELETED point). Turning the slot on
                    // without wiping them would resurrect that deleted state inside the fissioned point.
                    mig[(size_t) freeT] = MigBand {};
                    for (int L = 0; L < kNumLanes; ++L)
                        mig[(size_t) freeT].lane[(size_t) L] = { L == 0, 1000.0, 1.0, 0.0, 1, false };
                    mig[(size_t) freeT].on    = true;
                    mig[(size_t) freeT].type  = sType;
                    mig[(size_t) freeT].swept = false;
                    mig[(size_t) freeT].lane[0].on = false;                              // ST off
                    mig[(size_t) freeT].lane[4]    = { sOn, sFreq, sQ, sGain, sSlope, sByp };
                    mig[(size_t) freeT].linkFq = false;
                }
            }
        }

        // --- emit the v3 state tree ---
        juce::ValueTree out (stateType);
        for (int i = 0; i < in.getNumProperties(); ++i)                                 // carry view/session props forward
        {
            const auto name = in.getPropertyName (i);
            out.setProperty (name, in.getProperty (name), nullptr);
        }
        out.setProperty ("stateVersion", 3, nullptr);
        out.setProperty ("defaultLinkFq", msFreqLink, nullptr);
        out.setProperty ("defaultLinkQ", false, nullptr);
        if (migrationNote) out.setProperty ("migrationNote", true, nullptr);
        for (int b = 0; b < kNumBands; ++b)
        {
            out.setProperty (bandId (b, "linkFq"), mig[(size_t) b].linkFq, nullptr);
            out.setProperty (bandId (b, "linkQ"), false, nullptr);
        }

        auto add = [&out] (const juce::String& id, double v)
        {
            juce::ValueTree n ("PARAM");
            n.setProperty ("id", id, nullptr);
            n.setProperty ("value", v, nullptr);
            out.addChild (n, -1, nullptr);
        };
        add ("output",      oldReal ("output", 0.0));
        add ("phaseMode",   juce::jlimit (0.0, 2.0, oldReal ("phaseMode", 0.0)));
        add ("lpQuality",   juce::jlimit (0.0, 4.0, oldReal ("lpQuality", 1.0)));
        add ("phaseAmount", juce::jlimit (0.0, 1.0, oldReal ("phaseAmount", 0.5)));
        for (int b = 0; b < kNumBands; ++b)
        {
            add (bandId (b, "on"),     mig[(size_t) b].on ? 1.0 : 0.0);
            add (bandId (b, "type"),   (double) mig[(size_t) b].type);
            add (bandId (b, "swept"),  mig[(size_t) b].swept ? 1.0 : 0.0);
            add (bandId (b, "bypass"), 0.0);
            for (int L = 0; L < kNumLanes; ++L)
            {
                const auto& ln = mig[(size_t) b].lane[L];
                add (laneParamId (b, L, "on"),    ln.on ? 1.0 : 0.0);
                add (laneParamId (b, L, "freq"),  ln.freq);
                add (laneParamId (b, L, "q"),     ln.q);
                add (laneParamId (b, L, "gain"),  ln.gain);
                add (laneParamId (b, L, "slope"), (double) ln.slope);
                add (laneParamId (b, L, "byp"),   ln.byp ? 1.0 : 0.0);
            }
        }
        return out;
    }
}

TabbyEqAudioProcessor::TabbyEqAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", tabby::createParameterLayout())
{
    // Wire the per-band / per-lane atomic parameter pointers, and build the link-mirror index maps.
    const int numParams = getParameters().size();
    linkField.assign   ((size_t) numParams, (int8_t) -1);
    linkBand.assign    ((size_t) numParams, (int16_t) -1);
    linkLaneIdx.assign ((size_t) numParams, (int16_t) -1);

    auto mapLink = [this] (const juce::String& id, int band, int lane, int8_t field)
    {
        if (auto* p = apvts.getParameter (id))
        {
            const int idx = p->getParameterIndex();
            if (idx >= 0 && idx < (int) linkField.size())
            {
                linkField[(size_t) idx]   = field;
                linkBand[(size_t) idx]    = (int16_t) band;
                linkLaneIdx[(size_t) idx] = (int16_t) lane;
                p->addListener (this);            // listener body is alloc/lock-free: it only pushes to the FIFO
            }
        }
    };

    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        auto& p = bands[(size_t) b];
        p.on     = apvts.getRawParameterValue (tabby::bandId (b, "on"));
        p.type   = apvts.getRawParameterValue (tabby::bandId (b, "type"));
        p.swept  = apvts.getRawParameterValue (tabby::bandId (b, "swept"));
        p.bypass = apvts.getRawParameterValue (tabby::bandId (b, "bypass"));
        for (int L = 0; L < tabby::kNumLanes; ++L)
        {
            auto& ln = p.lane[L];
            ln.on    = apvts.getRawParameterValue (tabby::laneParamId (b, L, "on"));
            ln.freq  = apvts.getRawParameterValue (tabby::laneParamId (b, L, "freq"));
            ln.q     = apvts.getRawParameterValue (tabby::laneParamId (b, L, "q"));
            ln.gain  = apvts.getRawParameterValue (tabby::laneParamId (b, L, "gain"));
            ln.slope = apvts.getRawParameterValue (tabby::laneParamId (b, L, "slope"));
            ln.byp   = apvts.getRawParameterValue (tabby::laneParamId (b, L, "byp"));
            // Link mirroring listens to the position/width params of every lane. Each event carries the
            // EXACT field (freq/q/slope) + the captured value — the drain replays them verbatim (freq is
            // gated by linkFq, q AND slope by linkQ; no drain-time type mapping).
            mapLink (tabby::laneParamId (b, L, "freq"),  b, L, (int8_t) kFieldFreq);
            mapLink (tabby::laneParamId (b, L, "q"),     b, L, (int8_t) kFieldQ);
            mapLink (tabby::laneParamId (b, L, "slope"), b, L, (int8_t) kFieldSlope);
        }
        activeLaneAtom[(size_t) b].store (-1, std::memory_order_relaxed);
    }

    outputGain  = apvts.getRawParameterValue ("output");
    phaseMode   = apvts.getRawParameterValue ("phaseMode");
    lpQuality   = apvts.getRawParameterValue ("lpQuality");
    phaseAmount = apvts.getRawParameterValue ("phaseAmount");
    lpUpdater.startTimerHz (30);   // coalesces param edits into background FIR rebuilds + drains the link FIFO
}

TabbyEqAudioProcessor::~TabbyEqAudioProcessor()
{
    lpUpdater.stopTimer();
    for (int i = 0; i < (int) linkField.size(); ++i)
        if (linkField[(size_t) i] >= 0)
            if (auto* p = getParameters()[i])
                p->removeListener (this);
}

void TabbyEqAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());   // engine clamps to teq::kMaxChannels
    outputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));   // start at the saved trim — no ramp on load
    soloFilter.prepare (sampleRate, getTotalNumOutputChannels());

    // FIR paths: build BOTH initial FIRs from the current params so either mode is ready immediately.
    prepared.store (false, std::memory_order_relaxed);
    std::array<teq::BandParams, tabby::kNumBands> snap;
    for (int b = 0; b < tabby::kNumBands; ++b) snap[(size_t) b] = readBand (b);
    const int chans = getTotalNumOutputChannels();
    lp.prepare (sampleRate, maximumExpectedSamplesPerBlock, chans, (int) lpQuality->load(), snap.data(), tabby::kNumBands);
    np.prepare (sampleRate, maximumExpectedSamplesPerBlock, chans, kNaturalQuality, phaseAmount->load(), snap.data(), tabby::kNumBands);
    lastQuality = (int) lpQuality->load();
    lastK       = phaseAmount->load();
    lastMode    = (int) (phaseMode->load() + 0.5f);   // 0 Zero-Latency / 1 Natural / 2 Linear
    setLatencySamples (lastMode == 2 ? lp.latencySamples() : lastMode == 1 ? np.latencySamples() : 0);
    prepared.store (true, std::memory_order_release);   // publish LAST: the engine + both FIRs are now fully built
}

// Generic any-channel support up to the engine's cap: any MATCHED layout (mono, stereo, 5.1, 7.1,
// 7.1.4 Atmos, ambisonics, … up to teq::kMaxChannels), plus the one conventional convenience up-mix
// mono->stereo. We deliberately don't fan a mono source out onto surround/ambisonic buses. No down-mix.
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

    // H1 — own our lifecycle safety: do NO DSP before prepareToPlay() completes or after releaseResources().
    if (! prepared.load (std::memory_order_acquire)) return;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n      = buffer.getNumSamples();
    // H2 — never index past the REAL buffer.
    const int nc     = juce::jmin (numOut, teq::EqEngine::kMaxChannels, buffer.getNumChannels());   // channels we EQ
    if (nc <= 0) return;

    // Up-mix a mono input into stereo (the only non-matched layout we accept) => identical L/R.
    for (int c = juce::jmax (1, numIn); c < nc; ++c) buffer.copyFrom (c, 0, buffer, 0, 0, n);

    // IN/OUT level metering — only while a UI is attached.
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

    // Feed the analyzer FIFOs the chosen domain (Stereo ch0 / Mid / Side).
    auto pushDomain = [this, &buffer, n, nc] (teq::SpectrumTap& tap) noexcept
    {
        const int dom = spectrumDomain.load (std::memory_order_relaxed);
        const float* L = buffer.getReadPointer (0);
        const float* R = nc > 1 ? buffer.getReadPointer (1) : L;
        if      (nc < 2 || dom == 0) for (int s = 0; s < n; ++s) tap.push (L[s]);
        else if (dom == 1)           for (int s = 0; s < n; ++s) tap.push (0.5f * (L[s] + R[s]));
        else                         for (int s = 0; s < n; ++s) tap.push (0.5f * (L[s] - R[s]));
    };

    // Drag-audition: a narrow band-pass at an arbitrary frequency. Takes precedence over the normal path.
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

    // Solo (band-listen): band-pass the input at the soloed band's ACTIVE lane freq/Q.
    const int solo = soloBand.load (std::memory_order_relaxed);
    if (solo >= 0 && solo < tabby::kNumBands)
    {
        const auto bp = readBand (solo);
        int lane = activeLaneAtom[(size_t) solo].load (std::memory_order_relaxed);
        if (lane < 0 || lane >= teq::kNumLanes || ! bp.lanes[(size_t) lane].on)
        {
            lane = 0;                                                        // fall back to the lowest enabled lane
            for (int L = 0; L < teq::kNumLanes; ++L) if (bp.lanes[(size_t) L].on) { lane = L; break; }
        }
        const auto& ln = bp.lane ((teq::Lane) lane);
        soloFilter.setParams (teq::FilterType::BandPass, ln.freq, juce::jlimit (0.5, 12.0, ln.Q), 0.0);
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

    // Main EQ path.
    const int mode = (int) (phaseMode->load (std::memory_order_relaxed) + 0.5f);   // 0 Zero-Latency / 1 Natural / 2 Linear
    if (mode >= 1)
    {
        if (meter) pushDomain (engine.inputTap());
        if (mode == 2) lp.process (buffer, nc);
        else           np.process (buffer, nc);
        if (meter) pushDomain (engine.outputTap());
    }
    else
    {
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

//==============================================================================
// Message thread (30 Hz): drain the link-mirror FIFO, then feed the FIR builder / track mode changes.
void TabbyEqAudioProcessor::onTimer()
{
    drainLinkFifo();
    lpTick();
}

void TabbyEqAudioProcessor::lpTick()
{
    if (! prepared.load (std::memory_order_acquire)) return;

    const int   q    = (int) lpQuality->load();
    const int   mode = (int) (phaseMode->load() + 0.5f);   // 0 Zero-Latency / 1 Natural / 2 Linear
    const float k    = phaseAmount->load();

    auto reportLatency = [this] (int m) { setLatencySamples (m == 2 ? lp.latencySamples() : m == 1 ? np.latencySamples() : 0); };

    if (q != lastQuality)               { lastQuality = q; lp.setQuality (q); if (mode == 2) reportLatency (mode); }
    if (std::abs (k - lastK) > 1.0e-4f) { lastK = k;       np.setBlend (k); }   // Natural latency is fixed → no re-report
    if (mode != lastMode)               { lastMode = mode; reportLatency (mode); }

    if (mode >= 1)   // feed the active FIR builder (Natural or Linear)
    {
        std::array<teq::BandParams, tabby::kNumBands> snap;
        for (int b = 0; b < tabby::kNumBands; ++b) snap[(size_t) b] = readBand (b);
        if (mode == 2) lp.updateSnapshot (snap.data(), tabby::kNumBands);
        else           np.updateSnapshot (snap.data(), tabby::kNumBands);
    }
}

//==============================================================================
// Link mirroring — the producer (any thread) + the message-thread drain. Honesty: the listener BODY is
// alloc/lock-free (FIFO push + two relaxed stores), but JUCE's own listener dispatch briefly holds the
// parameter's listener lock — uncontended except during editor open/close listener churn, and inherent to
// every JUCE parameter listener/attachment, not added by this design.
// Re-entrancy tag for the mirror writes. THREAD-LOCAL, not a shared atomic: a shared flag would also
// swallow a GENUINE audio-thread automation event that lands inside the message thread's brief mirror
// window (the edit itself would apply but its mirror would silently be skipped, desyncing linked lanes
// until the next edit). Thread-local suppresses exactly the writes made BY the mirroring/migration code
// on its own thread and nothing else.
static thread_local bool tlsMirrorWrite = false;

void TabbyEqAudioProcessor::parameterValueChanged (int parameterIndex, float newValue)
{
    if (tlsMirrorWrite) return;                                               // our own mirror write — never re-enqueue
    if (parameterIndex < 0 || parameterIndex >= (int) linkField.size()) return;
    const int8_t field = linkField[(size_t) parameterIndex];
    if (field < 0) return;
    // Capture the exact FIELD and the VALUE the listener saw: the drain replays captured values verbatim,
    // so interleaved edits of two linked lanes can't corrupt each other via drain-time source re-reads, and
    // a type change between enqueue and drain can't remap which param a width event means.
    linkFifo.push ({ (int) linkBand[(size_t) parameterIndex], (int) linkLaneIdx[(size_t) parameterIndex],
                     (int) field, newValue });                                // sets overflowed on full
    linkDirty.store (true, std::memory_order_release);
}

void TabbyEqAudioProcessor::drainLinkFifo()
{
    auto flush = [this] { LinkEvent e; while (linkFifo.pop (e)) {} };

    if (linkFifo.overflowed.exchange (false, std::memory_order_acquire))     // storm: discard partial stream, resync
    {
        flush();
        linkDirty.store (false, std::memory_order_relaxed);
        resyncAllLinks();
        return;
    }
    if (! linkDirty.exchange (false, std::memory_order_acquire)) return;

    // Replay in FIFO delivery order, coalescing CONSECUTIVE events for the same param (only the run's last
    // value is mirrored — the earlier ones are dead intermediate states of the same knob).
    LinkEvent cur, e;
    bool have = false;
    while (linkFifo.pop (e))
    {
        if (have && ! (e.band == cur.band && e.lane == cur.lane && e.field == cur.field))
            mirrorEvent (cur);
        cur = e; have = true;
    }
    if (have) mirrorEvent (cur);

    if (linkFifo.overflowed.exchange (false, std::memory_order_acquire))     // overflow set mid-drain
    {
        flush();
        resyncAllLinks();
    }
}

// Write one captured normalized value to EVERY enabled lane of the point (disabled lanes are never
// written; they inherit on enable). Tagged so the writes never re-enqueue.
void TabbyEqAudioProcessor::applyToEnabledLanes (int band, const char* field, float v01)
{
    for (int L = 0; L < tabby::kNumLanes; ++L)
    {
        if (bands[(size_t) band].lane[L].on->load() <= 0.5f) continue;       // disabled lanes are not written
        if (auto* dst = apvts.getParameter (tabby::laneParamId (band, L, field)))
        {
            tlsMirrorWrite = true;                                           // tag: this write must not re-enqueue
            dst->setValueNotifyingHost (v01);                               // same range across lanes -> normalized copy is exact
            tlsMirrorWrite = false;
        }
    }
}

void TabbyEqAudioProcessor::mirrorEvent (const LinkEvent& ev)
{
    if (ev.band < 0 || ev.band >= tabby::kNumBands || ev.lane < 0 || ev.lane >= tabby::kNumLanes) return;
    if (ev.field < 0 || ev.field > kFieldSlope) return;
    if (bands[(size_t) ev.band].lane[ev.lane].on->load() <= 0.5f) return;    // a disabled source lane doesn't drive

    // Gate by the point's link flags: freq under linkFq; q AND slope under linkQ (both are the point's
    // width analogs — no drain-time type mapping, the event's field IS the param that moved).
    const char* prop   = (ev.field == kFieldFreq) ? "linkFq" : "linkQ";
    if (! (bool) apvts.state.getProperty (tabby::bandId (ev.band, prop), false)) return;

    // Replay the CAPTURED value onto every enabled lane — INCLUDING the source. Re-asserting the source is
    // what makes delivery order deterministic: when two linked lanes were edited between drains, the earlier
    // event's mirror overwrites the later lane's newer edit, and the later event then re-asserts its captured
    // value on all lanes — so the final state is the LAST delivered edit everywhere (LANES.md determinism).
    // The source write is a no-op re-assert in the common single-edit case.
    applyToEnabledLanes (ev.band, kLinkFieldNames[ev.field], ev.value);
}

int TabbyEqAudioProcessor::resyncActiveLane (int band) const
{
    const int prop = (int) apvts.state.getProperty (tabby::bandId (band, "activeLane"), -1);
    if (prop >= 0 && prop < tabby::kNumLanes && bands[(size_t) band].lane[prop].on->load() > 0.5f) return prop;
    for (int L = 0; L < tabby::kNumLanes; ++L) if (bands[(size_t) band].lane[L].on->load() > 0.5f) return L;   // lowest enabled
    return -1;
}

// Overflow recovery: for every linked point, snap all enabled lanes to the ACTIVE lane's current values
// (idempotent — the partial event stream was discarded, so "current active lane" is the best truth left).
// linkQ snaps BOTH q and slope: they are the point's two width analogs and both live under that flag
// (matches the event gating; no type mapping here either).
void TabbyEqAudioProcessor::resyncAllLinks()
{
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        const bool linkFq = (bool) apvts.state.getProperty (tabby::bandId (b, "linkFq"), false);
        const bool linkQ  = (bool) apvts.state.getProperty (tabby::bandId (b, "linkQ"),  false);
        if (! linkFq && ! linkQ) continue;
        const int active = resyncActiveLane (b);
        if (active < 0) continue;
        auto snap = [this, b, active] (const char* field)
        {
            if (auto* src = apvts.getParameter (tabby::laneParamId (b, active, field)))
                applyToEnabledLanes (b, field, src->getValue());
        };
        if (linkFq) snap ("freq");
        if (linkQ)  { snap ("q"); snap ("slope"); }
    }
}

void TabbyEqAudioProcessor::setBandActiveLane (int band, int lane) noexcept
{
    if (band < 0 || band >= tabby::kNumBands) return;
    const int L = juce::jlimit (0, tabby::kNumLanes - 1, lane);
    activeLaneAtom[(size_t) band].store (L, std::memory_order_relaxed);
    apvts.state.setProperty (tabby::bandId (band, "activeLane"), L, nullptr);
}

//==============================================================================
void TabbyEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TabbyEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    juce::ValueTree incoming = juce::ValueTree::fromXml (*xml);
    const int ver = (int) incoming.getProperty ("stateVersion", 2);   // a v2 XML without stateVersion counts as v2
    const bool legacy = ver <= 2;

    // Suppress link-mirror enqueues for the load's own param writes; drop any that slip through afterwards.
    tlsMirrorWrite = true;
    if (legacy) apvts.replaceState (migrateV2toV3 (incoming, apvts.state.getType()));
    else        apvts.replaceState (incoming);
    tlsMirrorWrite = false;

    { LinkEvent e; while (linkFifo.pop (e)) {} }   // drop any enqueues that slipped through the load
    linkFifo.overflowed.store (false, std::memory_order_relaxed);
    linkDirty.store (false, std::memory_order_relaxed);
    for (int i = 0; i < tabby::kNumBands; ++i)
        activeLaneAtom[(size_t) i].store ((int) apvts.state.getProperty (tabby::bandId (i, "activeLane"), -1), std::memory_order_relaxed);
}

teq::BandParams TabbyEqAudioProcessor::readBand (int b) const noexcept
{
    const auto& p = bands[(size_t) b];
    teq::BandParams bp;
    bp.on     = p.on->load()     > 0.5f;
    bp.type   = tabby::filterTypeFromChoice ((int) p.type->load());
    bp.swept  = p.swept->load()  > 0.5f;
    bp.bypass = p.bypass->load() > 0.5f;
    for (int L = 0; L < teq::kNumLanes; ++L)
    {
        auto& lane = bp.lanes[(size_t) L];
        const auto& src = p.lane[L];
        lane.on     = src.on->load() > 0.5f;
        lane.freq   = (double) src.freq->load();
        lane.Q      = (double) src.q->load();
        lane.gainDb = (double) src.gain->load();
        lane.slope  = kSlopeDb[juce::jlimit (0, 6, (int) src.slope->load())];
        lane.bypass = src.byp->load() > 0.5f;
    }
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
