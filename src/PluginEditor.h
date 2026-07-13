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
#include "ui/HeaderBrand.h"

//==============================================================================
// A flat text item (bottom toolbar / unframed chrome): no background, no outline — just the label,
// dim at rest and brightening on hover, FabFilter-style. Popups launched off these open upward at
// the window's bottom edge automatically.
class FlatItem : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        g.setColour ((highlighted ? tabby::palette::text() : tabby::palette::textDim())
                         .withAlpha (isEnabled() ? 1.0f : 0.4f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
    }
};

//==============================================================================
// Undo / redo buttons — self-painted curved arrows. Unicode arrow glyphs (↶/↷ etc.) render as
// mismatched emoji or tofu depending on the host's font stack, so the icon is a Path instead.
class HistoryArrowButton final : public juce::TextButton
{
public:
    explicit HistoryArrowButton (bool pointsRight) : redoArrow (pointsRight) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        // FLAT — no stock frame; a soft tint on hover, the arrow dimmed when disabled.
        if ((highlighted || down) && isEnabled())
        {
            g.setColour (tabby::palette::text().withAlpha (down ? 0.16f : 0.08f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
        }

        const auto  b   = getLocalBounds().toFloat();
        const auto  c   = b.getCentre();
        const float rad = juce::jmin (b.getWidth(), b.getHeight()) * 0.26f;

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

        g.setColour ((highlighted ? tabby::palette::text() : tabby::palette::textDim())
                         .withAlpha (isEnabled() ? 1.0f : 0.35f));
        g.strokePath (arc, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillPath (head);
    }

private:
    bool redoArrow;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryArrowButton)
};

//==============================================================================
// A flat glyph button (gear / fullscreen corner-brackets): no frame, just the glyph, dim at rest
// and brightening on hover — the unframed top-right chrome.
class GlyphButton final : public juce::Button
{
public:
    enum class Glyph { Gear, Fullscreen };
    explicit GlyphButton (Glyph g) : juce::Button ({}), glyph (g) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        g.setColour ((highlighted ? tabby::palette::text() : tabby::palette::textDim())
                         .withAlpha (isEnabled() ? 1.0f : 0.4f));
        const auto b = getLocalBounds().toFloat().reduced (4.5f);
        if (glyph == Glyph::Gear) drawGear (g, b);
        else                      drawBrackets (g, b);
    }

private:
    static void drawGear (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto  c = b.getCentre();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        g.drawEllipse (c.x - r * 0.58f, c.y - r * 0.58f, r * 1.16f, r * 1.16f, r * 0.34f);   // ring (hole free)
        juce::Path teeth;
        for (int i = 0; i < 8; ++i)   // 8 stub teeth around the ring
        {
            juce::Path t;
            t.addRoundedRectangle (-r * 0.13f, -r, r * 0.26f, r * 0.34f, r * 0.08f);
            teeth.addPath (t, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::twoPi / 8.0f)
                                  .translated (c.x, c.y));
        }
        g.fillPath (teeth);
    }

    static void drawBrackets (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const float L = juce::jmin (b.getWidth(), b.getHeight()) * 0.34f;   // corner arm length
        juce::Path p;
        p.startNewSubPath (b.getX(), b.getY() + L);                p.lineTo (b.getX(), b.getY());                p.lineTo (b.getX() + L, b.getY());
        p.startNewSubPath (b.getRight() - L, b.getY());            p.lineTo (b.getRight(), b.getY());            p.lineTo (b.getRight(), b.getY() + L);
        p.startNewSubPath (b.getRight(), b.getBottom() - L);       p.lineTo (b.getRight(), b.getBottom());       p.lineTo (b.getRight() - L, b.getBottom());
        p.startNewSubPath (b.getX() + L, b.getBottom());           p.lineTo (b.getX(), b.getBottom());           p.lineTo (b.getX(), b.getBottom() - L);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    Glyph glyph;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlyphButton)
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

        // The bottom-bar mode/latency labels follow the PARAMS (host automation moves them too).
        const int   pm = (int) (proc.apvts.getRawParameterValue ("phaseMode")->load() + 0.5f);
        const int   pq = (int) (proc.apvts.getRawParameterValue ("lpQuality")->load() + 0.5f);
        const float pk = proc.apvts.getRawParameterValue ("phaseAmount")->load();
        if (pm != lastPhaseMode || pq != lastPhaseQuality || std::abs (pk - lastPhaseAmount) > 1.0e-4f)
        {
            lastPhaseMode = pm; lastPhaseQuality = pq; lastPhaseAmount = pk;
            updatePhaseUi();
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
    void resetAll();          // load the "Default" preset: every band + output to defaults
    void toggleFullscreen();  // real borderless fullscreen (kiosk) — standalone only
    void updatePhaseUi();     // refresh the mode item label + the latency readout from the params

    // ---- bottom toolbar actions ----
    void showModeMenu();          // Zero / Natural (k submenu) / Linear (quality submenu)
    void showPresetMenu();        // Default + *.tabbyeq files from the preset directory
    void showAnalyzerPanel();     // the Pre/Post + Range/Speed/Tilt/Freeze popover
    void refreshAnalyzerItem();   // "Analyzer: Pre+Post|Pre|Post|Off"
    void doSavePreset();          // FileChooser into the preset dir
    void doImportPreset();        // pick a file -> load + copy into the preset dir
    void doExportPreset();        // save the current sound anywhere

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
    HeaderBrand    brand;                    // [stripe-cat mark] TabbyEQ · by Darwin's Cat / Semantic EQ → website
    juce::Slider   output { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };

    // ---- bottom toolbar (FabFilter-style flat items; every popup opens upward) ----
    FlatItem   modeItem;                       // "Zero Latency" / "Natural Phase – 70" / "Linear Phase – High"
    juce::Label latencyLabel;                  // reported latency — yellow (Natural) / red (Linear), beside the mode
    FlatItem   presetItem { "Default" };       // preset name -> menu (Default + user presets)
    FlatItem   saveItem { "Save" }, importItem { "Import" }, exportItem { "Export" };
    FlatItem   anaItem;                        // "Analyzer: Post" -> settings popover
    juce::String currentPresetName { "Default" };
    std::unique_ptr<juce::FileChooser> chooser;                         // async save/import/export
    int   lastPhaseMode = -1, lastPhaseQuality = -1;                    // timer poll caches (labels follow
    float lastPhaseAmount = -1.0f;                                      // host automation, not just the menu)
    GlyphButton gearBtn { GlyphButton::Glyph::Gear };         // View options menu (flat, top-right)
    GlyphButton fullBtn { GlyphButton::Glyph::Fullscreen };   // kiosk fullscreen — standalone only
    tabby::InfoButton infoButton { proc.updateChecker() };   // (i) — build/version + update-check popover, top-bar right (near POST)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqEditor)
};
