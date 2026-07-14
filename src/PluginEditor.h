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
#include "ui/ChromeButtons.h"        // FlatItem / GlyphButton — the flat bottom-bar + top-right glyphs
#include "ui/TabbyMark.h"            // the product brand mark, drawn inside the blister frame
#include "chrome/ChromeMetrics.h"    // ChromeMetrics (barHeight) + ChromeTheme
#include "chrome/BrandBlister.h"     // the brand-blister FRAME
#include "chrome/ChromeUnderline.h"  // the shell's continuous toolbar-bottom hairline overlay
#include "chrome/ChromeBar.h"        // the top-bar zone layout (RigidCenter / RightPinned)
#include "chrome/CompareCell.h"      // undo/redo + A/B/C/D compare registers (pushed model)
#include "chrome/PresetCell.h"       // preset name + save/import/export (product model + actions)

//==============================================================================
// TabbyEQ editor — for now: the classic analyzer + response-curve canvas, plus an Output trim.
// The semantic layer (source/role pickers, trait knobs, search->treat) lands on top next.
//
// The top toolbar/blister "chrome" is now assembled from the extractable tabby::chrome layer: the
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

    // ---- A/B/C/D compare registers (see chrome/CompareCell.h; engine seams on the processor) ----
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

    // ---- top chrome (the extractable tabby::chrome layer) ------------------------------------
    // chromeMetrics/chromeTheme MUST precede the frame + overlay (they hold const refs to them);
    // the frame MUST precede the underline (it references the frame).
    tabby::chrome::ChromeMetrics  chromeMetrics;                                   // ONE bar/blister geometry source
    tabby::chrome::ChromeTheme    chromeTheme { tabby::palette::bg(), juce::Colours::white.withAlpha (0.07f) };
    std::unique_ptr<juce::Drawable> catLogo;                                       // Darwin's Cat mark (BinaryData SVG)
    tabby::ui::TabbyMark          mark;                                            // the product mark drawn inside the frame
    tabby::chrome::BrandBlister   blister   { chromeMetrics, chromeTheme };        // [cat] TabbyEQ blister frame → website
    tabby::chrome::ChromeUnderline underline { blister, chromeMetrics, chromeTheme };   // the one continuous toolbar-bottom line
    tabby::chrome::CompareCell    compareCell { TabbyEqAudioProcessor::kNumSnapshots }; // undo/redo + A/B/C/D
    tabby::chrome::PresetCell     presetCell;                                      // preset name + save/import/export

    juce::TooltipWindow tooltips { this, 700 };                                // hosts the undo-label (and phase-blend) tooltips
    unsigned lastHistoryRev = 0;                                               // last seen historyRevision()
    unsigned lastApplyRev   = 0;                                               // last seen applyRevision()
    LevelMeter     inMeter  { proc, LevelMeter::Which::In };    // IN rail (left): meter only
    LevelMeter     outMeter { proc, LevelMeter::Which::Out };   // OUT rail (right): meter + trim
    CorrelationMeter corrMeter { proc };                        // top-bar L/R phase correlation
    juce::Slider   output { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };

    // ---- bottom toolbar (FabFilter-style flat items; every popup opens upward) ----
    tabby::ui::FlatItem modeItem;              // "Zero Latency" / "Natural Phase – 70" / "Linear Phase – High"
    juce::Label latencyLabel;                  // reported latency — yellow (Natural) / red (Linear), beside the mode
    tabby::ui::FlatItem anaItem;               // "Analyzer: Post" -> settings popover
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
