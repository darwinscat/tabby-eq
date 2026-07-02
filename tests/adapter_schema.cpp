// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

// ---------------------------------------------------------------------------
// TabbyEQ adapter schema / migration / link-mirroring harness (v3 placement lanes).
//
// Drives the REAL TabbyEqAudioProcessor to assert the v3 parameter SCHEMA (collision-free display names,
// param count, group-per-band), the v2->v3 state MIGRATION (plain / ms-split / mid-bypassed+side-active /
// sType fission + pool-full coercion / msFreqLink diverged-vs-matching), and the processor-side LINK
// MIRRORING (freq + Q + HP/LP slope mirrors, disabled lanes untouched, delivery order last-wins, overflow
// resync). Runs headless: ScopedJuceInitialiser_GUI supplies the MessageManager for the 30 Hz link drain.
// ---------------------------------------------------------------------------

#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

#include <iostream>
#include <set>

namespace
{
    int failures = 0;
    void check (bool ok, const char* what) { if (! ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; } }

    // Access the protected static XML<->binary helpers without instantiating (the class stays abstract).
    struct ApAccess : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
    };

    void pumpTimers (int ms)
    {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            mm->runDispatchLoopUntil (ms);
    }

    float rv (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        auto* p = s.getRawParameterValue (id);
        return p != nullptr ? p->load() : -999.0f;
    }
    bool  prop (juce::AudioProcessorValueTreeState& s, const juce::String& id) { return (bool) s.state.getProperty (id, false); }

    void setRealParam (juce::AudioProcessorValueTreeState& s, const juce::String& id, float real)
    {
        if (auto* p = s.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (real));
    }

    // --- a sparse v2 (pre-lanes) state builder: flat Mid/main band + optional M/S Side lane ---
    struct V2Builder
    {
        juce::ValueTree tree { "PARAMS" };
        V2Builder() { tree.setProperty ("stateVersion", 2, nullptr); }

        void add (const juce::String& id, double v)
        {
            juce::ValueTree n ("PARAM");
            n.setProperty ("id", id, nullptr);
            n.setProperty ("value", v, nullptr);
            tree.addChild (n, -1, nullptr);
        }
        void band (int b, bool on, int type, double freq, double q, double gain, int slope, bool byp)
        {
            using namespace tabby;
            add (bandId (b, "on"), on ? 1 : 0); add (bandId (b, "type"), type);
            add (bandId (b, "freq"), freq); add (bandId (b, "q"), q); add (bandId (b, "gain"), gain);
            add (bandId (b, "slope"), slope); add (bandId (b, "bypass"), byp ? 1 : 0);
        }
        void side (int b, bool ms, bool sOn, int sType, double sFreq, double sQ, double sGain, int sSlope, bool sByp)
        {
            using namespace tabby;
            add (bandId (b, "ms"), ms ? 1 : 0); add (bandId (b, "sOn"), sOn ? 1 : 0); add (bandId (b, "sType"), sType);
            add (bandId (b, "sFreq"), sFreq); add (bandId (b, "sQ"), sQ); add (bandId (b, "sGain"), sGain);
            add (bandId (b, "sSlope"), sSlope); add (bandId (b, "sBypass"), sByp ? 1 : 0);
        }
        void load (TabbyEqAudioProcessor& p)
        {
            auto xml = tree.createXml();
            juce::MemoryBlock mb;
            ApAccess::copyXmlToBinary (*xml, mb);
            p.setStateInformation (mb.getData(), (int) mb.getSize());
        }
    };

    bool near (double a, double b, double eps = 0.01) { return std::abs (a - b) < eps; }
}

int main()
{
    using namespace tabby;
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ============================ 1. Schema sanity ============================
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();

        const auto& params = p->getParameters();
        check (params.size() == kNumBands * 34 + 4, "parameter count == 24*34 + 4 globals (820)");

        std::set<juce::String> names;
        bool unique = true;
        for (auto* prm : params)
        {
            const auto nm = prm->getName (256);
            if (! names.insert (nm).second) { unique = false; std::cerr << "  dup name: " << nm << '\n'; }
        }
        check (unique, "all ~820 display names are unique");

        // Group-per-band tree: 24 subgroups (34 params each) + 4 top-level global params.
        const auto& tree = p->getParameterTree();
        check (tree.getSubgroups (false).size() == kNumBands, "24 per-band parameter groups");
        check (tree.getParameters (false).size() == 4, "4 top-level global params");
        bool groupsOk = true;
        for (auto* g : tree.getSubgroups (false))
            if (g->getParameters (false).size() != 34) groupsOk = false;
        check (groupsOk, "each band group holds 34 params");

        // ST-lane display names drop the lane word for the point-unique fields; keep it for On/Bypass.
        auto nameOf = [&] (const juce::String& id) { auto* pr = p->apvts.getParameter (id); return pr ? pr->getName (256) : juce::String ("?"); };
        check (nameOf (laneParamId (3, 0, "freq")) == "B4 Freq",         "ST freq drops the lane word");
        check (nameOf (laneParamId (3, 0, "on"))   == "B4 Stereo On",    "ST on keeps the lane word");
        check (nameOf (laneParamId (3, 0, "byp"))  == "B4 Stereo Bypass","ST bypass keeps the lane word");
        check (nameOf (laneParamId (3, 1, "freq")) == "B4 Left Freq",    "Left freq carries the lane word");
        check (nameOf (bandId (3, "bypass"))       == "B4 Bypass",       "point bypass name");
    }

    // ============================ 2. Migration: plain stereo (ms=false) ============================
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        V2Builder v;
        v.band (0, true, 0 /*Bell*/, 500.0, 2.0, 3.0, 2 /*24dB*/, true /*bypass -> ST lane byp*/);
        v.load (*p);

        check (rv (p->apvts, laneParamId (0, 0, "on"))   > 0.5f, "plain: ST lane on");
        check (near (rv (p->apvts, laneParamId (0, 0, "freq")), 500.0), "plain: ST freq migrated");
        check (near (rv (p->apvts, laneParamId (0, 0, "gain")), 3.0),   "plain: ST gain migrated");
        check ((int) rv (p->apvts, laneParamId (0, 0, "slope")) == 2,   "plain: ST slope migrated");
        check (rv (p->apvts, laneParamId (0, 0, "byp"))  > 0.5f, "plain: flat bypass -> ST lane byp");
        check (rv (p->apvts, bandId (0, "bypass"))       < 0.5f, "plain: point bypass stays false");
        check (rv (p->apvts, laneParamId (0, 3, "on"))   < 0.5f, "plain: Mid lane off");
        check (rv (p->apvts, laneParamId (0, 4, "on"))   < 0.5f, "plain: Side lane off");
    }

    // ============================ 3. Migration: ms split, mid-bypassed + side-active ============================
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        V2Builder v;
        v.band (1, true, 0 /*Bell*/, 800.0, 1.5, 4.0, 1, true /*flat bypass = Mid bypass*/);
        v.side (1, true, true, 0 /*sType==type*/, 1200.0, 3.0, -2.0, 3, false /*sBypass=false -> Side active*/);
        v.load (*p);

        check (rv (p->apvts, laneParamId (1, 0, "on")) < 0.5f, "split: ST lane off");
        check (rv (p->apvts, laneParamId (1, 3, "on")) > 0.5f, "split: Mid lane on");
        check (rv (p->apvts, laneParamId (1, 4, "on")) > 0.5f, "split: Side lane on");
        check (near (rv (p->apvts, laneParamId (1, 3, "freq")), 800.0),  "split: Mid freq <- flat");
        check (near (rv (p->apvts, laneParamId (1, 4, "freq")), 1200.0), "split: Side freq <- s*");
        check (rv (p->apvts, laneParamId (1, 3, "byp")) > 0.5f, "split: flat bypass -> Mid lane byp");
        check (rv (p->apvts, laneParamId (1, 4, "byp")) < 0.5f, "split: Side stays running (byp=false)");
        check (rv (p->apvts, bandId (1, "bypass"))      < 0.5f, "split: point bypass stays false");
    }

    // ============================ 4. Migration: sType != type -> fission into a new point ============================
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        V2Builder v;
        v.band (0, true, 0 /*Bell*/, 300.0, 1.0, 2.0, 1, false);
        v.band (1, true, 0 /*Bell*/, 900.0, 1.0, 4.0, 1, false);
        v.band (2, true, 0 /*Bell*/, 700.0, 2.0, 5.0, 1, false);
        v.side (2, true, true, 2 /*sType HighShelf != Bell*/, 6000.0, 1.2, -3.0, 1, false);
        v.load (*p);

        // Band 2 keeps its index, becomes {m}-only; the Side lane fissions to the first free slot (band 3).
        check (rv (p->apvts, laneParamId (2, 3, "on")) > 0.5f, "fission: source Mid lane on");
        check (rv (p->apvts, laneParamId (2, 4, "on")) < 0.5f, "fission: source Side lane gone");
        check (rv (p->apvts, bandId (3, "on")) > 0.5f, "fission: new point at first free slot (band 3) on");
        check ((int) rv (p->apvts, bandId (3, "type")) == 2, "fission: new point type = sType (HighShelf)");
        check (rv (p->apvts, laneParamId (3, 0, "on")) < 0.5f, "fission: new point ST off ({s}-only)");
        check (rv (p->apvts, laneParamId (3, 4, "on")) > 0.5f, "fission: new point Side on");
        check (near (rv (p->apvts, laneParamId (3, 4, "freq")), 6000.0), "fission: new point Side freq <- s*");
        check (! prop (p->apvts, bandId (2, "linkFq")), "fission: source linkFq false");
    }

    // ============================ 5. Migration: pool full -> coerce sType->type + migrationNote ============================
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        V2Builder v;
        for (int b = 0; b < kNumBands; ++b) v.band (b, true, 0 /*Bell*/, 200.0 + b, 1.0, 0.0, 1, false);   // all slots taken
        v.side (0, true, true, 2 /*HighShelf != Bell, no free slot*/, 5000.0, 1.0, -4.0, 1, false);
        v.load (*p);

        check (rv (p->apvts, laneParamId (0, 3, "on")) > 0.5f, "poolfull: Mid on");
        check (rv (p->apvts, laneParamId (0, 4, "on")) > 0.5f, "poolfull: Side kept (coerced) on same band");
        check ((int) rv (p->apvts, bandId (0, "type")) == 0, "poolfull: shared type stays Bell (sType coerced)");
        check (prop (p->apvts, "migrationNote"), "poolfull: migrationNote set");
    }

    // ============================ 6. Migration: msFreqLink diverged (must NOT re-link) vs matching (must link) ==========
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        V2Builder v; v.tree.setProperty ("msFreqLink", true, nullptr);
        v.band (0, true, 0, 800.0, 1.0, 0.0, 1, false);
        v.side (0, true, true, 0, 1200.0, 1.0, 0.0, 1, false);   // diverged freqs
        v.band (1, true, 0, 800.0, 1.0, 0.0, 1, false);
        v.side (1, true, true, 0, 800.0, 1.0, 0.0, 1, false);    // matching freqs
        v.load (*p);
        check (! prop (p->apvts, bandId (0, "linkFq")), "msFreqLink: diverged freqs -> band NOT re-linked");
        check (  prop (p->apvts, bandId (1, "linkFq")), "msFreqLink: matching freqs -> band linked");
        check (  prop (p->apvts, "defaultLinkFq"),      "msFreqLink -> defaultLinkFq default carried forward");
    }

    // ============================ 7. Link mirroring (freq / Q / slope, disabled-lane, order, overflow) =============
    {
        auto p = std::make_unique<TabbyEqAudioProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);

        auto splitBell = [&] (int b, int type)   // ST off, Mid+Side on, shared type
        {
            setRealParam (p->apvts, bandId (b, "type"), (float) type);
            setRealParam (p->apvts, laneParamId (b, 0, "on"), 0.0f);
            setRealParam (p->apvts, laneParamId (b, 3, "on"), 1.0f);
            setRealParam (p->apvts, laneParamId (b, 4, "on"), 1.0f);
        };

        // (a) freq mirror to enabled lanes only (Left stays put).
        splitBell (0, 0 /*Bell*/);
        p->apvts.state.setProperty (bandId (0, "linkFq"), true, nullptr);
        setRealParam (p->apvts, laneParamId (0, 3, "freq"), 2500.0f);
        pumpTimers (120);
        check (near (rv (p->apvts, laneParamId (0, 4, "freq")), 2500.0, 1.0), "link: Mid freq mirrors to Side");
        check (near (rv (p->apvts, laneParamId (0, 1, "freq")), 1000.0, 1.0), "link: disabled Left lane NOT written");

        // (b) Q mirror for Bell.
        p->apvts.state.setProperty (bandId (0, "linkQ"), true, nullptr);
        setRealParam (p->apvts, laneParamId (0, 3, "q"), 5.0f);
        pumpTimers (120);
        check (near (rv (p->apvts, laneParamId (0, 4, "q")), 5.0, 0.05), "link: Bell Q mirrors to Side");

        // (c) SLOPE mirror for HP/LP (linkQ drives slope when the shared type is a cut).
        splitBell (1, 3 /*High Pass*/);
        p->apvts.state.setProperty (bandId (1, "linkQ"), true, nullptr);
        setRealParam (p->apvts, laneParamId (1, 3, "slope"), 4.0f /*48 dB/oct*/);
        pumpTimers (120);
        check ((int) rv (p->apvts, laneParamId (1, 4, "slope")) == 4, "link: HP/LP slope mirrors to Side");

        // (d) delivery-order last-wins (two edits before a drain).
        splitBell (2, 0);
        p->apvts.state.setProperty (bandId (2, "linkFq"), true, nullptr);
        setRealParam (p->apvts, laneParamId (2, 3, "freq"), 400.0f);
        setRealParam (p->apvts, laneParamId (2, 3, "freq"), 700.0f);
        pumpTimers (120);
        check (near (rv (p->apvts, laneParamId (2, 4, "freq")), 700.0, 1.0), "link: last-delivered edit wins");

        // (e) overflow resync: >256 rapid edits (no drain in between) latch overflow -> resync from active lane.
        splitBell (3, 0);
        p->apvts.state.setProperty (bandId (3, "linkFq"), true, nullptr);
        for (int i = 0; i < 400; ++i) setRealParam (p->apvts, laneParamId (3, 3, "freq"), 600.0f + (float) (i % 30));
        setRealParam (p->apvts, laneParamId (3, 3, "freq"), 1234.0f);   // final value
        pumpTimers (200);
        check (near (rv (p->apvts, laneParamId (3, 4, "freq")), 1234.0, 1.0), "link: overflow resync snaps Side to the active lane");

        // (f) re-entrancy: the whole batch above completed without hanging (mirror writes never re-enqueue).
        check (true, "link: no infinite mirror loop (reached here)");
    }

    if (failures == 0) std::cout << "TabbyEQ adapter schema/migration/link: all checks passed\n";
    else               std::cerr << "TabbyEQ adapter schema/migration/link: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
