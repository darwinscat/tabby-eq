// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Palette.h"

//==============================================================================
// The product's flat, self-painted top-right GLYPH buttons (gear / fullscreen). The flat TEXT item
// (FlatItem) and the undo/redo arrow moved to felitronics::appkit::chrome (FlatButtons.h); only the
// glyph buttons — which have no appkit equivalent — stay product-side here.
namespace tabby::ui
{

//==============================================================================
// A flat glyph button (gear / fullscreen corner-brackets): no frame, just the glyph, dim at rest
// and brightening on hover — the unframed top-right chrome.
class GlyphButton final : public juce::Button
{
public:
    enum class Glyph { Gear, Fullscreen, Save, Import, Export };
    explicit GlyphButton (Glyph g) : juce::Button ({}), glyph (g) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        g.setColour ((highlighted ? tabby::palette::text() : tabby::palette::textDim())
                         .withAlpha (isEnabled() ? 1.0f : 0.4f));
        const auto b = getLocalBounds().toFloat().reduced (5.0f);
        switch (glyph)
        {
            case Glyph::Gear:       drawGear (g, b);        break;
            case Glyph::Fullscreen: drawBrackets (g, b);    break;
            case Glyph::Save:       drawFloppy (g, b);      break;
            case Glyph::Import:     drawTrayArrow (g, b, true);  break;   // arrow INTO the tray
            case Glyph::Export:     drawTrayArrow (g, b, false); break;   // arrow OUT of the tray
        }
    }

private:
    // Save = a floppy disk (rounded body, clipped top-right corner, a top shutter + a bottom label).
    static void drawFloppy (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const float w = b.getWidth(), h = b.getHeight(), cut = w * 0.26f;
        juce::Path body;
        body.startNewSubPath (b.getX(), b.getY());
        body.lineTo (b.getRight() - cut, b.getY());
        body.lineTo (b.getRight(), b.getY() + cut);
        body.lineTo (b.getRight(), b.getBottom());
        body.lineTo (b.getX(), b.getBottom());
        body.closeSubPath();
        g.strokePath (body, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        g.fillRect (b.getX() + w * 0.30f, b.getY(), w * 0.30f, h * 0.30f);                          // top shutter
        g.drawRect (b.getX() + w * 0.24f, b.getBottom() - h * 0.42f, w * 0.52f, h * 0.42f, 1.0f);   // label
    }

    // Import/Export = an open tray with an arrow going IN (down) or OUT (up).
    static void drawTrayArrow (juce::Graphics& g, juce::Rectangle<float> b, bool into)
    {
        const float w = b.getWidth(), h = b.getHeight();
        const float ty = b.getBottom(), th = h * 0.34f;   // open tray at the bottom
        juce::Path tray;
        tray.startNewSubPath (b.getX(), ty - th);
        tray.lineTo (b.getX(), ty);
        tray.lineTo (b.getRight(), ty);
        tray.lineTo (b.getRight(), ty - th);
        g.strokePath (tray, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));

        const float ax = b.getCentreX();
        const float top = b.getY() + h * 0.04f;
        const float bot = ty - th - h * 0.14f;             // just above the tray
        const float hy  = into ? bot : top;                // arrowhead tip
        const float hd  = into ? -1.0f : 1.0f;             // head opens upward (into) / downward (out)
        g.drawLine (ax, top, ax, bot, 1.3f);               // shaft
        juce::Path head;
        head.startNewSubPath (ax - w * 0.20f, hy + hd * h * 0.20f);
        head.lineTo (ax, hy);
        head.lineTo (ax + w * 0.20f, hy + hd * h * 0.20f);
        g.strokePath (head, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    static void drawGear (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto  c = b.getCentre();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        g.drawEllipse (c.x - r * 0.58f, c.y - r * 0.58f, r * 1.16f, r * 1.16f, r * 0.34f);   // ring (hole free)
        juce::Path teeth;
        for (int i = 0; i < 8; ++i)   // 8 stub teeth around the ring
        {
            juce::Path t;
            t.addRoundedRectangle (-r * 0.13f, -r, r * 0.26f, r * 0.34f, r * 0.08f);
            teeth.addPath (t, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::twoPi / 8.0f)
                                  .translated (c.x, c.y));
        }
        g.fillPath (teeth);
    }

    static void drawBrackets (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const float L = juce::jmin (b.getWidth(), b.getHeight()) * 0.34f;   // corner arm length
        juce::Path p;
        p.startNewSubPath (b.getX(), b.getY() + L);                p.lineTo (b.getX(), b.getY());                p.lineTo (b.getX() + L, b.getY());
        p.startNewSubPath (b.getRight() - L, b.getY());            p.lineTo (b.getRight(), b.getY());            p.lineTo (b.getRight(), b.getY() + L);
        p.startNewSubPath (b.getRight(), b.getBottom() - L);       p.lineTo (b.getRight(), b.getBottom());       p.lineTo (b.getRight() - L, b.getBottom());
        p.startNewSubPath (b.getX() + L, b.getBottom());           p.lineTo (b.getX(), b.getBottom());           p.lineTo (b.getX(), b.getBottom() - L);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    Glyph glyph;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlyphButton)
};

} // namespace tabby::ui
