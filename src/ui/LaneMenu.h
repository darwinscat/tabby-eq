// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Parameters.h"
#include "ui/Palette.h"
#include "ui/LaneGlyphs.h"

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// The placement-lane dropdown (LANES.md UI, decision #6). A PopupMenu of custom rows that STAYS OPEN on
// checkbox clicks (CustomComponent with isTriggeredAutomatically=false — the menu never auto-dismisses;
// a row calls triggerMenuItem() itself only on a name-click). Five lane rows (ST/L/R/M/S) + a separator
// + Link FQ / Link Q. Shared by the strip's dropdown button and the node right-click menu.
//
//   checkbox click  -> toggle the lane (menu stays open; last enabled refuses with a shake/flash)
//   name click      -> enable-if-needed + set ACTIVE lane + close
//   Alt-click       -> make-only-lane (enable this, disable the rest)  [an edit; stays open]
//   newly enabled lanes SEED freq/Q/gain/slope from the active lane; a new split seeds Link FQ/Q from
//   the View defaults. Non-stereo bus -> the four domain rows are dimmed ("stereo only").
namespace tabby::lanemenu
{
    //== shared context for one open menu (kept alive by the rows via shared_ptr) ==================
    struct Ctx
    {
        Ctx (TabbyEqAudioProcessor& p, int b, bool s) : proc (p), band (b), stereoBus (s) {}
        TabbyEqAudioProcessor& proc;
        int  band = 0;
        bool stereoBus = true;
        std::function<void(int)> onActivate;   // name-click chose lane -> owner selects it (menu closes)
        std::function<void()>    onChanged;    // any toggle / link / active edit -> owner re-syncs
        std::vector<juce::Component*> rows;     // every custom row (owned by the menu) — for repaint-all
    };

    //== small param helpers =======================================================================
    inline bool laneOn (TabbyEqAudioProcessor& p, int b, int L)
    {
        auto* a = p.apvts.getRawParameterValue (tabby::laneParamId (b, L, "on"));
        return a != nullptr && a->load() > 0.5f;
    }
    inline int enabledCount (TabbyEqAudioProcessor& p, int b)
    {
        int c = 0; for (int L = 0; L < tabby::kNumLanes; ++L) if (laneOn (p, b, L)) ++c; return c;
    }
    inline int lowestEnabled (TabbyEqAudioProcessor& p, int b)
    {
        for (int L = 0; L < tabby::kNumLanes; ++L) if (laneOn (p, b, L)) return L; return 0;
    }
    inline int activeLaneOf (TabbyEqAudioProcessor& p, int b)
    {
        const int a = (int) p.apvts.state.getProperty (tabby::bandId (b, "activeLane"), 0);
        if (a >= 0 && a < tabby::kNumLanes && laneOn (p, b, a)) return a;
        return lowestEnabled (p, b);
    }
    // One-shot gestured write (begin+set+end) — lane toggles are host-visible operator steps; bare
    // setValueNotifyingHost outside a gesture drops/misgroups DAW automation + undo.
    inline void setParamGestured (juce::RangedAudioParameter* prm, float v01)
    {
        if (prm == nullptr) return;
        prm->beginChangeGesture(); prm->setValueNotifyingHost (v01); prm->endChangeGesture();
    }
    inline void setLaneOnParam (TabbyEqAudioProcessor& p, int b, int L, bool on)
    {
        setParamGestured (p.apvts.getParameter (tabby::laneParamId (b, L, "on")), on ? 1.0f : 0.0f);
    }
    // Seed a newly enabled lane's edit fields from the active lane (freq/Q/gain/slope — type is shared).
    inline void seedLane (TabbyEqAudioProcessor& p, int b, int dst, int src)
    {
        if (dst == src) return;
        for (const char* f : { "freq", "q", "gain", "slope" })
        {
            auto* s = p.apvts.getParameter (tabby::laneParamId (b, src, f));
            auto* d = p.apvts.getParameter (tabby::laneParamId (b, dst, f));
            if (s != nullptr && d != nullptr) setParamGestured (d, s->getValue());        // same range -> exact
        }
    }
    // Seed the point's Link FQ / Link Q from the View-menu defaults (a point that just became a split).
    // Defaults only ever turn a flag ON — they never reset a flag the user explicitly enabled (a
    // re-split must not silently discard that choice).
    inline void seedLinkDefaults (TabbyEqAudioProcessor& p, int b)
    {
        auto& st = p.apvts.state;
        if ((bool) st.getProperty ("defaultLinkFq", false)) st.setProperty (tabby::bandId (b, "linkFq"), true, nullptr);
        if ((bool) st.getProperty ("defaultLinkQ",  false)) st.setProperty (tabby::bandId (b, "linkQ"),  true, nullptr);
    }
    // Turning a link ON must snap the existing divergence NOW (the processor mirrors only future edits):
    // copy the active lane's field(s) onto every other enabled lane.
    inline void snapLink (TabbyEqAudioProcessor& p, int b, bool freq)
    {
        const int src = activeLaneOf (p, b);
        for (int L = 0; L < tabby::kNumLanes; ++L)
        {
            if (L == src || ! laneOn (p, b, L)) continue;
            for (const char* f : (freq ? std::initializer_list<const char*> { "freq" }
                                       : std::initializer_list<const char*> { "q", "slope" }))
            {
                auto* sP = p.apvts.getParameter (tabby::laneParamId (b, src, f));
                auto* dP = p.apvts.getParameter (tabby::laneParamId (b, L, f));
                if (sP != nullptr && dP != nullptr) setParamGestured (dP, sP->getValue());
            }
        }
    }

    inline void repaintAll (const std::shared_ptr<Ctx>& c)
    {
        for (auto* r : c->rows) if (r != nullptr) r->repaint();
    }

    //== one lane row ==============================================================================
    class LaneRow : public juce::PopupMenu::CustomComponent, private juce::Timer
    {
    public:
        LaneRow (std::shared_ptr<Ctx> c, int lane)
            : juce::PopupMenu::CustomComponent (false), ctx (std::move (c)), L (lane)
        {
            ctx->rows.push_back (this);
        }
        ~LaneRow() override { for (auto& r : ctx->rows) if (r == this) r = nullptr; }

        void getIdealSize (int& w, int& h) override { w = 208; h = 26; }

        bool dimmed() const noexcept { return ! ctx->stereoBus && L != 0; }   // domain lanes need a stereo bus

        void paint (juce::Graphics& g) override
        {
            auto rf = getLocalBounds().toFloat();
            if (flash > 0) rf.translate ((flash & 1) ? 3.0f : -3.0f, 0.0f);   // shake on a refused uncheck

            const bool on     = laneOn (ctx->proc, ctx->band, L);
            const bool active = (activeLaneOf (ctx->proc, ctx->band) == L);
            const float a     = dimmed() ? 0.4f : 1.0f;

            if (active && ! dimmed())                                          // active-lane row highlight
            {
                g.setColour (tabby::palette::violet().withAlpha (0.20f));
                g.fillRoundedRectangle (rf.reduced (2.0f, 1.0f), 4.0f);
            }
            else if (isItemHighlighted() && ! dimmed())
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (rf.reduced (2.0f, 1.0f), 4.0f);
            }

            auto box = juce::Rectangle<float> (rf.getX() + 7.0f, rf.getCentreY() - 6.0f, 12.0f, 12.0f);
            const auto lc = tabby::palette::lane (L);
            if (flash > 0)   { g.setColour (juce::Colours::red.withAlpha (0.9f)); g.drawRoundedRectangle (box, 3.0f, 1.6f); }
            else if (on)     { g.setColour (lc.withAlpha (a)); g.fillRoundedRectangle (box, 3.0f);
                               g.setColour (tabby::palette::bg().withAlpha (0.9f));
                               juce::Path tick; tick.startNewSubPath (box.getX() + 2.5f, box.getCentreY());
                               tick.lineTo (box.getCentreX() - 0.5f, box.getBottom() - 3.0f);
                               tick.lineTo (box.getRight() - 2.0f, box.getY() + 3.0f);
                               g.strokePath (tick, juce::PathStrokeType (1.6f)); }
            else             { g.setColour (tabby::palette::textDim().withAlpha (a)); g.drawRoundedRectangle (box, 3.0f, 1.2f); }

            tabby::venn::drawOne (g, { rf.getX() + 26.0f, rf.getY() + 3.0f, 24.0f, rf.getHeight() - 6.0f },
                                  L, lc.withAlpha (a));

            g.setColour (lc.withAlpha (a));                                    // colour dot
            g.fillEllipse (rf.getX() + 54.0f, rf.getCentreY() - 3.0f, 6.0f, 6.0f);

            g.setColour ((active ? tabby::palette::text() : tabby::palette::textDim()).withAlpha (a));
            g.setFont (juce::Font (juce::FontOptions (13.0f).withStyle (active ? "Bold" : "Regular")));
            g.drawText (tabby::laneWord (L), juce::Rectangle<float> (rf.getX() + 66.0f, rf.getY(), 80.0f, rf.getHeight()),
                        juce::Justification::centredLeft);

            if (dimmed())
            {
                g.setColour (tabby::palette::textDim().withAlpha (0.6f));
                g.setFont (juce::Font (juce::FontOptions (9.5f).withStyle ("Italic")));
                g.drawText ("stereo only", rf.removeFromRight (66.0f), juce::Justification::centredRight);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (dimmed()) { doFlash(); return; }                              // domain lane on a non-stereo bus

            auto& p = ctx->proc; const int b = ctx->band;

            if (e.mods.isAltDown())                                            // make-only-lane (stays single)
            {
                if (! laneOn (p, b, L))
                {
                    const int src = activeLaneOf (p, b);                       // source BEFORE enabling the target
                    seedLane (p, b, L, src);                                   // seed while disabled, THEN enable —
                    setLaneOnParam (p, b, L, true);                            // the set is never transiently empty
                }
                for (int k = 0; k < tabby::kNumLanes; ++k) if (k != L) setLaneOnParam (p, b, k, false);
                p.setBandActiveLane (b, L);
                if (ctx->onChanged) ctx->onChanged();
                repaintAll (ctx);
                return;
            }

            const bool nameZone = e.position.x > 24.0f;
            if (! nameZone)                                                    // checkbox: toggle, stay open
            {
                const bool on = laneOn (p, b, L);
                if (on)
                {
                    if (enabledCount (p, b) <= 1) { doFlash(); return; }       // last enabled refuses
                    setLaneOnParam (p, b, L, false);
                    p.setBandActiveLane (b, activeLaneOf (p, b));              // clamp active to a live lane
                }
                else
                {
                    const int wasCount = enabledCount (p, b);
                    const int src = activeLaneOf (p, b);                       // source BEFORE the new lane exists
                    seedLane (p, b, L, src);                                   // seed while still disabled — the
                    setLaneOnParam (p, b, L, true);                            // engine never sees a half-seeded lane
                    if (wasCount == 1) seedLinkDefaults (p, b);               // first split -> seed link defaults
                }
                if (ctx->onChanged) ctx->onChanged();
                repaintAll (ctx);
                return;
            }

            // name click: enable-if-needed + set active + close
            if (! laneOn (p, b, L))
            {
                const int wasCount = enabledCount (p, b);
                const int src = enabledCount (p, b) > 0 ? activeLaneOf (p, b) : 0;
                seedLane (p, b, L, src);                                       // seed while still disabled
                setLaneOnParam (p, b, L, true);
                if (wasCount == 1) seedLinkDefaults (p, b);
            }
            p.setBandActiveLane (b, L);
            if (ctx->onActivate) ctx->onActivate (L);
            triggerMenuItem();
        }

    private:
        void doFlash() { flash = 8; startTimerHz (60); repaint(); }
        void timerCallback() override { if (--flash <= 0) { flash = 0; stopTimer(); } repaint(); }

        std::shared_ptr<Ctx> ctx;
        int L = 0;
        int flash = 0;
    };

    //== one link row (Link FQ / Link Q) ===========================================================
    class LinkRow : public juce::PopupMenu::CustomComponent
    {
    public:
        LinkRow (std::shared_ptr<Ctx> c, bool freq)
            : juce::PopupMenu::CustomComponent (false), ctx (std::move (c)), isFreq (freq)
        {
            ctx->rows.push_back (this);
        }
        ~LinkRow() override { for (auto& r : ctx->rows) if (r == this) r = nullptr; }

        void getIdealSize (int& w, int& h) override { w = 208; h = 24; }

        const char* prop() const noexcept { return isFreq ? "linkFq" : "linkQ"; }

        void paint (juce::Graphics& g) override
        {
            auto rf = getLocalBounds().toFloat();
            const bool on = (bool) ctx->proc.apvts.state.getProperty (tabby::bandId (ctx->band, prop()), false);
            if (isItemHighlighted()) { g.setColour (juce::Colours::white.withAlpha (0.06f)); g.fillRoundedRectangle (rf.reduced (2.0f, 1.0f), 4.0f); }

            auto box = juce::Rectangle<float> (rf.getX() + 7.0f, rf.getCentreY() - 6.0f, 12.0f, 12.0f);
            if (on) { g.setColour (tabby::palette::violetLo()); g.fillRoundedRectangle (box, 3.0f);
                      g.setColour (tabby::palette::bg().withAlpha (0.9f));
                      juce::Path tick; tick.startNewSubPath (box.getX() + 2.5f, box.getCentreY());
                      tick.lineTo (box.getCentreX() - 0.5f, box.getBottom() - 3.0f);
                      tick.lineTo (box.getRight() - 2.0f, box.getY() + 3.0f);
                      g.strokePath (tick, juce::PathStrokeType (1.6f)); }
            else    { g.setColour (tabby::palette::textDim()); g.drawRoundedRectangle (box, 3.0f, 1.2f); }

            g.setColour (tabby::palette::text());
            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText (isFreq ? "Link FQ" : "Link Q", juce::Rectangle<float> (rf.getX() + 26.0f, rf.getY(), 140.0f, rf.getHeight()),
                        juce::Justification::centredLeft);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            const bool v = ! (bool) ctx->proc.apvts.state.getProperty (tabby::bandId (ctx->band, prop()), false);
            ctx->proc.apvts.state.setProperty (tabby::bandId (ctx->band, prop()), v, nullptr);
            if (v) snapLink (ctx->proc, ctx->band, isFreq);   // enabling a link snaps the existing divergence NOW
            if (ctx->onChanged) ctx->onChanged();
            repaint();
        }

    private:
        std::shared_ptr<Ctx> ctx;
        bool isFreq = true;
    };

    //== build + show ==============================================================================
    // Append the lane rows (5 lanes + separator + Link FQ / Link Q) to `menu`. The rows hold a shared_ptr
    // to the Ctx, so it lives exactly as long as the menu (no external ownership needed).
    inline void build (juce::PopupMenu& menu, TabbyEqAudioProcessor& proc, int band, bool stereoBus,
                       std::function<void(int)> onActivate, std::function<void()> onChanged)
    {
        auto ctx = std::make_shared<Ctx> (proc, band, stereoBus);
        ctx->onActivate = std::move (onActivate);
        ctx->onChanged  = std::move (onChanged);
        for (int L = 0; L < tabby::kNumLanes; ++L)
            menu.addCustomItem (1000 + L, std::make_unique<LaneRow> (ctx, L));
        menu.addSeparator();
        menu.addCustomItem (2000, std::make_unique<LinkRow> (ctx, true));
        menu.addCustomItem (2001, std::make_unique<LinkRow> (ctx, false));
    }

    // Build + show the lane menu anchored to `target` (the strip button or a node's screen area).
    inline void show (TabbyEqAudioProcessor& proc, int band, bool stereoBus, juce::Component* target,
                      std::function<void(int)> onActivate, std::function<void()> onChanged)
    {
        juce::PopupMenu m;
        build (m, proc, band, stereoBus, std::move (onActivate), std::move (onChanged));
        juce::PopupMenu::Options o;
        if (target != nullptr) o = o.withTargetComponent (target);
        m.showMenuAsync (o, [] (int) {});
    }
}
