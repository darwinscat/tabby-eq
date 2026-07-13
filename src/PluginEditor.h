// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/EqCurveDisplay.h"
#include "ui/BandEditStrip.h"
#include "ui/LevelMeter.h"
#include "ui/CorrelationMeter.h"
#include "ui/VersionInfo.h"
#include "ui/SnapshotButton.h"

//==============================================================================
// Undo / redo buttons — self-painted curved arrows. Unicode arrow glyphs (↶/↷ etc.) render as
// mismatched emoji or tofu depending on the host's font stack, so the icon is a Path instead.
class HistoryArrowButton final : public juce::TextButton
{
public:
    explicit HistoryArrowButton (bool pointsRight) : redoArrow (pointsRight) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        TextButton::paintButton (g, highlighted, down);   // stock background (no text set)

        const auto  b   = getLocalBounds().toFloat();
        const auto  c   = b.getCentre();
        const float rad = juce::jmin (b.getWidth(), b.getHeight()) * 0.30f;

        // A 3/4 arc, clockwise from lower-left past 12 o'clock, with a tangent-aligned arrowhead
        // at the end — the classic "redo" shape; undo is its mirror.
        const float a0 = -2.4f, a1 = 1.0f;                 // radians, clockwise from 12 o'clock
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, a1, true);

        const juce::Point<float> end  (c.x + rad * std::sin (a1), c.y - rad * std::cos (a1));
        const juce::Point<float> dir  (std::cos (a1), std::sin (a1));   // clockwise tangent at a1
        const juce::Point<float> perp (-dir.y, dir.x);
        const float hl = rad * 0.85f, hw = rad * 0.55f;
        juce::Path head;
        head.addTriangle (end + dir * hl, end + perp * hw, end - perp * hw);

        if (! redoArrow)
        {
            const auto flip = juce::AffineTransform::scale (-1.0f, 1.0f, c.x, c.y);
            arc.applyTransform (flip);
            head.applyTransform (flip);
        }

        g.setColour (findColour (juce::TextButton::textColourOffId).withAlpha (isEnabled() ? 1.0f : 0.35f));
        g.strokePath (arc, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillPath (head);
    }

private:
    bool redoArrow;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryArrowButton)
};

//==============================================================================
// TabbyEQ editor — for now: the classic analyzer + response-curve canvas, plus an Output trim.
// The semantic layer (source/role pickers, trait knobs, search->treat) lands on top next.
class TabbyEqEditor final : public juce::AudioProcessorEditor,
                            public juce::DragAndDropContainer,   // A/B/C/D copy-by-drag between the register buttons
                            private juce::Timer
{
public:
    explicit TabbyEqEditor (TabbyEqAudioProcessor& p);
    ~TabbyEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // 10 Hz editor pump: drives the undo settle timer (CompareHistory counts ticks, not wall-clock;
    // every tick deep-copies the 820-param tree, so the pump is deliberately slow — settleTicks is
    // sized to keep the same ~0.4 s settle window). History only settles while an editor is open —
    // an edit made with the UI closed stays one pending burst and commits on the next open (or is
    // flushed by the next history operation). The revision poll re-syncs the history UI (A/B/C/D
    // markers etc.) only when the engine reports a change — no per-tick rescans.
    void timerCallback() override
    {
        proc.undoTick();
        if (const auto r = proc.historyRevision(); r != lastHistoryRev)
        {
            lastHistoryRev = r;
            updateSnapshotButtons();
            refreshHistoryUi();
        }
        if (const auto r = proc.applyRevision(); r != lastApplyRev)
        {
            lastApplyRev = r;
            display.refreshAfterLaneEdit();   // an apply replaced the live state wholesale
            syncViewFromState();              // view properties may have travelled with it
            revalidateSolo();                 // the soloed band may not exist in the applied state
        }
    }

    // ---- A/B/C/D compare registers (see ui/SnapshotButton.h; engine seams on the processor) ----
    void updateSnapshotButtons();                 // reflect the active register + edited dots
    void switchSnapshot (int i);                  // recall register i + re-sync the display
    void showSnapshotMenu (int i);                // right-click on button i: copy/paste menu
    void applySnapshotCopy (int from, int to);    // copyRegister + UI re-sync
    void copySnapshotToClipboard (int i);         // register i's state XML → system clipboard
    bool pasteSnapshotFromClipboard (int toReg);  // clipboard → register (validated)

    // ---- undo / redo UI ----
    bool keyPressed (const juce::KeyPress&) override;   // ⌘Z/⇧⌘Z undo/redo · 1-4 registers · ⌘C/⌘V clipboard
    void refreshHistoryUi();                      // undo/redo enablement + peek-label tooltips
    void syncViewFromState();                     // re-read the view properties the state tree carries
    void afterHistoryNav();                       // full editor re-sync after undo/redo/switch/copy/paste
    void revalidateSolo();                        // clear a solo whose band a history apply turned off
    bool historyNavBlocked() const;               // any gesture open — keys AND pointer nav paths are inert

    void showViewMenu();
    void resetAll();          // clear every band + output to defaults (temporary dev convenience)
    void toggleFullscreen();  // real borderless fullscreen (kiosk) — standalone only
    void updatePhaseUi();      // refresh the latency readout + grey the quality combo when not Linear

    TabbyEqAudioProcessor& proc;

    EqCurveDisplay display;
    BandEditStrip  strip;
    tabby::ui::SnapshotButton snapBtn[TabbyEqAudioProcessor::kNumSnapshots];   // A/B/C/D compare registers
    HistoryArrowButton undoBtn { false }, redoBtn { true };                    // curved-arrow undo/redo, next to A/B/C/D
    juce::TooltipWindow tooltips { this, 700 };                                // hosts the undo-label (and phase-blend) tooltips
    unsigned lastHistoryRev = 0;                                               // last seen historyRevision()
    unsigned lastApplyRev   = 0;                                               // last seen applyRevision()
    LevelMeter     inMeter  { proc, LevelMeter::Which::In };    // IN rail (left): meter only
    LevelMeter     outMeter { proc, LevelMeter::Which::Out };   // OUT rail (right): meter + trim
    CorrelationMeter corrMeter { proc };                        // top-bar L/R phase correlation
    juce::Label    inCap, outCap;
    juce::Label    title;
    juce::Slider   output { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton prePost;
    juce::ComboBox   phaseCombo;                                        // Zero Latency / Natural Phase / Linear Phase
    juce::ComboBox   qualityCombo;                                      // linear-phase FIR quality (Low..Max) — Linear only
    juce::Slider     phaseAmountSlider;                                 // Natural blend k (0 linear … 1 min phase) — Natural only
    juce::Label      latencyLabel;                                      // reported latency — yellow (Natural) / red (Linear)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> phaseAtt, qualityAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   phaseAmountAtt;
    juce::TextButton viewButton;
    juce::TextButton resetButton;
    juce::TextButton fullButton;
    tabby::InfoButton infoButton { proc.updateChecker() };   // (i) — build/version + update-check popover, top-bar right (near POST)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqEditor)
};
