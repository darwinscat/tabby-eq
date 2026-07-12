// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Palette.h"

namespace tabby::ui
{

//==============================================================================
// SnapshotButton — one A/B/C/D compare-register button (ported from orbitcab's, same UX
// language). On top of a stock TextButton (left-click = recall, wired by the editor via
// onClick) it owns the register-COPY gestures:
//
//   • right-click  → onPopup (the editor's copy menu). The gate must live HERE, not in a mouse
//     listener: juce::Button fires its click for ANY mouse button, so an unfiltered right-click
//     would recall the register underneath the menu.
//   • drag onto a sibling → that sibling's onCopyDrop (copy this register there, one undoable
//     edit in the target). A finished drag swallows the release — it must not double as a click.
//   • drop target for a sibling's drag, painted as a warm ring while a compatible drag hovers.
//
// TabbyEQ has no custom LookAndFeel, so the "modified since you dialed it in" dot and the
// drop-hover ring are painted here, over the stock button (family orange, like orbitcab's LnF).
// The editor is the juce::DragAndDropContainer; the drag payload is dragTag(index).
class SnapshotButton final : public juce::TextButton,
                             public juce::DragAndDropTarget
{
public:
    SnapshotButton() = default;

    void setRegisterIndex (int i)                        { index = i; }
    static juce::String dragTag (int i)                  { return "tabbyeq-snapshot:" + juce::String (i); }

    // The edited marker (driven by the editor from snapshotEdited() on every history revision).
    void setEdited (bool e)                              { if (edited != e) { edited = e; repaint(); } }

    std::function<void()>                  onPopup;      // right-click → the editor's copy menu
    std::function<void (int from, int to)> onCopyDrop;   // a sibling was dropped here → copy from → to

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onPopup) onPopup(); return; }   // no press, no recall
        TextButton::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() > 6)
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                dragging = true;
                setState (buttonNormal);    // release the pressed look — the drop is the action now
                dnd->startDragging (dragTag (index), this);
            }
        if (! dragging)
            TextButton::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A popup release or a finished drag must not fall through to Button::mouseUp — it would
        // fire the click and recall this register on top of the menu / the copy.
        if (dragging)              { dragging = false; return; }
        if (e.mods.isPopupMenu())  return;
        TextButton::mouseUp (e);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        TextButton::paintButton (g, highlighted, down);
        const auto r = getLocalBounds().toFloat();
        if (edited)
        {
            const float d = 4.0f;   // "modified since you dialed it in" — small warm dot, top-right
            g.setColour (tabby::palette::orange().withAlpha (isEnabled() ? 0.95f : 0.4f));
            g.fillEllipse (r.getRight() - d - 3.0f, r.getY() + 3.0f, d, d);
        }
        if (dropHover)              // "release to copy here" ring while a sibling's drag hovers
        {
            g.setColour (tabby::palette::orange().withAlpha (0.9f));
            g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);
        }
    }

    // ---- DragAndDropTarget (a sibling A/B/C/D drag) ----
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        const auto s = d.description.toString();
        return s.startsWith ("tabbyeq-snapshot:") && s.getTrailingIntValue() != index;
    }
    void itemDragEnter (const SourceDetails&)   override { setDropHover (true); }
    void itemDragExit  (const SourceDetails&)   override { setDropHover (false); }
    void itemDropped   (const SourceDetails& d) override
    {
        setDropHover (false);
        if (onCopyDrop)
            onCopyDrop (d.description.toString().getTrailingIntValue(), index);
    }

private:
    void setDropHover (bool h) { if (dropHover != h) { dropHover = h; repaint(); } }

    int  index     = 0;
    bool dragging  = false;
    bool edited    = false;
    bool dropHover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapshotButton)
};

} // namespace tabby::ui
