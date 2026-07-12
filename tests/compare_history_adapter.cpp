// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

// ---------------------------------------------------------------------------
// TabbyEQ CompareHistory ADAPTER harness — theory-driven falsification of the processor's undo/redo
// + A/B/C/D wiring (the engine itself is proven upstream by felitronics-appkit's 146-check suite;
// here we attack the SEAMS: captureLive/applyLiveState, the v4 <Workspace> session envelope, the
// legacy v2/v3 load paths, save purity, and the per-register semantics through the public surface).
//
// Determinism: undoTick() is called directly (the settle timer counts ticks, not wall-clock), and
// apvts.copyState() flushes the param->tree sync synchronously — so NO message-loop pumping is
// needed for history operations. ScopedJuceInitialiser_GUI supplies the MessageManager the
// processor's own construction expects.
// ---------------------------------------------------------------------------

#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

#include <iostream>

namespace
{
    int failures = 0;
    void check (bool ok, const char* what) { if (! ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; } }

    // Access the protected static XML<->binary helpers without instantiating (the class stays abstract).
    struct ApAccess : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

    float rv (TabbyEqAudioProcessor& p, const juce::String& id)
    {
        auto* v = p.apvts.getRawParameterValue (id);
        return v != nullptr ? v->load() : -999.0f;
    }

    void setReal (TabbyEqAudioProcessor& p, const juce::String& id, float real)
    {
        if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (real));
    }

    // Enough ticks for a stable burst to commit (settleTicks = 12: one tick notices the change,
    // twelve more see it stable), with headroom.
    void settle (TabbyEqAudioProcessor& p) { for (int i = 0; i < 20; ++i) p.undoTick(); }

    juce::MemoryBlock saveState (TabbyEqAudioProcessor& p)
    {
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        return mb;
    }

    void loadState (TabbyEqAudioProcessor& p, const juce::MemoryBlock& mb)
    {
        p.setStateInformation (mb.getData(), (int) mb.getSize());
    }

    std::unique_ptr<juce::XmlElement> parseState (const juce::MemoryBlock& mb)
    {
        return ApAccess::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    }

    juce::MemoryBlock treeToBinary (const juce::ValueTree& t)
    {
        juce::MemoryBlock mb;
        if (auto xml = t.createXml()) ApAccess::copyXmlToBinary (*xml, mb);
        return mb;
    }

    // The ST-lane freq of band 0 — the workhorse probe parameter.
    juce::String probeId() { return tabby::laneParamId (0, 0, "freq"); }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ====== 1. Settle-timer undo/redo through the adapter ======
    {
        TabbyEqAudioProcessor p;
        const float def = rv (p, probeId());
        check (! p.canUndo() && ! p.canRedo(), "1: fresh processor has no history");

        setReal (p, probeId(), 2000.0f);
        check (! p.canUndo(), "1: unsettled burst is not yet a step");
        settle (p);
        check (p.canUndo() && p.undoDepth() == 1, "1: settled edit commits as exactly one step");

        check (p.undo(), "1: undo succeeds");
        check (std::abs (rv (p, probeId()) - def) < 1e-3f, "1: undo restores the pre-edit value");
        check (p.canRedo(), "1: redo is armed after undo");
        check (p.redo() && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "1: redo re-applies the edit");
    }

    // ====== 2. Undo inside the settle window reverts EXACTLY the last edit (flush-first, §2.1) ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);          // committed S1
        setReal (p, probeId(), 3000.0f);                      // unsettled S2
        check (p.undo(), "2: undo mid-burst succeeds");
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "2: undo mid-burst reverts ONLY the unsettled edit (S1 not lost)");
        check (p.canUndo(), "2: the earlier settled step survives");
        check (p.redo() && std::abs (rv (p, probeId()) - 3000.0f) < 1e-3f, "2: the flushed burst is redo-able");
    }

    // ====== 3. Gesture brackets: a drag = ONE labelled step ======
    {
        TabbyEqAudioProcessor p;
        const float def = rv (p, probeId());

        setReal (p, probeId(), 1500.0f);                      // pre-gesture tweak, unsettled
        p.beginHistoryGesture ("Move Band 1");                // must flush the tweak as its OWN step
        const int depthAtGestureStart = p.undoDepth();
        check (depthAtGestureStart == 1, "3: beginGesture flushes the pre-gesture burst as a separate step");

        for (float f : { 1600.0f, 1700.0f, 1800.0f })
        {
            setReal (p, probeId(), f);
            settle (p);                                       // ticks INSIDE a gesture must not commit
        }
        check (p.undoDepth() == depthAtGestureStart, "3: nothing commits while the gesture is open");
        p.endHistoryGesture();
        check (p.undoDepth() == depthAtGestureStart + 1, "3: the whole drag lands as exactly ONE step");
        check (p.peekUndoLabel() == "Move Band 1", "3: the gesture step carries its label");

        check (p.undo() && std::abs (rv (p, probeId()) - 1500.0f) < 1e-3f, "3: undoing the drag restores the grab point");
        check (p.undo() && std::abs (rv (p, probeId()) - def) < 1e-3f, "3: undoing the pre-tweak restores the default");

        // Nested gestures coalesce into the outermost.
        p.beginHistoryGesture ("Outer");
        p.beginHistoryGesture ("Inner");
        setReal (p, probeId(), 4000.0f);
        p.endHistoryGesture();
        setReal (p, probeId(), 4100.0f);
        p.endHistoryGesture();
        check (p.undoDepth() == 1 && p.peekUndoLabel() == "Outer", "3: nested gestures = one step, outer label");
    }

    // ====== 4. Per-register isolation: switch is NOT an undo step; each register has its own history ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);
        check (p.snapshotEdited (0) && ! p.snapshotEdited (1), "4: edited marker set on A only");

        p.switchToSnapshot (1);
        check (p.getActiveSnapshot() == 1, "4: switch lands on B");
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "4: a never-used register keeps the live sound as its seed");
        check (! p.canUndo(), "4: B starts with an empty history (switch is not an undo step)");

        setReal (p, probeId(), 5000.0f); settle (p);
        check (p.canUndo(), "4: B accumulates its own history");
        check (p.undo() && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "4: undo acts on B only");

        p.switchToSnapshot (0);
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "4: A recalls its stashed sound");
        check (p.canUndo(), "4: A's history survived the round trip");
        check (p.undo(), "4: A's step is still undoable");
        check (p.snapshotHasContent (1), "4: B holds stored content after leaving it");
    }

    // ====== 5. Copy / paste ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);
        const int depthBefore = p.undoDepth();

        p.copySnapshot (0, 1);                                // birth of an empty slot: content lands, NO step
        check (p.snapshotHasContent (1), "5: copy fills the target register");
        check (! p.snapshotEdited (1), "5: birth of an empty register records no step");
        check (p.undoDepth() == depthBefore, "5: copying to a non-active register leaves the active history alone");

        auto stored = p.snapshotState (1);
        bool storedHasValue = false;
        for (int i = 0; i < stored.getNumChildren(); ++i)
        {
            const auto ch = stored.getChild (i);
            if (ch.getProperty ("id").toString() == probeId())
                storedHasValue = std::abs ((double) ch.getProperty ("value") - 2000.0) < 1e-3;
        }
        check (storedHasValue, "5: the stored register carries the copied value");

        check (! p.canPasteState (juce::ValueTree ("Garbage")), "5: a foreign tree is not pasteable");
        p.pasteState (0, juce::ValueTree ("Garbage"));        // must be a no-op
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "5: pasting garbage changes nothing");

        auto clip = p.snapshotState (0);                      // live capture -> clipboard tree
        for (int i = 0; i < clip.getNumChildren(); ++i)
        {
            auto ch = clip.getChild (i);
            if (ch.getProperty ("id").toString() == probeId())
                ch.setProperty ("value", 7777.0, nullptr);
        }
        p.pasteState (0, clip);                               // paste onto the ACTIVE register = one undoable step
        check (std::abs (rv (p, probeId()) - 7777.0f) < 1e-1f, "5: paste applies the clipboard content");
        check (p.undoDepth() == depthBefore + 1 && p.peekUndoLabel() == "Paste", "5: paste is one labelled step");
        check (p.undo() && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "5: paste is undoable");
    }

    // ====== 6. v4 session round-trip ======
    {
        TabbyEqAudioProcessor p1;
        setReal (p1, probeId(), 2000.0f); settle (p1);
        p1.switchToSnapshot (1);
        setReal (p1, probeId(), 5000.0f); settle (p1);
        const auto blob = saveState (p1);

        const auto xml = parseState (blob);
        check (xml != nullptr && xml->hasTagName ("Workspace"), "6: a v4 session's root is the <Workspace> envelope");
        check (xml != nullptr && xml->getIntAttribute ("stateVersion") == 4, "6: the envelope carries stateVersion=4");
        check (xml != nullptr && xml->getIntAttribute ("count") == 4, "6: the envelope stamps the register count");

        TabbyEqAudioProcessor p2;
        loadState (p2, blob);
        check (p2.getActiveSnapshot() == 1, "6: the active register survives the round trip");
        check (std::abs (rv (p2, probeId()) - 5000.0f) < 1e-3f, "6: the live sound survives the round trip");
        check (p2.snapshotHasContent (0), "6: register A's stored snapshot survives the round trip");
        check (! p2.canUndo() && ! p2.canRedo(), "6: a load is a fresh history boundary");
        check (! p2.snapshotEdited (0) && ! p2.snapshotEdited (1), "6: edited markers are clean after a load");

        p2.switchToSnapshot (0);
        check (std::abs (rv (p2, probeId()) - 2000.0f) < 1e-3f, "6: register A's content survives the round trip");
    }

    // ====== 7. Save purity: a host autosave poll must not mutate history ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);
        setReal (p, probeId(), 3000.0f);                      // unsettled burst

        const auto mb1 = saveState (p);
        const auto mb2 = saveState (p);
        check (mb1 == mb2, "7: two back-to-back saves are byte-identical (pure capture)");
        check (p.undoDepth() == 1, "7: saving does not flush the pending burst");

        TabbyEqAudioProcessor p2;
        loadState (p2, mb1);
        check (std::abs (rv (p2, probeId()) - 3000.0f) < 1e-3f, "7: an unsettled edit still saves as current live");

        settle (p);
        check (p.undoDepth() == 2, "7: the burst survives the save and settles normally");
    }

    // ====== 8. Legacy v3 session (flat state tree) ======
    {
        TabbyEqAudioProcessor donor;                           // build a v3-format blob the way old builds did
        setReal (donor, probeId(), 2500.0f);
        auto flat = donor.apvts.copyState();
        flat.setProperty ("stateVersion", 3, nullptr);
        const auto blob = treeToBinary (flat);

        TabbyEqAudioProcessor p;
        loadState (p, blob);
        check (std::abs (rv (p, probeId()) - 2500.0f) < 1e-3f, "8: a v3 session loads as the live state");
        check (p.getActiveSnapshot() == 0, "8: a v3 session lands on register A");
        check (! p.snapshotHasContent (1) && ! p.snapshotHasContent (2) && ! p.snapshotHasContent (3),
               "8: B/C/D are empty after a legacy load");
        check (! p.canUndo(), "8: a legacy load is a fresh history boundary");

        const auto resaved = parseState (saveState (p));
        check (resaved != nullptr && resaved->hasTagName ("Workspace"), "8: re-saving a migrated session emits the v4 envelope");
    }

    // ====== 9. Legacy v2 session still routes through the v2->v3 migration ======
    {
        juce::ValueTree v2 ("PARAMS");
        v2.setProperty ("stateVersion", 2, nullptr);
        auto add = [&v2] (const juce::String& id, double val)
        {
            juce::ValueTree n ("PARAM");
            n.setProperty ("id", id, nullptr);
            n.setProperty ("value", val, nullptr);
            v2.addChild (n, -1, nullptr);
        };
        add (tabby::bandId (0, "on"),   1.0);
        add (tabby::bandId (0, "freq"), 2345.0);               // v2 flat freq -> v3 ST-lane freq

        TabbyEqAudioProcessor p;
        loadState (p, treeToBinary (v2));
        check (std::abs (rv (p, probeId()) - 2345.0f) < 1e-3f, "9: a v2 flat band migrates onto the ST lane");
        check (rv (p, tabby::bandId (0, "on")) > 0.5f, "9: the migrated band is on");
    }

    // ====== 10. A corrupt <Workspace> (no Live payload) must not destroy the current state ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 4000.0f); settle (p);

        juce::ValueTree bad ("Workspace");
        bad.setProperty ("schema", 1, nullptr);
        bad.setProperty ("count", 4, nullptr);
        bad.setProperty ("active", 0, nullptr);                // deliberately NO <Live> child
        loadState (p, treeToBinary (bad));

        check (std::abs (rv (p, probeId()) - 4000.0f) < 1e-3f, "10: a corrupt envelope leaves the live state untouched");
        check (p.canUndo(), "10: a corrupt envelope leaves the history untouched");
    }

    // ====== 11. No phantom steps: idle ticks after an apply never create history ======
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);
        p.undo();
        const int depth = p.undoDepth();
        for (int i = 0; i < 40; ++i) p.undoTick();
        check (p.undoDepth() == depth, "11: idle ticks after undo commit nothing (apply/capture round trip is stable)");
    }

    // ====== 12. applyLiveState re-syncs derived state: the per-band active lane rides the registers ======
    {
        TabbyEqAudioProcessor p;
        p.setBandActiveLane (0, 3);                            // stored as a state-tree property
        p.switchToSnapshot (1);
        p.setBandActiveLane (0, 4);
        p.switchToSnapshot (0);
        check ((int) p.apvts.state.getProperty (tabby::bandId (0, "activeLane"), -1) == 3,
               "12: register A restores its own active-lane property");
        p.switchToSnapshot (1);
        check ((int) p.apvts.state.getProperty (tabby::bandId (0, "activeLane"), -1) == 4,
               "12: register B restores its own active-lane property");
    }

    // ====== 13. historyRevision bumps on every history mutation the UI must reflect ======
    {
        TabbyEqAudioProcessor p;
        const auto r0 = p.historyRevision();
        setReal (p, probeId(), 2000.0f); settle (p);
        const auto r1 = p.historyRevision();
        check (r1 != r0, "13: a settled commit bumps the revision");
        p.undo();
        const auto r2 = p.historyRevision();
        check (r2 != r1, "13: undo bumps the revision");
        p.switchToSnapshot (2);
        check (p.historyRevision() != r2, "13: a register switch bumps the revision");
    }

    if (failures == 0)
        std::cout << "TabbyEQ CompareHistory adapter harness: ALL OK\n";
    else
        std::cerr << "TabbyEQ CompareHistory adapter harness: " << failures << " FAILURE(S)\n";
    return failures == 0 ? 0 : 1;
}
