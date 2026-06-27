// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"

namespace
{
    const char* kTypeNames[9] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
    const char* kRouteNames[5] = { "Stereo", "Left", "Right", "Mid", "Side" };
    const char* kRouteShort[5] = { "ST", "L", "R", "M", "S" };   // compact label on the button

    int typeIndexOf (TabbyEqAudioProcessor& proc, int band)
    {
        if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (tabby::bandId (band, "type"))))
            return juce::jlimit (0, 8, prm->getIndex());
        return 0;
    }

    int routeIndexOf (TabbyEqAudioProcessor& proc, int band)
    {
        if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (tabby::bandId (band, "route"))))
            return juce::jlimit (0, 4, prm->getIndex());
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
    onButton.setClickingTogglesState (true);
    onButton.onClick = [this]
    {
        if (curBand < 0) return;
        if (auto* prm = proc.apvts.getParameter (tabby::bandId (curBand, "bypass")))
            prm->setValueNotifyingHost (onButton.getToggleState() ? 0.0f : 1.0f);   // "On" ticked = enabled = not bypassed
    };
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

    // Route: Stereo / L / R / Mid / Side — a compact button opening a menu (stereo-only; honest param).
    routeButton.setColour (juce::TextButton::buttonColourId, tabby::palette::panel().brighter (0.18f));
    routeButton.onClick = [this] { showRouteMenu(); };
    addAndMakeVisible (routeButton);

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
    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &routeButton, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);
    typeButton.setButtonText (has ? kTypeNames[typeIndexOf (proc, curBand)] : juce::String ("—"));

    const int rIdx = has ? routeIndexOf (proc, curBand) : 0;
    routeButton.setButtonText (has ? kRouteShort[rIdx] : juce::String ("—"));
    routeButton.setColour (juce::TextButton::textColourOffId,
                           (has && rIdx != 0) ? tabby::palette::orange() : tabby::palette::text());

    const bool byp = has && proc.apvts.getRawParameterValue (tabby::bandId (curBand, "bypass"))->load() > 0.5f;
    onButton.setToggleState (has && ! byp, juce::dontSendNotification);   // "On" reflects enabled (= not bypassed)

    rebind();
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    repaint();
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset();
    if (curBand < 0) return;

    auto id = [this] (juce::StringRef s) { return tabby::bandId (curBand, s); };
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
        it.setImage (tabby::shapes::icon (tabby::filterTypeFromChoice (i), tabby::palette::violetLo()));
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

void BandEditStrip::showRouteMenu()
{
    if (curBand < 0) return;
    const int cur = routeIndexOf (proc, curBand);

    juce::PopupMenu m;
    for (int i = 0; i < 5; ++i) m.addItem (i + 1, kRouteNames[i], true, i == cur);

    juce::Component::SafePointer<BandEditStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (routeButton),
                     [safe] (int r)
                     {
                         if (safe == nullptr || r <= 0 || safe->curBand < 0) return;
                         if (auto* prm = safe->proc.apvts.getParameter (tabby::bandId (safe->curBand, "route")))
                             prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (r - 1)));
                         safe->routeButton.setButtonText (kRouteShort[r - 1]);
                         safe->routeButton.setColour (juce::TextButton::textColourOffId,
                                                      r - 1 != 0 ? tabby::palette::orange() : tabby::palette::text());
                         safe->routeButton.repaint();
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
    q.setEnabled (! isTilt && ! isCut);   // bells & resonant shelves have Q; tilt / HP / LP don't
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
    routeButton.setBounds (r.removeFromLeft (44).withSizeKeepingCentre (44, 24));
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
