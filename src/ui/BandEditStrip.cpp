// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"

namespace
{
    const char* kTypeNames[9] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
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

    modeButton.onClick = [this] { toggleMs(); };                // ST <-> M/S
    modeButton.setColour (juce::TextButton::buttonColourId, tabby::palette::panel().brighter (0.18f));
    addAndMakeVisible (modeButton);

    midTab.setClickingTogglesState (true);  sideTab.setClickingTogglesState (true);
    midTab.setColour  (juce::TextButton::buttonOnColourId, tabby::palette::violet());
    sideTab.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    midTab.onClick  = [this] { setLane (false); };
    sideTab.onClick = [this] { setLane (true); };
    addChildComponent (midTab);                                 // shown only in M/S
    addChildComponent (sideTab);

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

    setAlpha (0.55f);                 // translucent by default; opaque while the mouse is over / editing
    addMouseListener (this, true);    // also receive child mouse events for the hover-opacity logic

    setBand (-1);
}

void BandEditStrip::updateOpacity()
{
    setAlpha (isMouseOverOrDragging (true) ? 1.0f : 0.55f);
}

void BandEditStrip::mouseEnter (const juce::MouseEvent&) { updateOpacity(); }
void BandEditStrip::mouseExit  (const juce::MouseEvent&) { updateOpacity(); }
void BandEditStrip::mouseUp    (const juce::MouseEvent&) { updateOpacity(); }

BandEditStrip::~BandEditStrip() { proc.setSoloBand (-1); }   // never leave audio stuck in solo

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;
    curMs = has && proc.apvts.getRawParameterValue (tabby::bandId (curBand, "ms"))->load() > 0.5f;
    if (! curMs) editingSide = false;

    title.setText (has ? juce::String (curBand + 1) : juce::String ("—"), juce::dontSendNotification);
    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &prevButton, &nextButton,
                                    &modeButton, &midTab, &sideTab, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);

    modeButton.setButtonText (curMs ? "M/S" : "ST");
    modeButton.setColour (juce::TextButton::textColourOffId, curMs ? tabby::palette::orange() : tabby::palette::text());
    midTab.setVisible (curMs);   sideTab.setVisible (curMs);
    prevButton.setVisible (! curMs);  nextButton.setVisible (! curMs);  title.setVisible (! curMs);
    midTab.setToggleState (! editingSide, juce::dontSendNotification);
    sideTab.setToggleState (editingSide, juce::dontSendNotification);

    typeButton.setType (tabby::filterTypeFromChoice (laneTypeIndex()));

    rebind();   // (re)creates the lane attachments + bypass attachment (drives the power button)
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    resized();   // top-row layout depends on M/S
    repaint();
}

juce::String BandEditStrip::laneId (const juce::String& base) const
{
    if (curMs && editingSide)   // Side lane: "freq" -> "sFreq", "q" -> "sQ", "type" -> "sType", ...
        return tabby::bandId (curBand, "s" + base.substring (0, 1).toUpperCase() + base.substring (1));
    return tabby::bandId (curBand, base);
}

int BandEditStrip::laneTypeIndex() const
{
    if (curBand < 0) return 0;
    if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (laneId ("type"))))
        return juce::jlimit (0, 8, prm->getIndex());
    return 0;
}

void BandEditStrip::setActiveLane (bool side)
{
    editingSide = side;
    midTab.setToggleState (! side, juce::dontSendNotification);
    sideTab.setToggleState (side, juce::dontSendNotification);
    typeButton.setType (tabby::filterTypeFromChoice (laneTypeIndex()));
    rebind();
    updateForType();
    repaint();
}

void BandEditStrip::setLane (bool side)   // tab click: switch lane AND tell the canvas to follow
{
    setActiveLane (side);
    if (onLaneChanged) onLaneChanged (side);
}

void BandEditStrip::copyMidToSide()   // seed Side from Mid so enabling M/S "splits" the band into two equals
{
    if (curBand < 0) return;
    auto copy = [this] (const char* mid, const char* side)
    {
        auto* m = proc.apvts.getParameter (tabby::bandId (curBand, mid));
        auto* s = proc.apvts.getParameter (tabby::bandId (curBand, side));
        if (m != nullptr && s != nullptr) s->setValueNotifyingHost (m->getValue());   // normalised (same ranges)
    };
    copy ("type", "sType"); copy ("freq", "sFreq"); copy ("q", "sQ");
    copy ("gain", "sGain"); copy ("slope", "sSlope");
    if (auto* s = proc.apvts.getParameter (tabby::bandId (curBand, "sOn")))     s->setValueNotifyingHost (1.0f);
    if (auto* s = proc.apvts.getParameter (tabby::bandId (curBand, "sBypass"))) s->setValueNotifyingHost (0.0f);
}

void BandEditStrip::toggleMs()
{
    if (curBand < 0) return;
    auto* msPrm = proc.apvts.getParameter (tabby::bandId (curBand, "ms"));
    if (msPrm == nullptr) return;
    const bool newMs = ! (msPrm->getValue() > 0.5f);
    if (newMs && sideIsFresh()) copyMidToSide();           // seed Side from Mid only on the FIRST split;
    msPrm->setValueNotifyingHost (newMs ? 1.0f : 0.0f);    // afterwards Side persists across ST<->M/S
    if (! newMs) editingSide = false;
    setBand (curBand);                                     // refresh mode / tabs / lane / layout
}

bool BandEditStrip::sideIsFresh() const   // Side lane untouched (factory defaults) -> safe to seed from Mid
{
    if (curBand < 0) return false;
    auto raw = [this] (const char* id) { return proc.apvts.getRawParameterValue (tabby::bandId (curBand, id))->load(); };
    return std::abs (raw ("sFreq") - 1000.0f) < 0.5f && std::abs (raw ("sGain")) < 0.01f
        && std::abs (raw ("sQ") - 1.0f) < 0.001f && (int) raw ("sType") == 0 && (int) raw ("sSlope") == 1
        && raw ("sBypass") < 0.5f && raw ("sOn") > 0.5f;
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset(); bypassAtt.reset();
    if (curBand < 0) { onButton.setToggleState (false, juce::dontSendNotification); return; }

    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, laneId ("slope"), slopeBox);   // Mid or Side lane
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, laneId ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("gain"),  gain);

    // Power button mirrors the lane's bypass param — single source of truth (node double-click + button
    // both write it; this keeps the button's lit state in sync however it's toggled).
    if (auto* bp = proc.apvts.getParameter (laneId ("bypass")))
    {
        bypassAtt = std::make_unique<juce::ParameterAttachment> (*bp,
            [this] (float v) { onButton.setToggleState (v < 0.5f, juce::dontSendNotification); onButton.repaint(); });
        bypassAtt->sendInitialUpdate();
    }
}

void BandEditStrip::showTypeMenu()
{
    if (curBand < 0) return;
    const int cur = laneTypeIndex();

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
                         if (auto* prm = safe->proc.apvts.getParameter (safe->laneId ("type")))
                             prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (r - 1)));
                         safe->typeButton.setType (tabby::filterTypeFromChoice (r - 1));
                         safe->updateForType();
                     });
}

void BandEditStrip::updateForType()
{
    if (curBand < 0) { slopeBox.setVisible (false); return; }

    const auto t       = tabby::filterTypeFromChoice (laneTypeIndex());
    const bool isCut   = (t == teq::FilterType::HighPass || t == teq::FilterType::LowPass);
    const bool isShelf = (t == teq::FilterType::LowShelf || t == teq::FilterType::HighShelf);
    const bool isTilt  = (t == teq::FilterType::Tilt);
    const bool hasGain = (t == teq::FilterType::Bell || isShelf || isTilt);
    // The Notch rides a variable ORDER now (felitronics-core v0.1.5, slope->order like HP/LP), so it takes the
    // slope combo in octaves too — not a Q box — matching its slope-whisker on the curve.
    const bool usesSlope = isCut || (t == teq::FilterType::Notch);

    slopeBox.setVisible (usesSlope);                   // HP/LP + Notch -> the slope combo (octaves) replaces Q
    gain.setVisible (hasGain);   gain.setEnabled (hasGain);
    q.setVisible (! usesSlope && ! isTilt);   q.setEnabled (! usesSlope && ! isTilt);   // no Q for HP/LP/Notch or tilt
    resized();                                         // visibility changed -> re-lay the bottom row
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
    if (curMs)   // M/S: Mid | Side lane tabs take the nav slot
    {
        midTab.setBounds  (top.removeFromLeft (21).withSizeKeepingCentre (21, 20));
        top.removeFromLeft (2);
        sideTab.setBounds (top.removeFromLeft (21).withSizeKeepingCentre (21, 20));
    }
    else         // Stereo: < index > navigation
    {
        prevButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
        title.setBounds (top.removeFromLeft (18));
        nextButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
    }
    top.removeFromLeft (8);
    typeButton.setBounds (top.removeFromLeft (30).withSizeKeepingCentre (30, 22));    // icon only
    top.removeFromLeft (8);
    soloButton.setBounds (top.removeFromLeft (24).withSizeKeepingCentre (24, 22));
    top.removeFromLeft (6);
    modeButton.setBounds (top.removeFromLeft (34).withSizeKeepingCentre (34, 22));    // ST <-> M/S

    // bottom row (one line), adapts to the type:
    //   HP/LP -> FREQ + SLOPE combo (no Q) · bell/shelf -> FREQ + Q + GAIN
    //   band-pass/notch/all-pass -> FREQ + Q · tilt -> FREQ + GAIN
    auto row = r.withSizeKeepingCentre (r.getWidth(), 22);
    if (slopeBox.isVisible())
    {
        slopeBox.setBounds (row.removeFromRight (96).withSizeKeepingCentre (96, 22));
        row.removeFromRight (8);
        freq.setBounds (row);
    }
    else
    {
        if (gain.isVisible())
        {
            gain.setBounds (row.removeFromRight (60).withSizeKeepingCentre (60, 22));
            row.removeFromRight (8);
        }
        if (q.isVisible())
        {
            freq.setBounds (row.removeFromLeft (70).withSizeKeepingCentre (70, 22));
            row.removeFromLeft (8);
            q.setBounds (row);
        }
        else
            freq.setBounds (row);
    }
}
