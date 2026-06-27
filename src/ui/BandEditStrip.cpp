// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"
#include "ui/Palette.h"

namespace
{
    const char* kTypeNames[9] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };

    // The response-shape path for a filter type, inside `a` (used for the type-menu icons).
    juce::Path filterShapePath (teq::FilterType t, juce::Rectangle<float> a)
    {
        using FT = teq::FilterType;
        const float x0 = a.getX(), x1 = a.getRight(), w = a.getWidth(), cx = a.getCentreX();
        const float top = a.getY() + 1.0f, bot = a.getBottom() - 1.0f, mid = a.getCentreY();
        juce::Path p;
        switch (t)
        {
            case FT::Bell:      p.startNewSubPath (x0, mid); p.quadraticTo (cx - w * 0.18f, mid, cx, top);
                                p.quadraticTo (cx + w * 0.18f, mid, x1, mid); break;
            case FT::BandPass:  p.startNewSubPath (x0, bot); p.quadraticTo (cx - w * 0.10f, bot, cx, top);
                                p.quadraticTo (cx + w * 0.10f, bot, x1, bot); break;
            case FT::Notch:     p.startNewSubPath (x0, mid); p.quadraticTo (cx - w * 0.10f, mid, cx, bot);
                                p.quadraticTo (cx + w * 0.10f, mid, x1, mid); break;
            case FT::LowShelf:  p.startNewSubPath (x0, top); p.lineTo (cx - w * 0.12f, top);
                                p.quadraticTo (cx, top, cx + w * 0.05f, mid); p.lineTo (x1, mid); break;
            case FT::HighShelf: p.startNewSubPath (x0, mid); p.lineTo (cx - w * 0.05f, mid);
                                p.quadraticTo (cx, top, cx + w * 0.12f, top); p.lineTo (x1, top); break;
            case FT::HighPass:  p.startNewSubPath (x0, bot); p.quadraticTo (cx - w * 0.05f, mid, cx, mid);
                                p.lineTo (x1, mid); break;
            case FT::LowPass:   p.startNewSubPath (x0, mid); p.lineTo (cx, mid);
                                p.quadraticTo (cx + w * 0.05f, mid, x1, bot); break;
            case FT::Tilt:      p.startNewSubPath (x0, bot); p.lineTo (x1, top); break;
            case FT::AllPass:   p.startNewSubPath (x0, mid); p.lineTo (x1, mid); break;
        }
        return p;
    }

    std::unique_ptr<juce::Drawable> makeFilterDrawable (teq::FilterType t)
    {
        auto dp = std::make_unique<juce::DrawablePath>();
        dp->setPath (filterShapePath (t, { 0.0f, 0.0f, 24.0f, 14.0f }));
        dp->setFill (juce::FillType (juce::Colours::transparentBlack));
        dp->setStrokeFill (juce::FillType (tabby::palette::violetLo()));
        dp->setStrokeType (juce::PathStrokeType (1.5f));
        return dp;
    }

    int typeIndexOf (TabbyEqAudioProcessor& proc, int band)
    {
        if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (tabby::bandId (band, "type"))))
            return juce::jlimit (0, 8, prm->getIndex());
        return 0;
    }
}

BandEditStrip::BandEditStrip (TabbyEqAudioProcessor& p) : proc (p)
{
    title.setText ("—", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    title.setColour (juce::Label::textColourId, tabby::palette::text());
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);

    onButton.setColour (juce::ToggleButton::tickColourId, tabby::palette::violet());
    addAndMakeVisible (onButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    soloButton.onClick = [this] { proc.setSoloBand (soloButton.getToggleState() ? curBand : -1); };
    addAndMakeVisible (soloButton);

    // Type: a button (showing the current type) that opens a menu whose items carry shape icons —
    // a plain ComboBox can't render per-item images, so we drive the menu ourselves.
    typeButton.setColour (juce::TextButton::buttonColourId,   tabby::palette::panel().brighter (0.18f));
    typeButton.setColour (juce::TextButton::textColourOffId,  tabby::palette::text());
    typeButton.onClick = [this] { showTypeMenu(); };
    addAndMakeVisible (typeButton);

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

BandEditStrip::~BandEditStrip() { proc.setSoloBand (-1); }   // never leave audio stuck in solo

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;

    title.setText (has ? "BAND " + juce::String (curBand + 1) : juce::String ("—"), juce::dontSendNotification);
    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);
    typeButton.setButtonText (has ? kTypeNames[typeIndexOf (proc, curBand)] : juce::String ("—"));

    rebind();
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    repaint();
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    onAtt.reset(); slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset();
    if (curBand < 0) return;

    auto id = [this] (juce::StringRef s) { return tabby::bandId (curBand, s); };
    onAtt    = std::make_unique<ButtonAtt> (proc.apvts, id ("on"),    onButton);
    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, id ("slope"), slopeBox);
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, id ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("gain"),  gain);
}

void BandEditStrip::showTypeMenu()
{
    if (curBand < 0) return;
    const int cur = typeIndexOf (proc, curBand);

    juce::PopupMenu m;
    for (int i = 0; i < 9; ++i)
    {
        juce::PopupMenu::Item it;
        it.itemID = i + 1;
        it.text   = kTypeNames[i];
        it.setImage (makeFilterDrawable (tabby::filterTypeFromChoice (i)));
        it.setTicked (i == cur);
        m.addItem (it);
    }

    juce::Component::SafePointer<BandEditStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (typeButton),
                     [safe] (int r)
                     {
                         if (safe == nullptr || r <= 0 || safe->curBand < 0) return;
                         if (auto* prm = safe->proc.apvts.getParameter (tabby::bandId (safe->curBand, "type")))
                             prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (r - 1)));
                         safe->typeButton.setButtonText (kTypeNames[r - 1]);
                         safe->updateForType();
                     });
}

void BandEditStrip::updateForType()
{
    if (curBand < 0) { slopeBox.setVisible (false); return; }

    const auto t       = tabby::filterTypeFromChoice (typeIndexOf (proc, curBand));
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
    r.removeFromLeft (4);
    soloButton.setBounds (r.removeFromLeft (28).withSizeKeepingCentre (28, 24));
    r.removeFromLeft (8);
    typeButton.setBounds (r.removeFromLeft (116).withSizeKeepingCentre (116, 24));
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
