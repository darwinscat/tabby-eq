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
    title.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (title);

    onButton.onClick = [this]
    {
        if (bypassAtt != nullptr)
            bypassAtt->setValueAsCompleteGesture (onButton.getToggleState() ? 1.0f : 0.0f);   // lit -> bypass it
    };
    addAndMakeVisible (onButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    soloButton.onClick = [this] { proc.setSoloBand (soloButton.getToggleState() ? curBand : -1); };
    addAndMakeVisible (soloButton);

    // Type: a button (showing the current type) that opens a menu whose items carry shape icons —
    // a plain ComboBox can't render per-item images, so we drive the menu ourselves.
    typeButton.onClick = [this] { showTypeMenu(); };
    addAndMakeVisible (typeButton);

    // Route: Stereo / L / R / Mid / Side — a compact button opening a menu (stereo-only; honest param).
    routeButton.setColour (juce::TextButton::buttonColourId, tabby::palette::panel().brighter (0.18f));
    routeButton.onClick = [this] { showRouteMenu(); };
    addAndMakeVisible (routeButton);

    prevButton.onClick = [this] { if (onStep) onStep (-1); };   // editor maps these to display.stepSelection
    nextButton.onClick = [this] { if (onStep) onStep (+1); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    const char* slopes[] = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
    for (int i = 0; i < 7; ++i) slopeBox.addItem (slopes[i], i + 1);
    addAndMakeVisible (slopeBox);

    auto setupField = [this] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearBar);          // value + unit live inside the bar; editing types the number
        s.setColour (juce::Slider::trackColourId,       tabby::palette::violet().withAlpha (0.45f));
        s.setColour (juce::Slider::textBoxTextColourId, tabby::palette::text());
        addAndMakeVisible (s);
    };
    setupField (freq);
    setupField (q);
    setupField (gain);

    freq.onDragEnd = [this] { if (onEdited) onEdited(); };   // re-place the toolbar AFTER the bar drag ends
    gain.onDragEnd = [this] { if (onEdited) onEdited(); };

    setBand (-1);
}

BandEditStrip::~BandEditStrip() { proc.setSoloBand (-1); }   // never leave audio stuck in solo

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;

    title.setText (has ? juce::String (curBand + 1) : juce::String ("—"), juce::dontSendNotification);
    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &routeButton, &prevButton, &nextButton, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);
    typeButton.setType (tabby::filterTypeFromChoice (has ? typeIndexOf (proc, curBand) : 0));

    const int rIdx = has ? routeIndexOf (proc, curBand) : 0;
    routeButton.setButtonText (has ? kRouteShort[rIdx] : juce::String ("—"));
    routeButton.setColour (juce::TextButton::textColourOffId,
                           (has && rIdx != 0) ? tabby::palette::orange() : tabby::palette::text());

    rebind();   // (re)creates the bypass attachment, which drives the power button's lit state
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    repaint();
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset(); bypassAtt.reset();
    if (curBand < 0) { onButton.setToggleState (false, juce::dontSendNotification); return; }

    auto id = [this] (juce::StringRef s) { return tabby::bandId (curBand, s); };
    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, id ("slope"), slopeBox);
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, id ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, id ("gain"),  gain);

    // Power button mirrors the bypass param — single source of truth (node double-click + button both
    // write it; this keeps the button's lit state in sync however it's toggled).
    if (auto* bp = proc.apvts.getParameter (id ("bypass")))
    {
        bypassAtt = std::make_unique<juce::ParameterAttachment> (*bp,
            [this] (float v) { onButton.setToggleState (v < 0.5f, juce::dontSendNotification); onButton.repaint(); });
        bypassAtt->sendInitialUpdate();
    }
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
                         safe->typeButton.setType (tabby::filterTypeFromChoice (r - 1));
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
    auto rr = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (tabby::palette::panel());
    g.fillRoundedRectangle (rr, 8.0f);
    g.setColour (tabby::palette::violetLo().withAlpha (0.35f));        // floating-panel rim
    g.drawRoundedRectangle (rr, 8.0f, 1.0f);
}

void BandEditStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 6);

    // top row: power · < index > · type-icon · solo · route
    auto top = r.removeFromTop (22);
    r.removeFromTop (8);
    onButton.setBounds (top.removeFromLeft (22).withSizeKeepingCentre (22, 22));      // power (enable)
    top.removeFromLeft (6);
    prevButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
    title.setBounds (top.removeFromLeft (18));
    nextButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
    top.removeFromLeft (8);
    typeButton.setBounds (top.removeFromLeft (30).withSizeKeepingCentre (30, 22));    // icon only
    top.removeFromLeft (8);
    soloButton.setBounds (top.removeFromLeft (24).withSizeKeepingCentre (24, 22));
    top.removeFromLeft (6);
    routeButton.setBounds (top.removeFromLeft (36).withSizeKeepingCentre (36, 22));

    // bottom row (one line): freq · q · (gain OR slope for HP/LP) — value + unit live inside each bar
    auto row = r.withSizeKeepingCentre (r.getWidth(), 22);
    if (slopeBox.isVisible())
        slopeBox.setBounds (row.removeFromRight (78).withSizeKeepingCentre (78, 22));
    else
        gain.setBounds (row.removeFromRight (60).withSizeKeepingCentre (60, 22));
    row.removeFromRight (8);
    freq.setBounds (row.removeFromLeft (70).withSizeKeepingCentre (70, 22));
    row.removeFromLeft (8);
    q.setBounds (row.withSizeKeepingCentre (row.getWidth(), 22));   // middle fills the rest
}
