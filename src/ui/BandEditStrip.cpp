// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"
#include "ui/Palette.h"

BandEditStrip::BandEditStrip (TabbyEqAudioProcessor& p) : proc (p)
{
    title.setText ("—", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    title.setColour (juce::Label::textColourId, tabby::palette::text());
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);

    onButton.setColour (juce::ToggleButton::tickColourId, tabby::palette::violet());
    addAndMakeVisible (onButton);

    const char* types[] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
    for (int i = 0; i < 9; ++i) typeBox.addItem (types[i], i + 1);
    typeBox.onChange = [this] { updateForType(); };
    addAndMakeVisible (typeBox);

    const char* slopes[] = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
    for (int i = 0; i < 7; ++i) slopeBox.addItem (slopes[i], i + 1);
    addAndMakeVisible (slopeBox);

    auto setupField = [this] (juce::Slider& s, juce::Label& cap, const juce::String& capText,
                              const juce::String& suffix, int decimals)
    {
        s.setSliderStyle (juce::Slider::LinearBar);          // shows the value; double-click to type
        s.setTextValueSuffix (suffix);
        s.setNumDecimalPlacesToDisplay (decimals);
        s.setColour (juce::Slider::trackColourId,       tabby::palette::violet().withAlpha (0.45f));
        s.setColour (juce::Slider::textBoxTextColourId, tabby::palette::text());
        addAndMakeVisible (s);

        cap.setText (capText, juce::dontSendNotification);
        cap.setFont (juce::Font (juce::FontOptions (10.0f)));
        cap.setColour (juce::Label::textColourId, tabby::palette::textDim());
        cap.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (cap);
    };
    setupField (freq, freqCap, "FREQ", " Hz", 0);
    setupField (q,    qCap,    "Q",    "",    2);
    setupField (gain, gainCap, "GAIN", " dB", 1);

    setBand (-1);
}

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;

    title.setText (has ? "BAND " + juce::String (curBand + 1) : juce::String ("—"), juce::dontSendNotification);
    juce::Component* controls[] = { &onButton, &typeBox, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);

    rebind();
    updateForType();
    repaint();
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    onAtt.reset(); typeAtt.reset(); slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset();
    if (curBand < 0) return;

    auto id = [this] (juce::StringRef s) { return tabby::bandId (curBand, s); };
    onAtt    = std::make_unique<ButtonAtt> (proc.apvts, id ("on"),    onButton);
    typeAtt  = std::make_unique<ComboAtt>  (proc.apvts, id ("type"),  typeBox);
    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, id ("slope"), slopeBox);
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, id ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("gain"),  gain);
}

void BandEditStrip::updateForType()
{
    if (curBand < 0) { slopeBox.setVisible (false); return; }

    const auto t       = tabby::filterTypeFromChoice (typeBox.getSelectedItemIndex());
    const bool isCut   = (t == teq::FilterType::HighPass || t == teq::FilterType::LowPass);
    const bool isShelf = (t == teq::FilterType::LowShelf || t == teq::FilterType::HighShelf);
    const bool isTilt  = (t == teq::FilterType::Tilt);
    const bool hasGain = (t == teq::FilterType::Bell || isShelf || isTilt);

    slopeBox.setVisible (isCut);                       // slope only applies to HP/LP
    gain.setEnabled (hasGain);                         // HP/LP/BP/notch/all-pass have no gain
    q.setEnabled (! isShelf && ! isTilt && ! isCut);   // shelves/tilt/HP/LP are Butterworth — Q unused
}

void BandEditStrip::paint (juce::Graphics& g)
{
    g.setColour (tabby::palette::panel());
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void BandEditStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 5);
    title.setBounds (r.removeFromLeft (62));
    r.removeFromLeft (6);
    onButton.setBounds (r.removeFromLeft (42).withSizeKeepingCentre (42, 24));
    r.removeFromLeft (8);
    typeBox.setBounds (r.removeFromLeft (104).withSizeKeepingCentre (104, 24));
    r.removeFromLeft (10);

    auto field = [&r] (juce::Label& cap, juce::Slider& s, int w)
    {
        auto col = r.removeFromLeft (w);
        cap.setBounds (col.removeFromTop (12));
        s.setBounds   (col.reduced (0, 1));
        r.removeFromLeft (8);
    };
    field (freqCap, freq, 90);
    field (qCap,    q,    64);
    field (gainCap, gain, 80);
    slopeBox.setBounds (r.removeFromLeft (96).withSizeKeepingCentre (96, 24));
}
