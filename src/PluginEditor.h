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
#include "ui/ChromeButtons.h"        // GlyphButton — the product's top-right gear/fullscreen glyphs
#include "ui/TabbyMark.h"            // the product brand mark, drawn inside the blister frame
#include <felitronics/appkit/chrome/ChromeMetrics.h>    // ChromeMetrics (barHeight) + ChromeTheme
#include <felitronics/appkit/chrome/BrandBlister.h>     // the brand-blister FRAME
#include <felitronics/appkit/chrome/ChromeUnderline.h>  // the shell's continuous toolbar-bottom hairline overlay
#include <felitronics/appkit/chrome/ChromeBar.h>        // the top-bar zone layout (RigidCenter / RightPinned)
#include <felitronics/appkit/chrome/FlatButtons.h>      // FlatItem — the flat bottom-bar items
#include <felitronics/appkit/chrome/CompareCell.h>      // undo/redo + A/B/C/D compare registers (pushed model)
#include <felitronics/appkit/chrome/PresetCell.h>       // preset name (product model + actions)

//==============================================================================
// TabbyEQ editor — for now: the classic analyzer + response-curve canvas, plus an Output trim.
// The semantic layer (source/role pickers, trait knobs, search->treat) lands on top next.
//
// The top toolbar/blister "chrome" is now assembled from the felitronics::appkit::chrome layer: the
// editor is the SHELL — it owns the ChromeMetrics/Theme, the brand blister FRAME (+ its product
// mark), the compare + preset cells, and the top-most underline overlay; ChromeBar lays the three
// zones out in resized(). The compare cell is its OWN DragAndDropContainer, so the editor no longer
// is one.
class TabbyEqEditor final : public juce::AudioProcessorEditor,
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
            pushCompareModel();               // active/edited markers + undo/redo enable + peek labels
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

    // ---- A/B/C/D compare registers (see appkit chrome/CompareCell.h; engine seams on the processor) ----
    void pushCompareModel();                      // build + push the CompareModel into the compare cell
    void switchSnapshot (int i);                  // recall register i + re-sync the display
    void showSnapshotMenu (int i);                // right-click on button i: copy/paste menu
    void applySnapshotCopy (int from, int to);    // copyRegister + UI re-sync
    void copySnapshotToClipboard (int i);         // register i's state XML → system clipboard
    bool pasteSnapshotFromClipboard (int toReg);  // clipboard → register (validated)

    // ---- undo / redo UI ----
    bool keyPressed (const juce::KeyPress&) override;   // ⌘Z/⇧⌘Z undo/redo · 1-4 registers · ⌘C/⌘V clipboard
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

    // ---- top chrome (the felitronics::appkit::chrome layer) ------------------------------------
    // chromeMetrics/chromeTheme MUST precede the frame + overlay (which COPY them by value at
    // construction); the frame MUST precede the underline (the underline subscribes to it).
    felitronics::appkit::chrome::ChromeMetrics chromeMetrics;                      // ONE bar/blister geometry source
    // The 7-field appkit ChromeTheme, seeded from THE PRODUCT PALETTE (guardrail: NOT appkit::brand —
    // its violet/orange differ by a few LSBs and would drift the pixel-compare). These reproduce the
    // pre-extraction hardcoded palette values exactly.
    felitronics::appkit::chrome::ChromeTheme   chromeTheme {
        .fill       = tabby::palette::bg(),
        .underline  = juce::Colours::white.withAlpha (0.07f),
        .accent     = tabby::palette::violet(),        // active-register frame (0xff9170ff)
        .attention  = tabby::palette::orange(),        // edited dot + drop ring (0xffff8822)
        .text       = tabby::palette::text(),
        .textDim    = tabby::palette::textDim(),
        .activeText = juce::Colours::white };
    std::unique_ptr<juce::Drawable> catLogo;                                       // Darwin's Cat mark (BinaryData SVG)
    tabby::ui::TabbyMark          mark;                                            // the product mark drawn inside the frame
    felitronics::appkit::chrome::BrandBlister   blister   { chromeMetrics, chromeTheme };        // [cat] TabbyEQ blister frame → website
    felitronics::appkit::chrome::ChromeUnderline underline { blister, chromeMetrics, chromeTheme };   // the one continuous toolbar-bottom line
    felitronics::appkit::chrome::CompareCell    compareCell { TabbyEqAudioProcessor::kNumSnapshots, chromeTheme }; // undo/redo + A/B/C/D
    felitronics::appkit::chrome::PresetCell     presetCell { chromeTheme };                     // preset name

    juce::TooltipWindow tooltips { this, 700 };                                // hosts the undo-label (and phase-blend) tooltips
    unsigned lastHistoryRev = 0;                                               // last seen historyRevision()
    unsigned lastApplyRev   = 0;                                               // last seen applyRevision()
    LevelMeter     inMeter  { proc, LevelMeter::Which::In };    // IN rail (left): meter only
    LevelMeter     outMeter { proc, LevelMeter::Which::Out };   // OUT rail (right): meter + trim
    CorrelationMeter corrMeter { proc };                        // top-bar L/R phase correlation
    juce::Slider   output { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };

    // ---- bottom toolbar (FabFilter-style flat items; every popup opens upward) ----
    felitronics::appkit::chrome::FlatItem modeItem;  // "Zero Latency" / "Natural Phase – 70" / "Linear Phase – High"
    juce::Label latencyLabel;                  // reported latency — yellow (Natural) / red (Linear), beside the mode
    felitronics::appkit::chrome::FlatItem anaItem;   // "Analyzer: Post" -> settings popover
    juce::String currentPresetName { "Default" };
    std::unique_ptr<juce::FileChooser> chooser;                         // async save/import/export
    int   lastPhaseMode = -1, lastPhaseQuality = -1;                    // timer poll caches (labels follow
    float lastPhaseAmount = -1.0f;                                      // host automation, not just the menu)

    // ---- top-right pinned glyphs ----
    tabby::ui::GlyphButton gearBtn { tabby::ui::GlyphButton::Glyph::Gear };         // View options menu (flat, top-right)
    tabby::ui::GlyphButton fullBtn { tabby::ui::GlyphButton::Glyph::Fullscreen };   // kiosk fullscreen — standalone only
    tabby::InfoButton infoButton { proc.updateChecker() };   // (i) — build/version + update-check popover, top-bar right (near POST)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqEditor)
};
