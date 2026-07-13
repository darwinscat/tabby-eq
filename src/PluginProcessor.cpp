// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/VersionInfo.h"   // tabby::currentDescribe() — the running build's kDescribe stamp

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
                // Fission only when the Side lane is actually AUDIBLE (live point + side enabled): fissioning
                // a disabled side lane would burn a free slot on an on-but-all-lanes-off husk of a point.
                // A disabled side migrates in place (values kept, sType silently dropped — inaudible).
                const int  freeT = (live && sOn && sType != mig[(size_t) b].type) ? nextFree() : -1;
                if (sType == mig[(size_t) b].type || freeT < 0)
                {
                    mig[(size_t) b].lane[4] = { sOn, sFreq, sQ, sGain, sSlope, sByp };   // Side on band b
                    if (live && sOn && sType != mig[(size_t) b].type) migrationNote = true;   // pool full on an AUDIBLE side -> noted coercion
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
      apvts (*this, nullptr, "PARAMS", tabby::createParameterLayout()),
      updateCheckerInstance (tabby::currentDescribe(), *appPreferencesInstance),   // compares kDescribe → latest release (prefs shared process-wide)
      // The shared appkit undo/redo + A/B/C/D engine (PerRegister mode). The opaque seam is the whole
      // APVTS state tree: captureLive()->copyState and applyLive()->applyLiveState. Initialized after
      // apvts (declaration order) — the engine's capture-stability assert captures at construction.
      // copyState() (not state.createCopy()!) is load-bearing: it flushes the param->tree sync under
      // its lock, so a capture is always converged — endGesture at mouse-up misses no drag writes.
      // Config: 4 registers; maxUndo 32 (a tabby snapshot is an 820-param tree — hundreds of KB in
      // memory; 32 halves orbitcab's worst case with no UX loss); settleTicks 4 at the editor's
      // 10 Hz pump = the same ~0.4 s settle window as orbitcab's 12 @ 30 Hz, at a third of the
      // full-tree capture cost per open-editor second.
      history (felitronics::appkit::CompareHistory::Mode::PerRegister,
               [this] { return apvts.copyState(); },
               [this] (const juce::ValueTree& t) { applyLiveState (t); },
               felitronics::appkit::CompareHistory::Config { kNumSnapshots, 32, 4 })
{
    // Every history mutation (commit / undo / redo / switch / copy / clear / load) bumps an atomic
    // revision the editor polls (historyRevision()) to refresh its undo/redo + A/B/C/D affordances.
    history.onHistoryChanged = [this] { historyRev.fetch_add (1, std::memory_order_relaxed); };

    // Any APPLY (undo / redo / switch / copy / load) replaced the live state wholesale — the editor
    // polls applyRevision() to re-sync everything it mirrors from the state tree (view properties,
    // lane caches). Read-only w.r.t. the live state (engine contract 2): it only bumps an atomic.
    history.onAfterApply = [this] (felitronics::appkit::CompareHistory::Reason)
    { applyRev.fetch_add (1, std::memory_order_relaxed); };

    // A foreign session carrying a different register count is diagnostic-only: the engine already
    // skipped the out-of-range snapshots (skip-not-clamp), and this build's count is fixed.
    history.onRegisterCountMismatch = [] (int savedCount, int buildCount)
    {
        juce::ignoreUnused (savedCount, buildCount);
        DBG ("TabbyEQ: session carries " << savedCount << " compare registers, build has "
             << buildCount << " — extras dropped");
    };

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
    activeLaneAtom[(size_t) band].store (L, std::memory_order_relaxed);   // derived mirror — never capture-visible

    // The PROPERTY write is capture-visible; skip it whenever it would not change the tree: an
    // exact echo of the RAW property (the editor re-fires its lane binding on every history
    // re-sync — compare the raw value, NOT the enabled-clamped resolution: even a same-value
    // write's suppress scope would flush a pending edit burst), or BIRTHING the property with the
    // value its absence already resolves to (first node click on a fresh band). Either would
    // count as a suppressed "forward move" and clear a freshly-built redo (crew P1).
    const int prop = (int) apvts.state.getProperty (tabby::bandId (band, "activeLane"), -1);
    if (prop == L || (prop < 0 && resyncActiveLane (band) == L))
        return;
    // Suppressed: selecting a lane TAB is view state, not an edit — unsuppressed, every tab click
    // would settle into a junk "Parameter Change" undo step. The property still rides the snapshot
    // (saves, registers), it just records no step. Engine caveat applies: as a suppressed forward
    // move a REAL lane change invalidates a stale redo — the cost of view props living in the
    // opaque snapshot (D1).
    const felitronics::appkit::CompareHistory::ScopedSuppress ss (history);
    apvts.state.setProperty (tabby::bandId (band, "activeLane"), L, nullptr);
}

// Reset every parameter to its default — suppressed (no undo step, per the phase decision). The
// link-FIFO is drained INSIDE the scope on both sides: mirror events queued BEFORE the reset must
// not replay stale pre-reset values after the scope closes (junk step + non-default linked lanes),
// and the reset's own enqueues must not settle post-scope either.
void TabbyEqAudioProcessor::resetAllToDefaults()
{
    const felitronics::appkit::CompareHistory::ScopedSuppress ss (history);
    drainLinkFifo();
    for (auto* p : getParameters())
        p->setValueNotifyingHost (p->getDefaultValue());
    drainLinkFifo();
}

//==============================================================================
juce::File TabbyEqAudioProcessor::presetDirectory()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                         .getChildFile ("Darwin's Cat").getChildFile ("TabbyEQ").getChildFile ("Presets");
    dir.createDirectory();
    return dir;
}

bool TabbyEqAudioProcessor::saveStateFile (const juce::File& f)
{
    // A preset carries the LIVE sound only (flat v3 tree) — portable across sessions and machines;
    // the compare workspace (A/B/C/D) is session state and stays out.
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", 3, nullptr);
    if (auto xml = state.createXml())
        return f.replaceWithText (xml->toString());
    return false;
}

bool TabbyEqAudioProcessor::loadStateFile (const juce::File& f)
{
    // Presets are FLAT live-state trees only — a <Workspace> session dump is NOT a preset (it
    // would smuggle four compare registers through a path labelled "preset", and its engine-side
    // rejection would be invisible here). The flat route through setStateInformation keeps every
    // validation/migration rule identical to host blobs, and cannot be rejected past this check.
    const auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    juce::MemoryBlock mb;
    copyXmlToBinary (*xml, mb);
    setStateInformation (mb.getData(), (int) mb.getSize());
    return true;
}

//==============================================================================
// The CompareHistory applyLive seam AND the shared load routine: replace the APVTS state from a
// snapshot, then re-sync everything derived from it. Runs on every apply (undo / redo / register
// switch / copy / session load). Deep-copies the incoming tree so the live tree never aliases an
// engine-stored snapshot (a later live edit would silently mutate the stored baseline — contract 1).
void TabbyEqAudioProcessor::applyLiveState (const juce::ValueTree& t)
{
    // Only a TabbyEQ state tree may become the live state. A hostile v4 blob can carry a valid
    // <Workspace> envelope with a garbage <Live> payload — the engine validates the ENVELOPE, the
    // payload type is the consumer's to check (rejecting here degrades to "registers load, live
    // keeps the prior sound"; the engine reseeds its baseline from whatever we kept — tolerated).
    if (! t.hasType (apvts.state.getType()))
        return;

    // Suppress link-mirror enqueues for the apply's own param writes; drop any that slip through afterwards.
    tlsMirrorWrite = true;
    apvts.replaceState (t.createCopy());
    tlsMirrorWrite = false;

    // Clear the flags BEFORE draining (acquire-exchange for parity with drainLinkFifo on weak
    // memory): an event racing this window is either popped here (dropped — it references the
    // PRE-apply state, same by-design semantics as the load path always had) or lands after the
    // pop with its producer having re-armed linkDirty (survives for the 30 Hz drain — provably so:
    // surviving the pop implies the push, and thus the later flag store, ordered after our clear).
    // The reverse order stranded an event behind a cleared flag for a later STALE replay. Worst
    // case now is one harmless spurious drain wake-up.
    (void) linkDirty.exchange (false, std::memory_order_acquire);
    (void) linkFifo.overflowed.exchange (false, std::memory_order_acquire);
    { LinkEvent e; while (linkFifo.pop (e)) {} }   // drop enqueues that slipped through the apply itself

    // Re-sync every processor-side mirror of a state-tree property (the snapshot is authoritative).
    for (int i = 0; i < tabby::kNumBands; ++i)
        activeLaneAtom[(size_t) i].store ((int) apvts.state.getProperty (tabby::bandId (i, "activeLane"), -1), std::memory_order_relaxed);
    spectrumDomain.store ((int) apvts.state.getProperty ("specDomain", 0), std::memory_order_relaxed);   // analyzer Stereo/Mid/Side (see PluginEditor)
}

void TabbyEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // v4: the session root is the CompareHistory <Workspace> envelope (live + A/B/C/D registers +
    // active index); the inner state trees keep the v3 lane format, so stateVersion rides on the
    // envelope root. toTree() is pure capture — a host autosave poll must never mutate history
    // (no flush here: an unsettled edit burst simply saves as part of the current live).
    auto ws = history.toTree();
    ws.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = ws.createXml())
        copyXmlToBinary (*xml, destData);
}

void TabbyEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    if (xml->hasTagName ("Workspace"))
    {
        // v4+: the CompareHistory envelope. The engine validates BEFORE mutating: false = corrupt
        // envelope (nothing applied, the prior state is kept) or a newer schema (best-effort load
        // still applied). Neither is a programming error on a host-supplied blob. The engine calls
        // applyLiveState() for the live payload, which does the param/link/lane resync.
        // NO markSaved() here: fromTree() re-baselines the saved marker itself on every load it
        // applies — an unconditional markSaved() would falsely CLEAN a dirty session whose load
        // was rejected (the user would then not be prompted to save real edits).
        if (! history.fromTree (juce::ValueTree::fromXml (*xml)))
            DBG ("TabbyEQ: <Workspace> envelope rejected (corrupt) or newer schema (best-effort)");
        return;
    }

    if (! xml->hasTagName (apvts.state.getType()))
        return;

    // Legacy v2/v3 session: a flat state tree with no compare workspace. It becomes the live state
    // of register A with B/C/D empty and a fresh, clean history — reset() clears registers/history
    // but leaves live untouched, so apply the (migrated) state first.
    juce::ValueTree incoming = juce::ValueTree::fromXml (*xml);
    const int ver = (int) incoming.getProperty ("stateVersion", 2);   // a v2 XML without stateVersion counts as v2
    applyLiveState (ver <= 2 ? migrateV2toV3 (incoming, apvts.state.getType()) : incoming);
    history.reset();
    history.markSaved();
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
