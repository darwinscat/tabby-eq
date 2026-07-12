// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"

#include <cmath>
#include "ui/Palette.h"
#include "ui/FilterShapes.h"
#include "ui/LaneMenu.h"
#include "eqview/HandleMath.h"   // shared filter-type classifiers (single home with the curve view)

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
            bypassAtt->setValueAsCompleteGesture (onButton.getToggleState() ? 1.0f : 0.0f);   // lit -> bypass the point
    };
    addAndMakeVisible (onButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    soloButton.onClick = [this] { proc.setSoloBand (soloButton.getToggleState() ? curBand : -1); };
    addAndMakeVisible (soloButton);

    // Type: a button (showing the current type) that opens a menu whose items carry shape icons. The type is
    // SHARED by all of the point's lanes (decision #2), so this edits band{b}_type regardless of the edit lane.
    typeButton.onClick = [this] { showTypeMenu(); };
    addAndMakeVisible (typeButton);

    // Placement-lane dropdown: the venn set-glyph + active dot; click opens the lane menu, wheel cycles lanes.
    laneButton.onClick = [this] { showLaneMenu(); };
    laneButton.onWheel = [this] (int dir) { cycleLane (dir); };
    addAndMakeVisible (laneButton);

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

    addMouseListener (this, true);    // also receive child mouse events for the hover-engagement logic

    setBand (-1);
}

// Idle translucency lives in the BACKGROUND paint only (see paint) — the component's own alpha stays 1.0
// so text / values / icons remain crisply readable over the curves at all times.
void BandEditStrip::updateEngaged()
{
    const bool over = isMouseOverOrDragging (true);
    if (over != engaged) { engaged = over; repaint(); }
}

void BandEditStrip::mouseEnter (const juce::MouseEvent&) { updateEngaged(); }
void BandEditStrip::mouseExit  (const juce::MouseEvent&) { updateEngaged(); }
void BandEditStrip::mouseUp    (const juce::MouseEvent&) { updateEngaged(); }

BandEditStrip::~BandEditStrip() { proc.setSoloBand (-1); }   // never leave audio stuck in solo

//==============================================================================
bool BandEditStrip::laneOn (int lane) const
{
    if (curBand < 0) return false;
    return tabby::lanemenu::laneOn (proc, curBand, lane);
}
unsigned BandEditStrip::enabledMask() const
{
    unsigned m = 0;
    for (int L = 0; L < tabby::kNumLanes; ++L) if (laneOn (L)) m |= (1u << L);
    return m;
}
bool BandEditStrip::stereoBus() const { return proc.getTotalNumOutputChannels() == 2; }

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;
    // The active edit lane is the persisted per-band property, clamped to an actually-enabled lane
    // (migration / a lane just un-checked can leave it pointing at a disabled lane).
    activeLane = has ? tabby::lanemenu::activeLaneOf (proc, curBand) : 0;

    // "3m"-style readout: band number + active lane letter. Same rule as the canvas badges — the letter
    // shows whenever the enabled set differs from the plain {ST} (so a single {l}-only point reads "3l").
    juce::String t ("—");
    if (has)
    {
        t = juce::String (curBand + 1);
        if (! tabby::lanemenu::plainSt (proc, curBand)) t << tabby::laneKey (activeLane);   // e.g. 3m / 3l / 3st
    }
    title.setText (t, juce::dontSendNotification);

    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &laneButton, &prevButton, &nextButton,
                                    &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);

    refreshLaneButton();
    typeButton.setType (tabby::filterTypeFromChoice (bandTypeIndex()));

    rebind();   // (re)creates the active-lane attachments + point-bypass attachment (drives the power button)
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    if (has) proc.setBandActiveLane (curBand, activeLane);
    resized();
    repaint();
}

juce::String BandEditStrip::laneId (const juce::String& base) const   // active-lane-scoped id
{
    const juce::String field = (base == "bypass") ? "byp" : base;
    return tabby::laneParamId (curBand, activeLane, field);
}

int BandEditStrip::bandTypeIndex() const   // the point's SHARED filter type
{
    if (curBand < 0) return 0;
    if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (tabby::bandId (curBand, "type"))))
        return juce::jlimit (0, 8, prm->getIndex());
    return 0;
}

void BandEditStrip::refreshLaneButton()
{
    laneButton.setState (curBand < 0 ? 1u : enabledMask(), activeLane);
}

void BandEditStrip::setActiveLane (int lane)   // external sync (canvas selected a node) — no onLaneChanged
{
    if (curBand < 0) return;
    lane = juce::jlimit (0, tabby::kNumLanes - 1, lane);
    if (! laneOn (lane)) lane = tabby::lanemenu::lowestEnabled (proc, curBand);   // never point at a disabled lane
    activeLane = lane;
    // keep the readout letter in step (same rule as the canvas badges: letter unless plain {ST})
    juce::String t = juce::String (curBand + 1);
    if (! tabby::lanemenu::plainSt (proc, curBand)) t << tabby::laneKey (activeLane);
    title.setText (t, juce::dontSendNotification);
    refreshLaneButton();
    typeButton.setType (tabby::filterTypeFromChoice (bandTypeIndex()));
    rebind();
    updateForType();
    proc.setBandActiveLane (curBand, activeLane);
    repaint();
}

void BandEditStrip::selectLane (int lane)   // user picked a lane (menu / wheel): switch + tell the canvas
{
    setActiveLane (lane);
    if (onLaneChanged) onLaneChanged (activeLane);
}

void BandEditStrip::cycleLane (int dir)   // wheel over the dropdown: next enabled lane in `dir`
{
    if (curBand < 0) return;
    const unsigned m = enabledMask();
    if (m == 0) return;
    int L = activeLane;
    for (int i = 0; i < tabby::kNumLanes; ++i)
    {
        L = (L + dir + tabby::kNumLanes) % tabby::kNumLanes;
        if (m & (1u << L)) break;
    }
    if (L != activeLane) selectLane (L);
}

void BandEditStrip::showLaneMenu()
{
    if (curBand < 0) return;
    juce::Component::SafePointer<BandEditStrip> safe (this);
    tabby::lanemenu::show (proc, curBand, stereoBus(), &laneButton,
        [safe] (int lane) { if (safe != nullptr) safe->selectLane (lane); },              // name-click -> active
        [safe] { if (safe != nullptr) { safe->setBand (safe->curBand);                    // toggles / links changed
                                        if (safe->onLanesEdited) safe->onLanesEdited(); } });
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset(); bypassAtt.reset();
    if (curBand < 0) { onButton.setToggleState (false, juce::dontSendNotification); return; }

    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, laneId ("slope"), slopeBox);   // active lane
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, laneId ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("gain"),  gain);

    // Power button mirrors the POINT bypass param — single source of truth (the whole point, not one lane).
    if (auto* bp = proc.apvts.getParameter (tabby::bandId (curBand, "bypass")))
    {
        bypassAtt = std::make_unique<juce::ParameterAttachment> (*bp,
            [this] (float v) { onButton.setToggleState (v < 0.5f, juce::dontSendNotification); onButton.repaint(); });
        bypassAtt->sendInitialUpdate();
    }
}

void BandEditStrip::showTypeMenu()
{
    if (curBand < 0) return;
    const int cur = bandTypeIndex();

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
                         const int oldChoice = safe->bandTypeIndex();
                         if (auto* prm = safe->proc.apvts.getParameter (tabby::bandId (safe->curBand, "type")))   // SHARED point type
                         {
                             prm->beginChangeGesture();   // gestured like every other strip edit — touch-automation hosts record it
                             prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (r - 1)));
                             prm->endChangeGesture();
                         }
                         tabby::snapQOnTypeSwitch (safe->proc.apvts, safe->curBand, oldChoice, r - 1);   // untouched Q follows the new type's default
                         safe->typeButton.setType (tabby::filterTypeFromChoice (r - 1));
                         safe->updateForType();
                     });
}

void BandEditStrip::updateForType()
{
    if (curBand < 0) { slopeBox.setVisible (false); return; }

    const auto t       = tabby::filterTypeFromChoice (bandTypeIndex());
    const bool isCut   = eqview::handles::isCut (t);        // shared classifiers — same home as the curve view's whiskers
    const bool isTilt  = (t == teq::FilterType::Tilt);
    const bool hasGain = eqview::handles::hasGain (t);
    // The Notch rides a variable ORDER (slope->order like HP/LP), so it shows the slope combo — but its
    // Q is an INDEPENDENT width (the −3 dB bandwidth, order-invariant), so unlike HP/LP the width bar
    // stays too (Oleh: «мы у Notch в окне ширину забыли»). Width displays in OCTAVES for the two
    // bandwidth-shaped types (Notch + BandPass): BW = (2/ln2)·asinh(1/(2Q)), exact and invertible.
    const bool isNotch   = (t == teq::FilterType::Notch);
    const bool isBw      = isNotch || (t == teq::FilterType::BandPass);
    const bool usesSlope = eqview::handles::slopeWhisker (t);   // == isCut || Notch || BandPass (single home)

    slopeBox.setVisible (usesSlope);
    gain.setVisible (hasGain);   gain.setEnabled (hasGain);
    const bool qOn = ! isCut && ! isTilt;              // only HP/LP (slope IS the width) and Tilt hide Q
    q.setVisible (qOn);   q.setEnabled (qOn);

    if (isBw)                                          // show/edit the width in octaves
    {
        q.textFromValueFunction = [] (double v)
        { return juce::String ((2.0 / std::log (2.0)) * std::asinh (1.0 / (2.0 * juce::jmax (0.05, v))), 2) + " oct"; };
        q.valueFromTextFunction = [] (const juce::String& txt)
        {
            const double bw = juce::jlimit (0.01, 12.0, txt.getDoubleValue());
            return 1.0 / (2.0 * std::sinh (std::log (2.0) * bw / 2.0));
        };
    }
    else
    {
        q.textFromValueFunction = [] (double v) { return juce::String (v, 1); };
        q.valueFromTextFunction = nullptr;
    }
    q.updateText();
    resized();                                         // visibility changed -> re-lay the bottom row
}

void BandEditStrip::paint (juce::Graphics& g)
{
    // Transparency split: only the BACKGROUND is translucent when idle (a faint wash — the curves stay
    // readable through the panel) while the controls keep full opacity; on hover / edit it goes fully
    // opaque. (The old whole-component setAlpha dimmed the value digits too.)
    auto rr = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (tabby::palette::panel().withAlpha (engaged ? 1.0f : 0.35f));
    g.fillRoundedRectangle (rr, kCorner);
    g.setColour (tabby::palette::violetLo().withAlpha (engaged ? 0.35f : 0.20f));   // floating-panel rim
    g.drawRoundedRectangle (rr, kCorner, 1.0f);
}

void BandEditStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 6);

    // WIDTH DISCIPLINE: the strip window's width (EqCurveDisplay::kToolbarW = 218) is exactly this top
    // row's buttons + margins — 22+6+12+30+12+8+30+8+24+6+44 = 202 inner + 2×8. The value bars below
    // divide the same width evenly. The strip never resizes between selections.
    auto top = r.removeFromTop (22);
    r.removeFromTop (8);
    onButton.setBounds (top.removeFromLeft (22).withSizeKeepingCentre (22, 22));      // power (point enable/bypass)
    top.removeFromLeft (6);
    prevButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));    // < index > nav — always
    title.setBounds (top.removeFromLeft (30));                                        // "24st" fits
    nextButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
    top.removeFromLeft (8);
    typeButton.setBounds (top.removeFromLeft (30).withSizeKeepingCentre (30, 22));    // icon only (shared type)
    top.removeFromLeft (8);
    soloButton.setBounds (top.removeFromLeft (24).withSizeKeepingCentre (24, 22));
    top.removeFromLeft (6);
    laneButton.setBounds (top.removeFromLeft (44).withSizeKeepingCentre (44, 22));    // placement-lane dropdown

    // bottom row (one line), adapts to the type:
    //   HP/LP/Notch -> FREQ + SLOPE combo (as before — the combo replaces Q/gain) · otherwise the visible
    //   value bars split the row EVENLY: bell/shelf freq|Q|gain at exactly 1/3 each (bp/ap + tilt: halves).
    auto row = r.withSizeKeepingCentre (r.getWidth(), 22);
    if (slopeBox.isVisible())
    {
        slopeBox.setBounds (row.removeFromRight (104).withSizeKeepingCentre (104, 22));
        row.removeFromRight (8);
        if (q.isVisible())                                             // Notch: FREQ | WIDTH(oct) share the rest
        {
            auto half = row.removeFromLeft (row.getWidth() / 2);
            half.removeFromRight (2); row.removeFromLeft (2);
            freq.setBounds (half);
            q.setBounds (row);
        }
        else
            freq.setBounds (row);                                      // HP/LP: FREQ fills the rest
    }
    else
    {
        juce::Slider* bars[3] = { &freq, nullptr, nullptr }; int nb = 1;
        if (q.isVisible())    bars[nb++] = &q;
        if (gain.isVisible()) bars[nb++] = &gain;
        for (int i = 0; i < nb; ++i)
        {
            auto cell = row.removeFromLeft (row.getWidth() / (nb - i));   // equal shares of the full width
            if (i > 0)      cell.removeFromLeft (2);                      // 4 px gutter between neighbours
            if (i < nb - 1) cell.removeFromRight (2);
            bars[i]->setBounds (cell);
        }
    }
}
