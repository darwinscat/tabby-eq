// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/EqCurveDisplay.h"
#include "ui/BandEditStrip.h"
#include "ui/LevelMeter.h"

//==============================================================================
// TabbyEQ editor — for now: the classic analyzer + response-curve canvas, plus an Output trim.
// The semantic layer (source/role pickers, trait knobs, search->treat) lands on top next.
class TabbyEqEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TabbyEqEditor (TabbyEqAudioProcessor& p);
    ~TabbyEqEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showViewMenu();
    void resetAll();          // clear every band + output to defaults (temporary dev convenience)
    void toggleFullscreen();  // real borderless fullscreen (kiosk) — standalone only

    TabbyEqAudioProcessor& proc;

    EqCurveDisplay display;
    BandEditStrip  strip;
    LevelMeter     inMeter  { proc, LevelMeter::Which::In };    // IN rail (left): meter only
    LevelMeter     outMeter { proc, LevelMeter::Which::Out };   // OUT rail (right): meter + trim
    juce::Label    inCap, outCap;
    juce::Label    title;
    juce::Slider   output { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton prePost;
    juce::TextButton viewButton;
    juce::TextButton resetButton;
    juce::TextButton fullButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqEditor)
};
