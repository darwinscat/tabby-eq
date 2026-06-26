// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

#include <memory>

//==============================================================================
// The selected-band inspector strip: shows AND keyboard-edits the real freq / Q / gain / type /
// slope of whichever band is selected on the curve (#8 — values are first-class, not hidden). One
// set of controls serves all 24 bands: when the selection changes it rebinds its APVTS attachments
// to the new band's parameters, so the strip and the draggable node stay in lock-step for free.
class BandEditStrip : public juce::Component
{
public:
    explicit BandEditStrip (TabbyEqAudioProcessor& p);
    ~BandEditStrip() override;

    void setBand (int band);                 // -1 = nothing selected (controls disabled)

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void rebind();                           // (re)create attachments for the current band
    void updateForType();                    // slope shown only for HP/LP; gain/Q enabled by type

    TabbyEqAudioProcessor& proc;
    int curBand = -1;

    juce::Label        title;
    juce::ToggleButton onButton { "On" };
    juce::TextButton   soloButton { "S" };
    juce::ComboBox     typeBox, slopeBox;
    juce::Slider       freq, q, gain;
    juce::Label        freqCap, qCap, gainCap;
    juce::Rectangle<int> iconBounds;   // the filter-shape icon, left of the type box

    std::unique_ptr<ButtonAtt> onAtt;
    std::unique_ptr<ComboAtt>  typeAtt, slopeAtt;
    std::unique_ptr<SliderAtt> freqAtt, qAtt, gainAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandEditStrip)
};
