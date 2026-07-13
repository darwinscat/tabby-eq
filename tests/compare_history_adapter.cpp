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

    // Enough ticks for a stable burst to commit (settleTicks = 4: one tick notices the change,
    // four more see it stable), with generous headroom.
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

    constexpr int kNumSnapshotsProbe = 4;   // mirrors TabbyEqAudioProcessor::kNumSnapshots for OOB probes
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

        check (! p.snapshotEdited (-1) && ! p.snapshotEdited (kNumSnapshotsProbe), "4: an out-of-range register index reads as not-edited (no OOB)");

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

        // Aliasing pin: the live tree must be a DEEP COPY of the pasted content — a later live edit
        // must never mutate the caller's clipboard tree (this fails if applyLiveState drops createCopy).
        // The copyState() below is load-bearing: it FLUSHES the param write into the (potentially
        // aliased) live tree — without it the write sits in the parameter adapter and the pin can
        // never fail in this loop-less harness (the APVTS flush timer never fires here).
        setReal (p, probeId(), 9000.0f);
        (void) p.apvts.copyState();
        bool clipIntact = false;
        for (int i = 0; i < clip.getNumChildren(); ++i)
        {
            const auto ch = clip.getChild (i);
            if (ch.getProperty ("id").toString() == probeId())
                clipIntact = std::abs ((double) ch.getProperty ("value") - 7777.0) < 1e-3;
        }
        check (clipIntact, "5: post-paste live edits never mutate the clipboard tree (deep copy pinned)");

        check (p.undo() && std::abs (rv (p, probeId()) - 7777.0f) < 1e-1f, "5: undo flushes the post-paste burst first");
        check (p.undo() && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "5: paste is undoable");
    }

    // ====== 5b. Foreign-but-PARAMS-typed paste: degrades to DEFAULTS, documented ======
    // Empirically verified (JUCE 8.0.14): replaceState appends a fresh valueless child for every
    // param absent from the pasted tree, and the valueTreeChildAdded listener then setNewState()s
    // it — no "value" property -> the param RESETS TO ITS DEFAULT (it does NOT keep its current
    // value; static reads of updateParameterConnectionsToChildTrees miss the child-added listener).
    // So a sparse/foreign PARAMS tree = present-params-applied + absent-params-defaulted. No crash,
    // one undoable step, fully undoable.
    {
        TabbyEqAudioProcessor p;
        const float def     = rv (p, probeId());
        const float defGain = rv (p, tabby::laneParamId (0, 0, "gain"));
        setReal (p, probeId(), 2000.0f); settle (p);

        juce::ValueTree sparse ("PARAMS");                     // only ONE param child, no probe child
        juce::ValueTree n ("PARAM");
        n.setProperty ("id", tabby::laneParamId (0, 0, "gain"), nullptr);
        n.setProperty ("value", -6.0, nullptr);
        sparse.addChild (n, -1, nullptr);

        p.pasteState (0, sparse);
        check (std::abs (rv (p, probeId()) - def) < 1e-3f, "5b: absent params reset to their defaults");
        check (std::abs (rv (p, tabby::laneParamId (0, 0, "gain")) - (-6.0f)) < 1e-3f, "5b: present params apply");
        check (p.undo()
                   && std::abs (rv (p, tabby::laneParamId (0, 0, "gain")) - defGain) < 1e-3f
                   && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f,
               "5b: the sparse paste is ONE undoable step (both the applied and the defaulted params revert)");
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
        check (p.compareHistory().hasUnsavedChanges(), "10: a REJECTED load must not falsely clean a dirty session");

        // A hostile blob can pass the ENVELOPE validation with a garbage <Live> payload — the
        // consumer's applyLiveState must refuse to install a foreign tree as the live state.
        juce::ValueTree hostile ("Workspace");
        hostile.setProperty ("schema", 1, nullptr);
        hostile.setProperty ("count", 4, nullptr);
        hostile.setProperty ("active", 0, nullptr);
        juce::ValueTree live ("Live");
        live.appendChild (juce::ValueTree ("Garbage"), nullptr);
        hostile.appendChild (live, nullptr);
        loadState (p, treeToBinary (hostile));
        check (std::abs (rv (p, probeId()) - 4000.0f) < 1e-3f, "10: a garbage <Live> payload never becomes the live state");
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
        check (p.getBandActiveLane (0) == 3, "12: the processor-side ATOM follows the property (drives solo)");
        p.switchToSnapshot (1);
        check ((int) p.apvts.state.getProperty (tabby::bandId (0, "activeLane"), -1) == 4,
               "12: register B restores its own active-lane property");
        check (p.getBandActiveLane (0) == 4, "12: the atom follows across the switch back");
    }

    // ====== 12b. applyLiveState re-syncs the analyzer-domain mirror (specDomain property -> atomic) ======
    {
        TabbyEqAudioProcessor p;
        p.apvts.state.setProperty ("specDomain", 2, nullptr);   // Side — as the editor's view menu writes it
        p.setSpectrumDomain (2);
        p.switchToSnapshot (1);
        p.apvts.state.setProperty ("specDomain", 1, nullptr);   // Mid on register B
        p.setSpectrumDomain (1);
        p.switchToSnapshot (0);
        check (p.getSpectrumDomain() == 2, "12b: register A restores its analyzer domain (atomic follows the property)");
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

    // ====== 14. Host program-change MID-GESTURE (release-mode recovery; the engine jasserts in
    // debug — loading inside an open scope is documented misuse, but a host can still do it) ======
   #if ! JUCE_DEBUG
    {
        TabbyEqAudioProcessor p;
        setReal (p, probeId(), 2000.0f); settle (p);
        const auto blob = saveState (p);                       // v4 blob: probe = 2000

        setReal (p, probeId(), 3000.0f); settle (p);
        p.beginHistoryGesture ("Drag");
        setReal (p, probeId(), 3500.0f);                       // mid-drag...
        loadState (p, blob);                                   // ...host switches the program
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "14: the loaded state wins over the open gesture");
        p.endHistoryGesture();                                 // the editor's mouse-up still arrives — must be a safe no-op
        check (! p.canUndo() && ! p.canRedo(), "14: a mid-gesture load is still a fresh history boundary");
        for (int i = 0; i < 20; ++i) p.undoTick();
        check (p.undoDepth() == 0, "14: the discarded gesture leaves no phantom step behind");
    }
   #endif

    // ====== 15. Newer-schema envelope: best-effort load (forward compat) ======
    {
        TabbyEqAudioProcessor donor;
        setReal (donor, probeId(), 3210.0f); settle (donor);
        auto ws = juce::ValueTree::fromXml (*parseState (saveState (donor)));
        ws.setProperty ("schema", 2, nullptr);                 // a future build's envelope

        TabbyEqAudioProcessor p;
        loadState (p, treeToBinary (ws));
        check (std::abs (rv (p, probeId()) - 3210.0f) < 1e-3f, "15: a newer-schema envelope still loads best-effort");
    }

    // ====== 16. Register-count mismatch: extras dropped, callback fires (skip-not-clamp) ======
    {
        TabbyEqAudioProcessor donor;
        setReal (donor, probeId(), 2000.0f); settle (donor);
        auto ws = juce::ValueTree::fromXml (*parseState (saveState (donor)));
        ws.setProperty ("count", 6, nullptr);                  // a 6-register build's session
        auto snaps = ws.getChildWithName ("Snaps");
        juce::ValueTree s5 ("Snap");
        s5.setProperty ("i", 5, nullptr);
        s5.appendChild (donor.apvts.copyState(), nullptr);     // a dialed-in out-of-range register
        snaps.appendChild (s5, nullptr);

        TabbyEqAudioProcessor p;
        int savedC = 0, buildC = 0;
        p.compareHistory().onRegisterCountMismatch = [&] (int s, int b) { savedC = s; buildC = b; };
        loadState (p, treeToBinary (ws));
        check (savedC == 6 && buildC == 4, "16: onRegisterCountMismatch fires with (saved, build)");
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "16: the in-range workspace still loads");
    }

    // ====== 16b. Gesture brackets drain the link-mirror FIFO onto the correct side of the step ======
    // The processor's 30 Hz drain timer never fires in this loop-less harness, so any mirror that
    // lands is proof the BRACKET drained it. Regression: remove drainLinkFifo() from the brackets
    // and both mirror checks below fail (the linked lane stays put).
    {
        TabbyEqAudioProcessor p;
        const juce::String linkedId = tabby::laneParamId (0, 1, "freq");   // the L lane mirrors ST when linked
        p.apvts.state.setProperty (tabby::bandId (0, "linkFq"), true, nullptr);
        setReal (p, tabby::laneParamId (0, 1, "on"), 1.0f);                // enable the mirror target lane
        settle (p);

        setReal (p, probeId(), 2000.0f);                                    // pre-gesture tweak: queues a mirror event
        p.beginHistoryGesture ("Drag");                                     // must drain: mirror joins the flushed pre-burst
        check (std::abs (rv (p, linkedId) - 2000.0f) < 1e-3f, "16b: a pending mirror lands BEFORE the gesture baseline");
        const int depthInGesture = p.undoDepth();

        setReal (p, probeId(), 2500.0f);                                    // mid-drag: queues another mirror event
        p.endHistoryGesture();                                              // must drain: mirror folds INTO the gesture step
        check (std::abs (rv (p, linkedId) - 2500.0f) < 1e-3f, "16b: the drag's last mirror folds into the gesture step");
        check (p.undoDepth() == depthInGesture + 1, "16b: still exactly one gesture step");

        check (p.undo(), "16b: the gesture step undoes");
        check (std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f && std::abs (rv (p, linkedId) - 2000.0f) < 1e-3f,
               "16b: undo reverts the drag INCLUDING its mirror (one step, both lanes)");
    }

    // ====== 17. Redo stack survives a register switch round-trip ======
    {
        TabbyEqAudioProcessor p;
        const float def = rv (p, probeId());
        setReal (p, probeId(), 2000.0f); settle (p);
        p.undo();
        check (p.canRedo() && std::abs (rv (p, probeId()) - def) < 1e-3f, "17: redo armed after undo");
        p.switchToSnapshot (1);
        p.switchToSnapshot (0);
        check (p.canRedo(), "17: A's redo stack survives the switch round-trip");
        check (p.redo() && std::abs (rv (p, probeId()) - 2000.0f) < 1e-3f, "17: redo still applies after the round-trip");
    }

    // ====== 18. Crew-round UI seams: the active-lane echo guard, suppressed reset, gesture gate ======
    {
        TabbyEqAudioProcessor p;
        const float def = rv (p, probeId());

        // (a) The editor re-fires its lane binding on every history re-sync; an exact-value echo
        // must be a TRUE no-op — a suppressed write that "moves" the state clears the fresh redo.
        p.setBandActiveLane (0, 3);
        setReal (p, probeId(), 2000.0f); settle (p);
        p.undo();
        check (p.canRedo(), "18: redo armed after undo");
        p.setBandActiveLane (0, 3);                        // exact echo of the restored value
        for (int i = 0; i < 20; ++i) p.undoTick();
        check (p.canRedo(), "18: a no-op active-lane echo keeps the redo stack (crew P1 guard)");

        // (a2) The guard must also keep a PENDING burst pending — without it, the suppress scope's
        // pre-flush promotes the burst to a step immediately. This is the mutation-killer: the
        // plain echo in (a) passes even without the guard (an exact-value setProperty is already a
        // ValueTree no-op); this check does not.
        check (p.redo(), "18: redo still applies");
        const int depthA = p.undoDepth();
        setReal (p, probeId(), 2600.0f);               // unsettled burst
        p.setBandActiveLane (0, 3);                    // exact echo again — must not flush it
        check (p.undoDepth() == depthA, "18: an echo never flushes a pending burst (guard mutation-killer)");
        settle (p);
        check (p.undoDepth() == depthA + 1, "18: the burst then settles normally");

        // (b) Reset is a suppressed bulk write: defaults land, NO undo step records, nothing
        // phantom settles afterwards.
        const int depth = p.undoDepth();
        p.resetAllToDefaults();
        check (std::abs (rv (p, probeId()) - def) < 1e-3f, "18: reset restores the defaults");
        check (p.undoDepth() == depth, "18: reset records no undo step");
        for (int i = 0; i < 20; ++i) p.undoTick();
        check (p.undoDepth() == depth, "18: reset leaves no pending phantom step");

        // (c) The open-gesture gate the editor's navigation keys check.
        check (! p.historyGestureOpen(), "18: no gesture open at rest");
        p.beginHistoryGesture ("T");
        check (p.historyGestureOpen(), "18: the gate reflects an open bracket");
        p.endHistoryGesture();
        check (! p.historyGestureOpen(), "18: the gate clears at end");
    }

    if (failures == 0)
        std::cout << "TabbyEQ CompareHistory adapter harness: ALL OK\n";
    else
        std::cerr << "TabbyEQ CompareHistory adapter harness: " << failures << " FAILURE(S)\n";
    return failures == 0 ? 0 : 1;
}
