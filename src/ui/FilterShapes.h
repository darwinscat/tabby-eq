// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <teq/EqTypes.h>

#include <memory>

//==============================================================================
// Filter-type shape icons — the little line-drawings of each response (à la Neutron). Shared by the
// edit-strip type menu and the curve's right-click menu so the iconography stays in one place.
namespace tabby::shapes
{
    inline juce::Path path (teq::FilterType t, juce::Rectangle<float> a)
    {
        using FT = teq::FilterType;
        const float x0 = a.getX(), x1 = a.getRight(), w = a.getWidth(), cx = a.getCentreX();
        const float top = a.getY() + 1.0f, bot = a.getBottom() - 1.0f, mid = a.getCentreY();
        juce::Path p;
        switch (t)
        {
            // Bell: a round DOME (cubic through a flattened apex) — a pointy peak reads as BandPass
            // (Oleh's icon review). BandPass keeps the tall narrow spike (the long nose is the tell);
            // Notch is exactly BandPass FLIPPED: baseline at the top, long nose down.
            case FT::Bell:      p.startNewSubPath (x0, mid);
                                p.cubicTo (cx - w * 0.28f, mid, cx - w * 0.17f, top, cx, top);
                                p.cubicTo (cx + w * 0.17f, top, cx + w * 0.28f, mid, x1, mid); break;
            case FT::BandPass:  p.startNewSubPath (x0, bot); p.quadraticTo (cx - w * 0.10f, bot, cx, top);
                                p.quadraticTo (cx + w * 0.10f, bot, x1, bot); break;
            case FT::Notch:     p.startNewSubPath (x0, top); p.quadraticTo (cx - w * 0.10f, top, cx, bot);
                                p.quadraticTo (cx + w * 0.10f, top, x1, top); break;
            case FT::LowShelf:  p.startNewSubPath (x0, top); p.lineTo (cx - w * 0.12f, top);
                                p.quadraticTo (cx, top, cx + w * 0.05f, mid); p.lineTo (x1, mid); break;
            case FT::HighShelf: p.startNewSubPath (x0, mid); p.lineTo (cx - w * 0.05f, mid);
                                p.quadraticTo (cx, top, cx + w * 0.12f, top); p.lineTo (x1, top); break;
            // HP/LP: Г-shaped — the transition squeezed into ~1/4 of the width (a steeper, more-dB/oct
            // knee) with a soft corner at the plateau, then a long flat top (Oleh's icon review).
            case FT::HighPass:  p.startNewSubPath (x0, bot);
                                p.cubicTo (x0 + w * 0.10f, bot, x0 + w * 0.12f, mid, x0 + w * 0.26f, mid);
                                p.lineTo (x1, mid); break;
            case FT::LowPass:   p.startNewSubPath (x0, mid); p.lineTo (x1 - w * 0.26f, mid);
                                p.cubicTo (x1 - w * 0.12f, mid, x1 - w * 0.10f, bot, x1, bot); break;
            case FT::Tilt:      p.startNewSubPath (x0, bot); p.lineTo (x1, top); break;
            case FT::AllPass:   p.startNewSubPath (x0, mid); p.lineTo (x1, mid); break;
        }
        return p;
    }

    // A Drawable of the shape — for PopupMenu::Item::setImage (which the default LookAndFeel renders).
    inline std::unique_ptr<juce::Drawable> icon (teq::FilterType t, juce::Colour col)
    {
        auto dp = std::make_unique<juce::DrawablePath>();
        dp->setPath (path (t, { 0.0f, 0.0f, 24.0f, 14.0f }));
        dp->setFill (juce::FillType (juce::Colours::transparentBlack));
        dp->setStrokeFill (juce::FillType (col));
        dp->setStrokeType (juce::PathStrokeType (1.5f));
        return dp;
    }
}
