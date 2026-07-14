// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ui/ChromeButtons.h"   // HistoryArrowButton (undo/redo arrows)
#include "../ui/Palette.h"         // Ф1: register buttons stay palette-styled (→ ChromeTheme at the lib move)

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace tabby::chrome
{

//==============================================================================
// RegisterButton — one A/B/C/D compare-register button (ported from SnapshotButton). On top of a
// stock TextButton (left-click = recall, wired by the cell) it owns the register-COPY gestures:
//
//   • right-click  → onPopup (the product's copy menu). The gate must live HERE, not in a listener:
//     juce::Button fires its click for ANY mouse button, so an unfiltered right-click would recall
//     the register underneath the menu.
//   • drag onto a sibling → that sibling's onCopyDrop (copy this register there). A finished drag
//     swallows the release — it must not double as a click.
//   • drop target for a sibling's drag, painted as a warm ring while a compatible drag hovers.
//
// The drag payload is GENERIC (`compare-register:<i>`, not a product string): the CompareCell owns
// its own DragAndDropContainer, so the drag is isolated to this cell and needs no product tag.
class RegisterButton final : public juce::TextButton,
                             public juce::DragAndDropTarget
{
public:
    RegisterButton() = default;

    void setRegisterIndex (int i)                        { index = i; }
    static juce::String dragTag (int i)                  { return "compare-register:" + juce::String (i); }

    void setEdited (bool e)                              { if (edited != e) { edited = e; repaint(); } }

    std::function<void()>                  onPopup;      // right-click → the product's copy menu
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
        // FLAT (no stock frame): the ACTIVE register is a violet rounded FRAME (outline, not a fill),
        // siblings are bare letters that lift on hover — one visual block with the flat undo/redo arrows.
        const auto r = getLocalBounds().toFloat();
        if (getToggleState())
        {
            g.setColour (tabby::palette::violet().withAlpha (down ? 0.7f : 1.0f));
            g.drawRoundedRectangle (r.reduced (1.5f), 4.0f, 1.5f);   // active = frame, not fill
        }
        else if (highlighted || down)
        {
            g.setColour (tabby::palette::text().withAlpha (down ? 0.16f : 0.08f));
            g.fillRoundedRectangle (r.reduced (1.0f), 4.0f);
        }
        g.setColour (getToggleState() ? juce::Colours::white
                                      : highlighted ? tabby::palette::text() : tabby::palette::textDim());
        g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);

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
        return s.startsWith ("compare-register:") && s.getTrailingIntValue() != index;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RegisterButton)
};

//==============================================================================
// CompareModel — the state the PRODUCT pushes into the cell (setModel) whenever its history/register
// state changes. The cell NEVER reaches back into the product's history: CompareHistory's
// onHistoryChanged / onAfterApply are single callback slots already owned by the processor, so the
// product owns them and PUSHES the resulting state here. Contract SHAPE is complete; the richness is
// LEAN (only what the EQ does today — `hasContent` / `pasteAvailable` complete the contract and are
// consumed by the product's copy menu for now).
struct CompareModel
{
    static constexpr int kMaxRegisters = 8;

    int  registerCount = 4;
    int  active        = -1;                         // the recalled register (violet)
    std::array<bool, kMaxRegisters> edited     {};   // per-register "modified since dialed in" dot
    std::array<bool, kMaxRegisters> hasContent {};   // per-register content-bearing (menu source gate)
    bool canUndo = false, canRedo = false;           // undo/redo enablement
    juce::String undoLabel, redoLabel;               // raw peek labels (empty → "Parameter Change")
    bool pasteAvailable   = false;                   // the clipboard holds a valid state (menu gate)
    bool navigationBlocked = false;                  // any gesture open → nav paths inert
};

// CompareActions — the cell calls these; the product performs the engine work and re-pushes a model.
struct CompareActions
{
    std::function<void (int)>       recall;              // switch to register i (the product gates + re-syncs)
    std::function<void (int, int)>  copy;                // copy from → to (drag / menu)
    std::function<void ()>          undo, redo;          // undo / redo arrows
    std::function<void (int)>       showMenu;            // right-click register i → the product's menu
    std::function<void (int)>       copyToClipboard;     // ⌘C
    std::function<bool (int)>        pasteFromClipboard; // ⌘V → true if handled
};

//==============================================================================
// CompareCell — the A/B/C/D compare block (undo · redo · registers) as one chrome cell. It owns its
// buttons, is its OWN DragAndDropContainer (register-copy drags never leave the cell), pushes model
// state into its buttons, and forwards gestures to product actions. Register keys 1..N are OPT-IN
// (the product forwards keys to handleRegisterKey), never grabbed here.
class CompareCell final : public juce::Component,
                          public juce::DragAndDropContainer
{
public:
    explicit CompareCell (int registerCount)
        : count (juce::jlimit (1, CompareModel::kMaxRegisters, registerCount))
    {
        for (auto* b : { &undo, &redo })
        {
            b->setColour (juce::TextButton::buttonColourId,  tabby::palette::panel());
            b->setColour (juce::TextButton::textColourOffId, tabby::palette::text());
            addAndMakeVisible (*b);
        }
        undo.onClick = [this] { if (actions.undo) actions.undo(); };
        redo.onClick = [this] { if (actions.redo) actions.redo(); };

        for (int i = 0; i < count; ++i)
        {
            auto b = std::make_unique<RegisterButton>();
            b->setRegisterIndex (i);
            b->setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + i)));
            b->setColour (juce::TextButton::buttonColourId,   tabby::palette::panel());
            b->setColour (juce::TextButton::buttonOnColourId, tabby::palette::violet());
            b->setColour (juce::TextButton::textColourOffId,  tabby::palette::textDim());
            b->setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            b->onClick    = [this, i] { if (actions.recall)   actions.recall (i); };
            b->onPopup    = [this, i] { if (actions.showMenu) actions.showMenu (i); };
            b->onCopyDrop = [this]    (int from, int to) { if (actions.copy) actions.copy (from, to); };
            addAndMakeVisible (*b);
            regButtons.push_back (std::move (b));
        }
    }

    void setActions (CompareActions a) { actions = std::move (a); }

    // Push the product's history/register state in. Nothing here reads back into the product.
    void setModel (const CompareModel& m)
    {
        model = m;
        for (int i = 0; i < count; ++i)
        {
            regButtons[(size_t) i]->setToggleState (i == m.active, juce::dontSendNotification);
            regButtons[(size_t) i]->setEdited (i < CompareModel::kMaxRegisters && m.edited[(size_t) i]);
        }
        undo.setEnabled (m.canUndo);
        redo.setEnabled (m.canRedo);
        undo.setTooltip (tip ("Undo", m.canUndo, m.undoLabel, juce::ModifierKeys::commandModifier));
        redo.setTooltip (tip ("Redo", m.canRedo, m.redoLabel,
                              juce::ModifierKeys::Flags (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)));
    }

    const CompareModel& getModel() const noexcept { return model; }

    // OPT-IN register keys 1..N — the product forwards keys here if it wants them. recall() gates
    // navigation itself (product-side), so a key landing mid-drag is a swallowed no-op, as before.
    bool handleRegisterKey (const juce::KeyPress& key)
    {
        for (int i = 0; i < count; ++i)
            if (key == juce::KeyPress ((juce::juce_wchar) ('1' + i)))
            {
                if (actions.recall) actions.recall (i);
                return true;
            }
        return false;
    }

    int registerCount() const noexcept { return count; }
    juce::String registerName (int i) const { return juce::String::charToString ((juce::juce_wchar) ('A' + i)); }

    // The component a product popup (the copy/paste menu) should anchor to — register i's button.
    juce::Component* registerAnchor (int i) { return (i >= 0 && i < count) ? regButtons[(size_t) i].get() : (juce::Component*) this; }

    // The cell's fixed (state-independent) width: undo + redo + tGap + N registers.
    int fixedWidth() const { return kUndoW + kRedoW + kTGap + count * kAbcdW; }

    void resized() override
    {
        const int vi = kVInset, hInner = getHeight() - 2 * kVInset;
        int x = 0;
        undo.setBounds (x, vi, kUndoW, hInner); x += kUndoW;
        redo.setBounds (x, vi, kRedoW, hInner); x += kRedoW;
        x += kTGap;
        for (auto& b : regButtons) { b->setBounds (x, vi, kAbcdW, hInner); x += kAbcdW; }
    }

private:
    static juce::String tip (const juce::String& verb, bool can, const juce::String& label, juce::ModifierKeys::Flags mods)
    {
        // Peek-label tooltip — an unlabelled settle burst reads as "Parameter Change".
        const auto pretty = label.isEmpty() ? juce::String ("Parameter Change") : label;
        const auto hint   = " (" + juce::KeyPress ('z', mods, 0).getTextDescriptionWithIcons() + ")";
        return (can ? verb + " " + pretty : verb) + hint;
    }

    static constexpr int kUndoW = 22, kRedoW = 22, kTGap = 12, kAbcdW = 26, kVInset = 5;   // kTGap: static undo/redo↔A gap

    int count;
    tabby::ui::HistoryArrowButton undo { false }, redo { true };
    std::vector<std::unique_ptr<RegisterButton>> regButtons;
    CompareActions actions;
    CompareModel   model;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompareCell)
};

} // namespace tabby::chrome
