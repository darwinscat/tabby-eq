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
#include <cstdio>
#include <iostream>
#include <vector>

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

    // Bool params (band/lane on, bypass) need their OWN setter: setChoice's dynamic_cast to
    // AudioParameterChoice fails silently on an AudioParameterBool, so the write never happens.
    void setBool (juce::AudioProcessorValueTreeState& s, const juce::String& id, bool v)
    {
        if (auto* b = dynamic_cast<juce::AudioParameterBool*> (s.getParameter (id))) *b = v;
    }

    // A centred 0 dB as Apple Silicon reads it back. JUCE snaps a parameter as start + interval * floor (...), and
    // clang fuses that into one FMA where the ISA has one, so on -24..24 dB with a 0.01 step the host's "0" lands
    // on -5.36e-7 dB there and on exactly 0 on x86-64 (see centredZeroDb in PluginProcessor.cpp). The tests that
    // pin "0 means 0" write the host's 0 and then put THIS value in the parameter's raw store, so every platform
    // runs the case the FMA row produces — and a build without the snap fails on every platform, not just one.
    constexpr float kFmaCentredZeroDb = -5.36441803e-07f;
    void setCentredZero (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        setFloat (s, id, 0.0f);                                    // the host's "0"...
        s.getRawParameterValue (id)->store (kFmaCentredZeroDb);    // ...as the FMA snap leaves it
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
            setBool   (p->apvts, tabby::bandId (b, "on"),   true);          // Bool param — setChoice would silently no-op
            setChoice (p->apvts, tabby::bandId (b, "type"), b % 9);        // Bell..Tilt (shared point type)
            setChoice (p->apvts, tabby::laneParamId (b, 0, "slope"), b % 7);   // ST lane: 6..96 dB/oct
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "freq"), 20.0f + (float) b * 800.0f);
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "gain"), (b % 2 == 0 ? 24.0f : -24.0f));
            setFloat  (p->apvts, tabby::laneParamId (b, 0, "q"),    (b % 2 == 0 ? 40.0f : 0.05f));
            if (b % 3 == 0)   // exercise the split (M/S delta-fold) path on a third of the bands
            {
                setBool (p->apvts, tabby::laneParamId (b, 0, "on"), false);     // ST off
                setBool (p->apvts, tabby::laneParamId (b, 3, "on"), true);      // Mid on
                setBool (p->apvts, tabby::laneParamId (b, 4, "on"), true);      // Side on
                setFloat (p->apvts, tabby::laneParamId (b, 3, "gain"), 6.0f);
                setFloat (p->apvts, tabby::laneParamId (b, 4, "gain"), -6.0f);
                setFloat (p->apvts, tabby::laneParamId (b, 4, "freq"), 20.0f + (float) b * 850.0f);
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
        setFloat  (donor->apvts, tabby::laneParamId (0, 0, "gain"), 12.0f);   // v3 id (ST lane) — band0_gain is a dead v2 id
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

    // -- 14. Point-level dynamics through the real audio path -------------------------------------------
    // The adapter's job here is narrow and load-bearing: dynamics must be INERT unless a point asks for
    // it, must actually engage when it does, and must never resume a stale duck after a block in which
    // the dynamic path did not run. The threshold is driven MANUALLY throughout: the auto-relative mode
    // deliberately lets a permanently loud band become "the norm" (DYNAMICS.md § 11), which is correct
    // behaviour but useless as a fixed reference point for a test.
    {
        const double fs = 48000.0;
        const int    n  = 512;
        const double f0 = 1000.0;
        double phase = 0.0;   // continuous across every run below (a steady tone, not a retrigger)

        auto tone = [fs, f0, &phase] (juce::AudioBuffer<float>& b, float amp)
        {
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float s = amp * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * f0 / fs;
                for (int c = 0; c < b.getNumChannels(); ++c) b.getWritePointer (c)[i] = s;
            }
        };
        // One static +12 dB bell at f0 on the ST lane — the point every case below starts from.
        auto makeBand = [] (TabbyEqAudioProcessor& p, int b = 0, float gainDb = 12.0f)
        {
            setBool   (p.apvts, tabby::bandId (b, "on"), true);
            setChoice (p.apvts, tabby::bandId (b, "type"), 0);                       // Bell
            setBool   (p.apvts, tabby::laneParamId (b, 0, "on"), true);
            setFloat  (p.apvts, tabby::laneParamId (b, 0, "freq"), 1000.0f);
            setFloat  (p.apvts, tabby::laneParamId (b, 0, "q"), 1.0f);
            setFloat  (p.apvts, tabby::laneParamId (b, 0, "gain"), gainDb);
        };
        // A manual (non-adaptive) duck: -18 dB of range, threshold far below the programme.
        // Dynamics is an opt-in preview (View menu, default OFF), so arming a point also means turning
        // the feature on — every case below is about what the dynamic path DOES once it runs. The
        // switch's own contract (armed + disabled == static) is pinned separately, right after (a).
        auto armDynamics = [] (TabbyEqAudioProcessor& p, float rangeDb, int b = 0)
        {
            p.setDynamicsEnabled (true);
            setBool  (p.apvts, tabby::bandId (b, "dyn_on"), true);
            setBool  (p.apvts, tabby::bandId (b, "dyn_auto"), false);
            setFloat (p.apvts, tabby::bandId (b, "dyn_thr"), -30.0f);   // loud tone (-6 dBFS) engages; the quiet resume (-46) does not
            setFloat (p.apvts, tabby::bandId (b, "dyn_range"), rangeDb);
        };
        // Run `blocks` tone blocks of `len` samples; the last one is left in `out`.
        auto runTone = [&] (TabbyEqAudioProcessor& p, int blocks, int len, juce::AudioBuffer<float>& out)
        {
            juce::AudioBuffer<float> buf (2, len);
            juce::MidiBuffer midi;
            for (int i = 0; i < blocks; ++i)
            {
                tone (buf, 0.5f);
                p.processBlock (buf, midi);
            }
            out.makeCopyOf (buf);
        };
        // The same tone at -46 dBFS: far under the manual threshold below, so a healthy point does not
        // engage on it at all — which is exactly what makes a leftover duck visible.
        auto runQuiet = [&] (TabbyEqAudioProcessor& p, int len, juce::AudioBuffer<float>& out)
        {
            juce::AudioBuffer<float> buf (2, len);
            juce::MidiBuffer midi;
            tone (buf, 0.005f);
            p.processBlock (buf, midi);
            out.makeCopyOf (buf);
        };
        // Peak over a window of >= 1 full cycle == the tone's current amplitude, whatever the window
        // length — which is what lets the short resumed block below be compared against a 512 reference.
        auto peak = [] (const juce::AudioBuffer<float>& b) { return (double) b.getMagnitude (0, b.getNumSamples()); };
        auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            if (a.getNumSamples() != b.getNumSamples()) return false;
            for (int c = 0; c < a.getNumChannels(); ++c)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    if (! juce::exactlyEqual (a.getReadPointer (c)[i], b.getReadPointer (c)[i])) return false;
            return true;
        };

        // (a) INERT when off — bit-identical output, not merely "close". A non-zero range with dyn_on
        // false must not move a sample, and neither must dyn_on with range 0 ("no dynamics" is a
        // documented disengage, not a target of zero).
        juce::AudioBuffer<float> ref;
        double refQuiet = 0.0;
        {
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            runTone (*p, 8, n, ref);
            check (peak (ref) > 0.0, "dyn: reference static band produces signal");
            juce::AudioBuffer<float> quiet;
            runQuiet (*p, 128, quiet);          // the same static band's answer to the quiet resume signal
            refQuiet = peak (quiet);
        }
        {
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            setFloat (p->apvts, tabby::bandId (0, "dyn_range"), -18.0f);   // armed but OFF
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> got;
            runTone (*p, 8, n, got);
            check (identical (got, ref), "dyn: range with dyn_on false is bit-identical to a static point");
        }
        {
            // Range 0 is a documented disengage, and it is a TRUE one: the adapter reads a range within half a
            // step of 0 as exactly 0 (the FMA snap left -5.36e-7 dB on Apple Silicon, forced here on every
            // platform), so the point never engages, and the band's delta section runs at 0 dB — which is
            // exact: the bell's mix term k·(A²−1) is 0 at A = 1, the output is the input. This check used to
            // allow "one ULP" and blame that section; the ULP was the snap's -5.36e-7 dB range, a live duck of
            // that size. Bit-identical to a static point now, on every platform.
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            armDynamics (*p, 0.0f);                                        // ON but zero range
            setCentredZero (p->apvts, tabby::bandId (0, "dyn_range"));
            check (juce::exactlyEqual (p->readBand (0).dyn.rangeDb, 0.0), "dyn: a centred-0 range reads as exactly 0");
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> got;
            runTone (*p, 8, n, got);
            check (identical (got, ref), "dyn: dyn_on with range 0 is bit-identical to a static point, on every platform");
            check (juce::exactlyEqual (p->dynamicDeltaDb (0, 0), 0.0f), "dyn: ...and publishes no gain reduction");
        }

        // (a2) The FEATURE SWITCH. Dynamics ships OFF (View menu opt-in), and off has to mean the same
        // thing as "this point is not dynamic" — not a quieter duck, not a delta section running at
        // unity. A point armed to its full range with the switch off must therefore be BIT-identical
        // to the static reference, and publish nothing to the GR meter.
        {
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            setBool  (p->apvts, tabby::bandId (0, "dyn_on"), true);
            setBool  (p->apvts, tabby::bandId (0, "dyn_auto"), false);
            setFloat (p->apvts, tabby::bandId (0, "dyn_thr"), -30.0f);
            setFloat (p->apvts, tabby::bandId (0, "dyn_range"), -18.0f);   // fully armed...
            check (! p->dynamicsEnabled(), "dyn: the preview switch defaults OFF");
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> got;
            runTone (*p, 8, n, got);       // the SAME block count as the reference — the tone's phase must line up
            check (identical (got, ref), "dyn: a fully armed point with the preview OFF is bit-identical to static");
            check (juce::exactlyEqual (p->dynamicDeltaDb (0, 0), 0.0f), "dyn: ...and publishes no gain reduction");

            // And the switch is live: the same instance, mid-stream, starts ducking once enabled.
            p->setDynamicsEnabled (true);
            juce::AudioBuffer<float> engaged;
            runTone (*p, 40, n, engaged);
            check (peak (engaged) < peak (ref) * 0.5, "dyn: enabling the preview mid-stream engages the point");
        }

        // (b) ENGAGES when asked, and (c) RELEASES when the dynamic path stops running. Same processor:
        // the released state is only meaningful measured against this instance's own ducked level.
        {
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            armDynamics (*p, -18.0f);
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;

            juce::AudioBuffer<float> engaged;
            runTone (*p, 40, n, engaged);
            check (peak (engaged) < peak (ref) * 0.5, "dyn: an engaged point pulls the band down");
            check (peak (engaged) > 0.0,              "dyn: ...without killing the signal");

            // The release edge. LaneDynamics computes each chunk's delta AFTER processing it, so a seam
            // left armed would re-apply the full duck to the FIRST sample back — seconds stale after a
            // long solo. Resuming on QUIET material (well under the threshold) is what makes the two
            // outcomes separable: a released seam stays at unity for the whole block, while a stale one
            // opens ducked and crawls back at the release time constant. Re-attack cannot mask it,
            // because nothing here asks the detector to engage at all.
            juce::AudioBuffer<float> spill;
            p->setAudition (true, 1000.0f, 6.0f);
            runTone (*p, 1, n, spill);                 // one block off the dynamic path
            p->setAudition (false);

            juce::AudioBuffer<float> resumed;
            runQuiet (*p, 128, resumed);               // FIRST block back on the EQ path, below threshold
            check (peak (resumed) > refQuiet * 0.7,    "dyn: the first block after audition opens unducked (release edge)");
            check (peak (resumed) > peak (engaged) / peak (ref) * refQuiet * 1.5,
                                                       "dyn: ...measurably above the level it was ducking to");

            // Same edge through the OTHER long-lived monitor state: band-listen. A solo can sit engaged
            // for seconds, which is exactly how long a leftover duck would be waiting on the way out.
            juce::AudioBuffer<float> tmp;
            runTone (*p, 20, n, tmp);                  // re-engage
            p->setSoloBand (0);
            runTone (*p, 1, n, tmp);
            p->setSoloBand (-1);
            juce::AudioBuffer<float> afterSolo;
            runQuiet (*p, 128, afterSolo);
            check (peak (afterSolo) > refQuiet * 0.7,  "dyn: the first block after solo opens unducked (release edge)");
        }

        // (e) The published GR (DYNAMICS.md § 5) — the number the meter and later the Helper read. It
        // is checked against the AUDIBLE duck, not against itself: a number that agrees with the ears
        // cannot be reading the wrong lane, the wrong band, or a stale block.
        {
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p, 0, 0.0f);
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> flatRef;
            runTone (*p, 8, n, flatRef);
            check (juce::exactlyEqual (p->dynamicDeltaDb (0, 0), 0.0f), "gr: a static point publishes no reduction");

            auto q = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*q, 0, 0.0f);
            armDynamics (*q, -18.0f, 0);
            q->setPlayConfigDetails (2, 2, fs, n);
            q->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> engaged;
            runTone (*q, 40, n, engaged);

            const double published = q->dynamicDeltaDb (0, 0);
            const double audible   = 20.0 * std::log10 (peak (engaged) / peak (flatRef));
            check (published < -1.0,                        "gr: an engaged point publishes a NEGATIVE delta (a duck, signed)");
            check (std::abs (published - audible) < 1.0,    "gr: the published delta matches the audible one within 1 dB");
            check (juce::exactlyEqual (q->dynamicDeltaDb (0, 3), 0.0f), "gr: an idle lane of the same point publishes nothing");
            check (juce::exactlyEqual (q->dynamicDeltaDb (1, 0), 0.0f), "gr: a neighbouring point publishes nothing");

            // Out of range is a read, not a crash: the editor asks per node, and nodes come and go.
            check (juce::exactlyEqual (q->dynamicDeltaDb (-1, 0), 0.0f), "gr: negative band index reads 0");
            check (juce::exactlyEqual (q->dynamicDeltaDb (tabby::kNumBands, 0), 0.0f), "gr: past-the-end band index reads 0");
            check (juce::exactlyEqual (q->dynamicDeltaDb (0, teq::kNumLanes), 0.0f),   "gr: past-the-end lane index reads 0");

            // The meter must not outlive the reduction: the release edge zeroes it with the seam.
            q->setAudition (true, 1000.0f, 6.0f);
            juce::AudioBuffer<float> spill;
            runTone (*q, 1, n, spill);
            check (juce::exactlyEqual (q->dynamicDeltaDb (0, 0), 0.0f), "gr: the release edge clears the published delta too");

            // And it lands on the LANE that is actually running — here Mid, with Stereo switched off.
            auto m = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*m, 0, 0.0f);
            setBool  (m->apvts, tabby::laneParamId (0, 0, "on"), false);   // ST off
            setBool  (m->apvts, tabby::laneParamId (0, 3, "on"), true);    // Mid on
            setFloat (m->apvts, tabby::laneParamId (0, 3, "freq"), 1000.0f);
            setFloat (m->apvts, tabby::laneParamId (0, 3, "q"), 1.0f);
            armDynamics (*m, -18.0f, 0);
            m->setPlayConfigDetails (2, 2, fs, n);
            m->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> mid;
            runTone (*m, 40, n, mid);
            check (m->dynamicDeltaDb (0, 3) < -1.0f,                      "gr: a Mid-only point publishes on the Mid lane");
            check (juce::exactlyEqual (m->dynamicDeltaDb (0, 0), 0.0f),   "gr: ...and not on the Stereo lane it does not use");
        }

        // (d) The detectors must probe the SECTION INPUT, not each point's own input. Two identical
        // dynamic points in series prove it: fed the untouched section input, both see the same
        // full-level programme and both duck their whole -18 dB range (-36 dB total). A chain that
        // detected on its own input would hand point 2 an already-ducked signal, it would earn far less
        // reduction, and the pair would land tens of dB high — the pumping failure mode in miniature.
        {
            auto flat = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*flat, 0, 0.0f);
            makeBand (*flat, 1, 0.0f);
            flat->setPlayConfigDetails (2, 2, fs, n);
            flat->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> flatRef;
            runTone (*flat, 8, n, flatRef);

            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p, 0, 0.0f);
            makeBand (*p, 1, 0.0f);
            armDynamics (*p, -18.0f, 0);
            armDynamics (*p, -18.0f, 1);
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> chained;
            runTone (*p, 40, n, chained);

            // Measured: -31.6 dB (0.0264) — twice the -15.8 dB each point earns from an RMS detector
            // reading -9 dBFS against a -30 dBFS threshold at ratio 4. A chain detecting on its own
            // input lands near -20 dB (0.10), because point 2 only ever sees point 1's leftovers.
            check (peak (chained) < peak (flatRef) * 0.04,
                   "dyn: two chained points each earn their full range (detector sees the section input)");
            check (peak (chained) > 0.0, "dyn: ...and the chain still passes signal");
        }

        // (f) RELEASE ON DISENGAGE (core v0.53.0; the adapter opts in). A point switched off while it ducks
        // used to have its delta zeroed on the spot — a step in the band's gain, a click. It now releases
        // through its own ballistics. Driven through both edges that disengage a point without removing it —
        // dyn_on off, and the preview switch off — each on the ONLY dynamic point, which is the case where the
        // static-path gate (no point is dynamic -> engine.process()) would have cut the release off. The
        // instrument is max|Δ²y| of the output around the edge, on a low tone whose own Δ² is small, held
        // against the SAME duck dropped at once: bypassing the point, a hard step of the whole band by design
        // (the core does not release it) — which is also the precondition that the instrument sees a snap at
        // all. A static twin fed the same tone gives the hand-back: once the published delta has landed on 0
        // the output is the static point's, bit for bit.
        //
        // Range to 0 is the third disengage edge, on every platform: the adapter reads a range within half a step
        // of 0 as exactly 0, so the FMA snap's -5.36e-7 dB (Apple Silicon; forced here everywhere) disengages
        // exactly as x86-64's clean 0 does — it releases, lands and hands back like the other two. (Before the
        // snap, on Apple Silicon it stayed engaged at a sub-micro-dB cap and never went static.)
        {
            constexpr int    blk   = 256;
            constexpr double fTone = 220.0;
            constexpr int    kLead = 200;    // blocks of ducking before the edge (~1.07 s)
            constexpr int    kTail = 600;    // blocks after it (~3.2 s) — the release and the hand-back

            // A 0 dB bell ON the tone: the static curve is flat, so the output is the tone times the duck.
            auto setUp = [&] (TabbyEqAudioProcessor& p, bool dynamic)
            {
                makeBand (p, 0, 0.0f);
                setFloat (p.apvts, tabby::laneParamId (0, 0, "freq"), (float) fTone);
                if (dynamic) armDynamics (p, -18.0f, 0);
                p.setPlayConfigDetails (2, 2, fs, blk);
                p.prepareToPlay (fs, blk);
            };
            struct Run { std::vector<float> y; std::vector<float> delta; };   // ch0 output, published delta per block
            auto render = [&] (std::vector<TabbyEqAudioProcessor*> ps, std::vector<Run>& runs, int blocks, double& ph)
            {
                juce::AudioBuffer<float> buf (2, blk);
                juce::MidiBuffer midi;
                for (int k = 0; k < blocks; ++k)
                {
                    float in[blk];
                    for (int i = 0; i < blk; ++i)
                    {
                        in[i] = 0.5f * (float) std::sin (ph);
                        ph += juce::MathConstants<double>::twoPi * fTone / fs;
                    }
                    for (size_t j = 0; j < ps.size(); ++j)
                    {
                        for (int c = 0; c < 2; ++c) std::copy (in, in + blk, buf.getWritePointer (c));
                        ps[j]->processBlock (buf, midi);
                        runs[j].y.insert (runs[j].y.end(), buf.getReadPointer (0), buf.getReadPointer (0) + blk);
                        runs[j].delta.push_back (ps[j]->dynamicDeltaDb (0, 0));
                    }
                }
            };
            auto maxD2 = [] (const std::vector<float>& y, size_t a, size_t b)
            {
                double m = 0.0;
                for (size_t i = juce::jmax (a, (size_t) 2); i < b && i < y.size(); ++i)
                    m = juce::jmax (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
                return m;
            };
            auto dBFS = [] (double v) { return 20.0 * std::log10 (juce::jmax (v, 1.0e-12)); };

            const char* edgeName[] = { "dyn_on off", "the preview switch off", "range to 0" };
            for (int edge = 0; edge < 3; ++edge)
            {
                auto rel   = std::make_unique<TabbyEqAudioProcessor>();   // the edge under test
                auto hard  = std::make_unique<TabbyEqAudioProcessor>();   // the same duck dropped at once (bypass)
                auto still = std::make_unique<TabbyEqAudioProcessor>();   // never dynamic
                setUp (*rel, true); setUp (*hard, true); setUp (*still, false);
                std::vector<Run> runs (3);
                double ph = 0.0;
                render ({ rel.get(), hard.get(), still.get() }, runs, kLead, ph);

                const float ducked = runs[0].delta.back();
                const juce::String tag = juce::String ("dyn release (") + edgeName[edge] + "): ";
                check (ducked < -6.0f, (tag + "PRECONDITION: the point is ducking before the edge").toRawUTF8());

                if      (edge == 0) setBool  (rel->apvts, tabby::bandId (0, "dyn_on"), false);
                else if (edge == 1) rel->setDynamicsEnabled (false);
                else                setCentredZero (rel->apvts, tabby::bandId (0, "dyn_range"));
                setBool (hard->apvts, tabby::bandId (0, "bypass"), true);
                check (edge < 2 || juce::exactlyEqual (rel->readBand (0).dyn.rangeDb, 0.0),
                       (tag + "the FMA snap's -5.36e-7 dB reads as exactly 0").toRawUTF8());
                render ({ rel.get(), hard.get(), still.get() }, runs, kTail, ph);

                // The gain trajectory: still ducked a block after the edge and rising monotonically — where the
                // hard step it replaces reads 0 on the very first block.
                const auto& d = runs[0].delta;
                check (juce::exactlyEqual (runs[1].delta[(size_t) kLead], 0.0f),
                       (tag + "PRECONDITION: the bypassed twin drops its duck at once").toRawUTF8());
                check (d[(size_t) kLead] < 0.5f * ducked,
                       (tag + "a block after the edge the point still holds most of its duck: it releases, it does not snap").toRawUTF8());
                bool monotone = true;
                int  landed   = -1;
                for (size_t k = (size_t) kLead; k < d.size(); ++k)
                {
                    monotone = monotone && d[k] >= d[k - 1] && d[k] <= 0.0f;
                    if (landed < 0 && juce::exactlyEqual (d[k], 0.0f)) landed = (int) k;
                }
                check (monotone, (tag + "the published delta rises monotonically toward 0 dB").toRawUTF8());

                // The step. The window is the edge block plus the next 15 (~85 ms): where a snap happens. What
                // the release leaves is the band's 16-sample control grid — the zipper its own attack and
                // release have while engaged — tens of dB under the step, not the tone's own Δ².
                const size_t e0 = (size_t) kLead * blk, e1 = e0 + 16 * blk;
                const double s = maxD2 (runs[1].y, e0, e1), r = maxD2 (runs[0].y, e0, e1);
                const double toneD2 = maxD2 (runs[2].y, e0, e1);   // the undimmed tone's own Δ²
                check (s > 30.0 * toneD2, (tag + "PRECONDITION: the hard step is loud on this instrument").toRawUTF8());
                check (r < 0.05 * s,    (tag + "the release's worst second difference is under 5% of the hard step's").toRawUTF8());

                double landDev = -1.0;   // the landing's second difference against the static twin
                {
                    check (landed > kLead + 1, (tag + "the release takes time: more than one block").toRawUTF8());
                    check (landed > 0 && landed < kLead + kTail - 100,
                           (tag + "the release lands on exactly 0 dB, with room to spare in the render").toRawUTF8());
                    bool stays = landed > 0;
                    for (size_t k = (size_t) juce::jmax (landed, 0); k < d.size(); ++k) stays = stays && juce::exactlyEqual (d[k], 0.0f);
                    check (stays, (tag + "...and stays there").toRawUTF8());

                    // The landing itself. The step window above closes ~85 ms after the edge and the bit identity
                    // below opens a block after the landing, so the moment the producer disengages and hands the
                    // seam back is watched here: across the blocks around it the output's second difference is
                    // the never-dynamic twin's to within a hair — the delta is under 1e-6 dB by then, and the
                    // hand-back adds nothing on top.
                    landDev = 0.0;
                    if (landed > kLead + 2)
                        for (size_t i = (size_t) (landed - 2) * blk; i < (size_t) (landed + 3) * blk && i < runs[0].y.size(); ++i)
                        {
                            const auto d2 = [] (const std::vector<float>& y, size_t j) { return (double) y[j] - 2.0 * (double) y[j - 1] + (double) y[j - 2]; };
                            landDev = juce::jmax (landDev, std::abs (d2 (runs[0].y, i) - d2 (runs[2].y, i)));
                        }
                    check (landed > kLead + 2 && landDev < 1.0e-6,
                           (tag + "the landing and the hand-back add no step (second difference within 1e-6 of the static twin's)").toRawUTF8());

                    // The hand-back: from the block after the one it landed in, the point IS the static point.
                    bool same = landed > 0;
                    for (size_t i = (size_t) (landed + 1) * blk; same && i < runs[0].y.size(); ++i)
                        same = juce::exactlyEqual (runs[0].y[i], runs[2].y[i]);
                    check (same, (tag + "once landed, the output is the never-dynamic twin's, bit for bit").toRawUTF8());
                }

                const juce::String at = landed > 0 ? "at 0 dB within " + juce::String (juce::roundToInt (1000.0 * (landed - kLead + 1) * blk / fs)) + " ms"
                                                   : juce::String ("never at exactly 0 dB");
                std::printf ("  release on disengage, %-22s ducked %6.2f dB, %-40s max|d2y| hard step %6.1f dBFS, release %6.1f, tone %6.1f%s\n",
                             edgeName[edge], (double) ducked, (at + ";").toRawUTF8(), dBFS (s), dBFS (r), dBFS (toneD2),
                             landDev >= 0.0 ? (juce::String ("; landing vs static ") + juce::String (dBFS (landDev), 1) + " dBFS").toRawUTF8() : "");
            }
        }

        // (g) A STATIC SETTING CONFIGURED BEFORE THE FIRST SAMPLE RENDERS EXACTLY AS BEFORE. The release is an
        // EDGE: it can only begin where a point was ducking. A point that is static from its first sample —
        // plainly static, armed with dyn_on off, or fully armed with the preview off — must still render what
        // the adapter always rendered for it, which is the bare engine's static answer. So hold each one
        // against a bare teq::EqEngine fed readBand() (with dyn.on cleared, as the adapter hands a point the
        // preview switch has off) every block, and NO trim: the output trim sits at the host's 0 dB as the FMA
        // snap leaves it (-5.36e-7 dB, forced here on every platform), which the adapter reads as exactly 0 —
        // a gain of exactly 1. Bit for bit, no tolerance. The reference compiles the same header-only engine in
        // this TU that the processor compiles in its own, under the same flags: with clang a contraction is
        // decided per source expression, so the two agree on every row it builds (checked on arm64 with FMA
        // and x86-64 without, sanitized and not). GCC's -ffp-contract=fast contracts after inlining, so on an
        // FMA target it could part the two TUs — the reason to look here first if this ever reads unequal there.
        {
            constexpr int blk = 256, blocks = 400;
            for (int kind = 0; kind < 3; ++kind)
            {
                auto p = std::make_unique<TabbyEqAudioProcessor>();
                makeBand (*p, 0, 6.0f);
                makeBand (*p, 1, -9.0f);
                setFloat (p->apvts, tabby::laneParamId (1, 0, "freq"), 220.0f);
                if (kind >= 1)
                {
                    setBool  (p->apvts, tabby::bandId (0, "dyn_auto"), false);
                    setFloat (p->apvts, tabby::bandId (0, "dyn_thr"), -30.0f);
                    setFloat (p->apvts, tabby::bandId (0, "dyn_range"), -18.0f);
                    setBool  (p->apvts, tabby::bandId (0, "dyn_on"), kind == 2);   // kind 1: armed, dyn_on OFF
                }                                                                  // kind 2: dyn_on ON, preview OFF
                setCentredZero (p->apvts, "output");
                p->setPlayConfigDetails (2, 2, fs, blk);
                p->prepareToPlay (fs, blk);

                auto bare = std::make_unique<teq::EqEngine>();
                check (bare->prepare (fs, blk, 2), "static-as-before: PRECONDITION: the bare engine prepares");

                juce::AudioBuffer<float> a (2, blk), b (2, blk);
                juce::MidiBuffer midi;
                double ph = 0.0;
                bool same = true;
                for (int k = 0; k < blocks; ++k)
                {
                    for (int i = 0; i < blk; ++i)
                    {
                        const float x = 0.5f * (float) std::sin (ph) + 0.25f * (float) std::sin (0.23 * ph);
                        ph += juce::MathConstants<double>::twoPi * f0 / fs;
                        a.setSample (0, i, x); a.setSample (1, i, 0.8f * x);
                    }
                    b.makeCopyOf (a);
                    p->processBlock (a, midi);
                    {
                        juce::ScopedNoDenormals noDenormals;   // what the processor's audio thread runs under
                        for (int band = 0; band < tabby::kNumBands; ++band)
                        {
                            auto bp = p->readBand (band);
                            bp.dyn.on = false;
                            bare->setBand (band, bp);
                        }
                        check (bare->process (b.getArrayOfWritePointers(), 2, blk), "static-as-before: PRECONDITION: the bare engine runs");
                    }
                    same = same && identical (a, b);
                }
                const char* what[] = { "static-as-before: a static point renders the bare engine's answer, bit for bit",
                                       "static-as-before: ...and so does one armed with dyn_on off",
                                       "static-as-before: ...and one fully armed with the preview off" };
                check (same, what[kind]);
                check (juce::exactlyEqual (p->dynamicDeltaDb (0, 0), 0.0f), "static-as-before: ...publishing no gain reduction");
            }
        }

        // (j) A 0 dB TRIM IS A WIRE. With no point on, the only thing between input and output is the trim, so a
        // trim at the host's 0 dB — as the FMA snap leaves it, -5.36e-7 dB, forced here on every platform — must
        // hand the input back bit for bit, from the first sample (prepare) and after a live write (the smoother's
        // target) alike. Before the snap it multiplied by 0.99999994 on Apple Silicon.
        {
            constexpr int blk = 256;
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            setCentredZero (p->apvts, "output");
            p->setPlayConfigDetails (2, 2, fs, blk);
            p->prepareToPlay (fs, blk);
            juce::AudioBuffer<float> x (2, blk), in (2, blk);
            juce::MidiBuffer midi;
            juce::Random r (4242);
            bool wire = true;
            for (int k = 0; k < 40; ++k)
            {
                if (k == 20) { setFloat (p->apvts, "output", -6.0f); }        // a live move away...
                if (k == 21) { setCentredZero (p->apvts, "output"); }         // ...and back to the host's 0
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < blk; ++i) x.setSample (c, i, r.nextFloat() * 2.0f - 1.0f);
                in.makeCopyOf (x);
                p->processBlock (x, midi);
                if (k < 20 || k >= 30) wire = wire && identical (x, in);     // after the 20 ms ramp back has landed
            }
            check (wire, "trim: a 0 dB output trim is bit-identical to no trim, at prepare and after a live move back");
        }

        // (h) A ZERO-LENGTH BLOCK MOVES NOTHING (law 11: no samples, no time). JUCE's VST3 wrapper passes one on
        // when a host flushes parameters with its buses attached (juce_audio_plugin_client_VST3.cpp, process():
        // numSamples 0 with numInputs/numOutputs set still reaches processBlock). The adapter used to read it as
        // a block the dynamic path skipped — captureSectionInput() refuses n == 0, so releaseDynamics() ran — and
        // dropped every duck and every release in flight on the spot. So: a render with a zero-length call
        // spliced in mid-duck and again mid-release is the render without it, bit for bit.
        {
            constexpr int blk = 256;
            auto a = std::make_unique<TabbyEqAudioProcessor>();
            auto b = std::make_unique<TabbyEqAudioProcessor>();
            for (auto* p : { a.get(), b.get() })
            {
                makeBand (*p, 0, 0.0f);
                setFloat (p->apvts, tabby::laneParamId (0, 0, "freq"), 220.0f);
                armDynamics (*p, -18.0f, 0);
                p->setPlayConfigDetails (2, 2, fs, blk);
                p->prepareToPlay (fs, blk);
            }
            juce::AudioBuffer<float> x (2, blk), y (2, blk), empty (2, 0);
            juce::MidiBuffer midi;
            double ph = 0.0;
            bool same = true;
            float duckBeforeSplice = 0.0f;
            for (int k = 0; k < 400; ++k)
            {
                if (k == 150) duckBeforeSplice = a->dynamicDeltaDb (0, 0);
                if (k == 150 || k == 210) a->processBlock (empty, midi);          // mid-duck, then mid-release
                if (k == 200) for (auto* p : { a.get(), b.get() }) setBool (p->apvts, tabby::bandId (0, "dyn_on"), false);
                for (int i = 0; i < blk; ++i)
                {
                    const float s = 0.5f * (float) std::sin (ph);
                    ph += juce::MathConstants<double>::twoPi * 220.0 / fs;
                    x.setSample (0, i, s); x.setSample (1, i, s);
                }
                y.makeCopyOf (x);
                a->processBlock (x, midi);
                b->processBlock (y, midi);
                same = same && identical (x, y);
            }
            check (duckBeforeSplice < -6.0f, "zero-length: PRECONDITION: the point is ducking where the first call is spliced in");
            check (same, "zero-length: a zero-length block mid-duck and mid-release changes nothing, bit for bit");
        }

        // (i) RE-PREPARING MID-RELEASE STARTS THE NEW STREAM AT ITS OWN SETTINGS. prepareToPlay() hands a held band
        // back before the engine restarts its bands; the other order made the hand-back the band's first write of
        // the new stream, which snapped it to the OLD parameters and let the host's edit made while stopped ramp
        // in. So: a point released mid-release, its static gain moved 0 -> 12 dB, re-prepared — renders what a
        // fresh instance with the final settings renders, bit for bit, from the first sample.
        {
            constexpr int blk = 256;
            auto finalSettings = [&] (TabbyEqAudioProcessor& p)
            {
                makeBand (p, 0, 0.0f);
                setFloat (p.apvts, tabby::laneParamId (0, 0, "freq"), 220.0f);
                armDynamics (p, -18.0f, 0);
                setBool (p.apvts, tabby::bandId (0, "dyn_on"), false);
            };
            auto a = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*a, 0, 0.0f);
            setFloat (a->apvts, tabby::laneParamId (0, 0, "freq"), 220.0f);
            armDynamics (*a, -18.0f, 0);
            a->setPlayConfigDetails (2, 2, fs, blk);
            a->prepareToPlay (fs, blk);
            juce::AudioBuffer<float> x (2, blk), y (2, blk);
            juce::MidiBuffer midi;
            double ph = 0.0;
            auto fill = [&] (juce::AudioBuffer<float>& buf)
            {
                for (int i = 0; i < blk; ++i)
                {
                    const float s = 0.5f * (float) std::sin (ph);
                    ph += juce::MathConstants<double>::twoPi * 220.0 / fs;
                    buf.setSample (0, i, s); buf.setSample (1, i, s);
                }
            };
            for (int k = 0; k < 200; ++k) { fill (x); a->processBlock (x, midi); }
            setBool (a->apvts, tabby::bandId (0, "dyn_on"), false);
            for (int k = 0; k < 10; ++k) { fill (x); a->processBlock (x, midi); }
            check (a->dynamicDeltaDb (0, 0) < -1.0f, "re-prepare: PRECONDITION: the point is mid-release when the stream stops");
            a->releaseResources();
            finalSettings (*a);
            setFloat (a->apvts, tabby::laneParamId (0, 0, "gain"), 12.0f);
            a->prepareToPlay (fs, blk);

            auto b = std::make_unique<TabbyEqAudioProcessor>();
            finalSettings (*b);
            setFloat (b->apvts, tabby::laneParamId (0, 0, "gain"), 12.0f);
            b->setPlayConfigDetails (2, 2, fs, blk);
            b->prepareToPlay (fs, blk);

            bool same = true;
            for (int k = 0; k < 40; ++k)
            {
                fill (x); y.makeCopyOf (x);
                a->processBlock (x, midi);
                b->processBlock (y, midi);
                same = same && identical (x, y);
            }
            check (same, "re-prepare: a stream re-prepared mid-release renders a fresh instance's answer, bit for bit");
            check (juce::exactlyEqual (a->dynamicDeltaDb (0, 0), 0.0f), "re-prepare: ...and publishes no leftover gain reduction");
        }
    }

    if (failures == 0) std::cout << "TabbyEQ lifecycle/misuse: all checks passed\n";
    else               std::cerr << "TabbyEQ lifecycle/misuse: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
