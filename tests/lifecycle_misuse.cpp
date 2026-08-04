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

    // Bool params (band/lane on, bypass) need their OWN setter: setChoice's dynamic_cast to
    // AudioParameterChoice fails silently on an AudioParameterBool, so the write never happens.
    void setBool (juce::AudioProcessorValueTreeState& s, const juce::String& id, bool v)
    {
        if (auto* b = dynamic_cast<juce::AudioParameterBool*> (s.getParameter (id))) *b = v;
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
        auto armDynamics = [] (TabbyEqAudioProcessor& p, float rangeDb, int b = 0)
        {
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
            // Range 0 is a documented disengage, so it must not DUCK — but it is not bit-identical:
            // with dyn.on the band still runs its (unity) delta section, which costs one float ULP.
            // The bit-identity promise is scoped to dyn.on == false, asserted above; here we pin the
            // real contract, tightly enough that any actual gain movement fails the check.
            auto p = std::make_unique<TabbyEqAudioProcessor>();
            makeBand (*p);
            armDynamics (*p, 0.0f);                                        // ON but zero range
            p->setPlayConfigDetails (2, 2, fs, n);
            p->prepareToPlay (fs, n);
            phase = 0.0;
            juce::AudioBuffer<float> got;
            runTone (*p, 8, n, got);
            double maxDiff = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (got.getReadPointer (c)[i] - ref.getReadPointer (c)[i]));
            check (maxDiff <= 2.0e-7, "dyn: dyn_on with range 0 moves the signal by at most one ULP");
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
    }

    if (failures == 0) std::cout << "TabbyEQ lifecycle/misuse: all checks passed\n";
    else               std::cerr << "TabbyEQ lifecycle/misuse: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
