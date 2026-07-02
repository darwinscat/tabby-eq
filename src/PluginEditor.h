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

//==============================================================================
// TabbyEQ editor — for now: the classic analyzer + response-curve canvas, plus an Output trim.
// The semantic layer (source/role pickers, trait knobs, search->treat) lands on top next.
class TabbyEqEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TabbyEqEditor (TabbyEqAudioProcessor& p);
    ~TabbyEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showViewMenu();
    void resetAll();          // clear every band + output to defaults (temporary dev convenience)
    void toggleFullscreen();  // real borderless fullscreen (kiosk) — standalone only
    void updatePhaseUi();      // refresh the latency readout + grey the quality combo when not Linear

    TabbyEqAudioProcessor& proc;

    EqCurveDisplay display;
    BandEditStrip  strip;
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
