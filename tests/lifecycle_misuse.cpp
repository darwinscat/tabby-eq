// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

// ---------------------------------------------------------------------------
// TabbyEQ adapter lifecycle / misuse harness — the tabby-side of felitronics-core's sanitizer barrier.
//
// This is NOT a DSP-correctness test (that lives upstream in felitronics-core's ctest suites, and we do
// NOT re-test the core primitives here). It drives the REAL juce::AudioProcessor — TabbyEqAudioProcessor —
// through adversarial host lifecycle orders (process before prepareToPlay, process after releaseResources,
// double prepare, mode switches, mono→stereo up-mix, short/empty buffers, hostile restored state) so that
// ASan + UBSan (the CI job that runs this) light up any use-before-prepare / OOB / divide-by-zero /
// uninitialised-read in the ADAPTER's own glue — the exact class that once crashed a sibling plugin only on
// x86-64 while staying silent on the Apple-Silicon dev machine.
//
// It runs headless: ScopedJuceInitialiser_GUI gives us a MessageManager (the processor starts a 30 Hz
// juce::Timer in its ctor) WITHOUT opening an X display (X is only touched via Desktop/peers, which we never
// create — no editor, no window, no xvfb needed). LeakSanitizer is disabled in the CI env (JUCE keeps
// process-exit global singletons by design); ASan's heap-overflow / use-after-free + all of UBSan stay on.
// ---------------------------------------------------------------------------

#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <iostream>

namespace
{
    int failures = 0;

    void check (bool ok, const char* what)
    {
        if (! ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
    }

    void fillNoise (juce::AudioBuffer<float>& b, int seed)
    {
        juce::Random r ((juce::int64) seed);
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getWritePointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i) d[i] = r.nextFloat() * 2.0f - 1.0f;
        }
    }

    bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            const auto* d = b.getReadPointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (d[i])) return false;
        }
        return true;
    }

    // Run one block of noise through the processor and assert the output stays finite (NaN/Inf would be a
    // real adapter/coeff bug). `chans`==0 exercises the empty-buffer guard (H2 / nc<=0).
    void processNoise (TabbyEqAudioProcessor& p, int chans, int n, int seed, const char* label)
    {
        juce::AudioBuffer<float> buf;
        buf.setSize (juce::jmax (0, chans), n);
        if (chans > 0) fillNoise (buf, seed);
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        if (chans > 0) check (allFinite (buf), label);
    }

    void setChoice (juce::AudioProcessorValueTreeState& s, const juce::String& id, int index)
    {
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id))) *c = index;
    }

    void setFloat (juce::AudioProcessorValueTreeState& s, const juce::String& id, float v)
    {
        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (s.getParameter (id))) *f = v;
    }

    // Let the message thread run briefly so the processor's 30 Hz LpUpdater timer actually FIRES — before
    // prepare it must early-return on !prepared (the gate), after prepare it feeds the FIR builders.
    void pumpTimers (int ms)
    {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            mm->runDispatchLoopUntil (ms);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager for the ctor timer; does NOT open X

    // -- 1. Construct; fire the timer BEFORE any prepare (lpTick must early-return on !prepared) ----------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        pumpTimers (80);   // several 30 Hz ticks with prepared==false

        // -- 2. Process BEFORE prepareToPlay in every phase mode: H1 must make it a safe dry passthrough --
        for (int mode = 0; mode <= 2; ++mode)
        {
            setChoice (p->apvts, "phaseMode", mode);
            const int n = 256;
            juce::AudioBuffer<float> buf (2, n);
            fillNoise (buf, 100 + mode);
            juce::AudioBuffer<float> before; before.makeCopyOf (buf);
            juce::MidiBuffer midi;
            p->processBlock (buf, midi);   // unprepared → early return, buffer untouched
            bool unchanged = true;
            for (int c = 0; c < 2 && unchanged; ++c)
                for (int i = 0; i < n; ++i)
                    if (! juce::exactlyEqual (buf.getReadPointer (c)[i], before.getReadPointer (c)[i])) { unchanged = false; break; }
            check (unchanged, "process-before-prepare is a dry passthrough (H1)");
        }
        setChoice (p->apvts, "phaseMode", 0);
    }

    // -- 3. Full prepare + process in each mode; -- 4. release then process; -- 5. re-prepare -----------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);
        pumpTimers (80);
        for (int mode = 0; mode <= 2; ++mode)
        {
            setChoice (p->apvts, "phaseMode", mode);
            pumpTimers (60);   // let lpTick rebuild the active FIR
            processNoise (*p, 2, 512, 200 + mode, "prepared stereo process is finite");
        }

        p->releaseResources();                       // prepared → false
        for (int mode = 0; mode <= 2; ++mode)        // process after release: H1 dry passthrough, no UAF
        {
            setChoice (p->apvts, "phaseMode", mode);
            processNoise (*p, 2, 512, 300 + mode, "process-after-release is safe");
        }

        p->prepareToPlay (44100.0, 128);             // re-prepare at a new sr/block → alive again
        processNoise (*p, 2, 128, 400, "process after re-prepare is finite");
    }

    // -- 6. Double prepareToPlay back-to-back (re-entrancy) -----------------------------------------------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->prepareToPlay (44100.0, 64);
        p->prepareToPlay (96000.0, 1024);            // second prepare without a release
        processNoise (*p, 2, 1024, 500, "process after double-prepare is finite");
    }

    // -- 7. mono→stereo up-mix (the one non-matched layout the adapter accepts) --------------------------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (1, 2, 48000.0, 256);
        p->prepareToPlay (48000.0, 256);
        processNoise (*p, 2, 256, 600, "mono→stereo up-mix is finite");   // buffer carries max(in,out)=2 ch
    }

    // -- 8. Short / empty buffers vs a stereo config (H2 buffer-channel clamp + nc<=0 guard) -------------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);
        processNoise (*p, 1, 512, 700, "under-channel buffer (1<bus) does not overrun");
        processNoise (*p, 0, 512, 701, "zero-channel buffer is a no-op");
        processNoise (*p, 2, 0,   702, "zero-sample buffer is a no-op");
    }

    // -- 9. Analyzer taps active across all three spectrum domains, stereo + mono --------------------------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);
        p->setAnalyzerActive (true);
        for (int dom = 0; dom <= 2; ++dom)
        {
            p->setSpectrumDomain (dom);
            processNoise (*p, 2, 512, 800 + dom, "analyzer domain stereo is finite");
        }
        p->setSoloBand (3);                                    // band-listen path
        processNoise (*p, 2, 512, 810, "solo band-listen is finite");
        p->setSoloBand (-1);
        p->setAudition (true, 3200.0f, 8.0f);                  // drag-audition path
        processNoise (*p, 2, 512, 811, "drag-audition is finite");
        p->setAudition (false);
        p->setAnalyzerActive (false);
    }

    // -- 10. Extreme-but-valid params: all 24 bands on, ±24 dB, every type/slope, cycling modes ----------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 256);
        p->prepareToPlay (48000.0, 256);
        for (int b = 0; b < tabby::kNumBands; ++b)
        {
            setChoice (p->apvts, tabby::bandId (b, "on"),   1);
            setChoice (p->apvts, tabby::bandId (b, "type"), b % 9);        // Bell..Tilt (shared point type)
            setChoice (p->apvts, tabby::laneParamId (b, 0, "slope"), b % 7);   // ST lane: 6..96 dB/oct
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "freq"), 20.0f + (float) b * 800.0f);
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "gain"), (b % 2 == 0 ? 24.0f : -24.0f));
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "q"),    (b % 2 == 0 ? 40.0f : 0.05f));
            if (b % 3 == 0)   // exercise the split (M/S delta-fold) path on a third of the bands
            {
                setChoice (p->apvts, tabby::laneParamId (b, 0, "on"), 0);       // ST off
                setChoice (p->apvts, tabby::laneParamId (b, 3, "on"), 1);       // Mid on
                setChoice (p->apvts, tabby::laneParamId (b, 4, "on"), 1);       // Side on
                setFloat  (p->apvts, tabby::laneParamId (b, 3, "gain"), 6.0f);
                setFloat  (p->apvts, tabby::laneParamId (b, 4, "gain"), -6.0f);
                setFloat  (p->apvts, tabby::laneParamId (b, 4, "freq"), 20.0f + (float) b * 850.0f);
            }
        }
        for (int mode = 0; mode <= 2; ++mode)
        {
            setChoice (p->apvts, "phaseMode", mode);
            pumpTimers (60);
            processNoise (*p, 2, 256, 900 + mode, "extreme all-bands-on process is finite");
        }
    }

    // -- 11. State round-trip + hostile out-of-range restored choices (APVTS must clamp) -----------------
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 256);
        p->prepareToPlay (48000.0, 256);

        juce::MemoryBlock mb;
        p->getStateInformation (mb);
        p->setStateInformation (mb.getData(), (int) mb.getSize());   // clean round-trip

        // Inject deliberately out-of-range indices for the choice params that later index fixed C arrays
        // (phaseMode→FIR sizes[], lpQuality→FIR length, slope→kSlopeDb[]). APVTS is expected to clamp these
        // on replaceState; if it does NOT, UBSan/ASan will catch the OOB downstream — a real adapter gap.
        auto state = p->apvts.copyState();
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            auto child = state.getChild (i);
            const auto id = child.getProperty ("id").toString();
            if (id == "phaseMode" || id == "lpQuality"
                || id.endsWith ("_slope") || id.endsWith ("_type"))   // per-lane slopes + the shared point type
                child.setProperty ("value", 999.0, nullptr);
        }
        p->apvts.replaceState (state);

        const float pm = p->apvts.getRawParameterValue ("phaseMode")->load();
        const float lq = p->apvts.getRawParameterValue ("lpQuality")->load();
        check (pm >= 0.0f && pm <= 2.0f, "restored out-of-range phaseMode is clamped to [0,2]");
        check (lq >= 0.0f && lq <= 4.0f, "restored out-of-range lpQuality is clamped to [0,4]");

        p->prepareToPlay (48000.0, 256);   // re-prepare picks up the (clamped) restored quality
        pumpTimers (60);
        for (int mode = 0; mode <= 2; ++mode)
        {
            setChoice (p->apvts, "phaseMode", mode);
            pumpTimers (60);
            processNoise (*p, 2, 256, 1000 + mode, "process after hostile-state restore is finite");
        }
    }

    // -- 12. setStateInformation BEFORE prepareToPlay (the classic host load order) ----------------------
    {
        auto donor = std::make_unique<TabbyEqAudioProcessor>();
        setChoice (donor->apvts, "phaseMode", 2);
        setFloat  (donor->apvts, tabby::bandId (0, "gain"), 12.0f);
        juce::MemoryBlock mb;
        donor->getStateInformation (mb);

        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setStateInformation (mb.getData(), (int) mb.getSize());   // state loaded on an unprepared processor
        pumpTimers (40);                                             // timer fires with restored state, !prepared
        processNoise (*p, 2, 256, 1100, "process before prepare after state-load is a passthrough");
        p->setPlayConfigDetails (2, 2, 48000.0, 256);
        p->prepareToPlay (48000.0, 256);
        pumpTimers (60);
        processNoise (*p, 2, 256, 1101, "process after prepare with pre-loaded state is finite");
    }

    // -- 13. Construct / destruct churn (dangling timer / thread teardown) -------------------------------
    for (int i = 0; i < 4; ++i)
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->prepareToPlay (48000.0, 128);
        processNoise (*p, 2, 128, 1200 + i, "churn cycle process is finite");
        p->releaseResources();
    }

    if (failures == 0) std::cout << "TabbyEQ lifecycle/misuse: all checks passed\n";
    else               std::cerr << "TabbyEQ lifecycle/misuse: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
