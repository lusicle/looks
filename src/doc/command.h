// Command pattern + undo stack over the whole document.
//
// Rules:
//  - Every document mutation is a Command executed through UndoStack. No
//    direct writes to Document outside a Command's apply/revert.
//  - apply() must be exact-inverse of revert(): undo/redo replays them.
//  - Continuous gestures (param drag) execute with coalesce=true; the stack
//    asks the top command to merge() the newcomer so a drag is one undo step.
//  - begin_group/end_group wraps several commands into a single undo entry
//    (e.g. "delete layer" = unroute mods + unlink + remove layer).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"

namespace looks::doc {

class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;
    virtual void apply(Document& doc) = 0;
    virtual void revert(Document& doc) = 0;
    // Coalescing hook: absorb `next` (already applied to the document) into
    // this command so one undo reverts the whole gesture. Return false to
    // decline; the stack then pushes `next` as its own entry.
    virtual bool merge(const Command& next) {
        (void)next;
        return false;
    }
};

// Base for every mutation scoped to ONE look. The target look is
// captured at construction, never read from an editing cursor: undo and
// redo must land on the look the edit was made in, whatever the UI has
// scoped to since.
class LookCommand : public Command {
public:
    explicit LookCommand(uint64_t look) : look_(look) {}
    uint64_t look_id() const { return look_; }

protected:
    Look& look_of(Document& doc) const { return doc.look(look_); }
    // True when `other` edits the same look — every merge() override
    // checks this before coalescing.
    bool same_look(const LookCommand& other) const {
        return other.look_ == look_;
    }

private:
    uint64_t look_ = 0;
};

// Base for every mutation scoped to ONE sequence — same explicit-scope
// rule as LookCommand: sequences nest, so "the current timeline" is view
// state and undo must not chase it.
class SequenceCommand : public Command {
public:
    explicit SequenceCommand(uint64_t sequence) : sequence_(sequence) {}
    uint64_t sequence_id() const { return sequence_; }

protected:
    Sequence& sequence_of(Document& doc) const {
        return doc.sequence(sequence_);
    }
    bool same_sequence(const SequenceCommand& other) const {
        return other.sequence_ == sequence_;
    }

private:
    uint64_t sequence_ = 0;
};

class UndoStack {
public:
    explicit UndoStack(size_t max_depth = 1024) : max_depth_(max_depth) {}

    // Applies the command, then records it (clearing the redo stack).
    // With coalesce=true the top undo entry gets a chance to merge() it.
    void execute(Document& doc, std::unique_ptr<Command> cmd, bool coalesce = false);

    bool undo(Document& doc);
    bool redo(Document& doc);
    bool can_undo() const { return group_depth_ == 0 && !undo_.empty(); }
    bool can_redo() const { return group_depth_ == 0 && !redo_.empty(); }

    // Names for menu display ("Undo Move Layer"). Empty when unavailable.
    std::string undo_name() const;
    std::string redo_name() const;

    // Transaction: commands executed between begin/end collapse into one
    // entry named `name`. Groups nest; only the outermost end pushes.
    void begin_group(std::string name);
    void end_group();

    // Ends a coalescing run: the next execute(..., coalesce=true) will not
    // merge into the current top. Call on gesture end (mouse up).
    void break_coalescing() {
        coalesce_barrier_ = true;
        coalescing_active_ = false;
    }

    // True between a coalesced execute and its break_coalescing: a drag
    // is mutating the document every frame. The render side skips cache
    // hashing/readback while this holds - those entries can never be
    // reused and their cost is what makes gestures lag.
    bool coalescing_active() const { return coalescing_active_; }

    void clear();
    size_t undo_depth() const { return undo_.size(); }

private:
    struct Group {
        std::string name;
        std::vector<std::unique_ptr<Command>> commands;
    };

    void push_entry(std::unique_ptr<Command> cmd, bool coalesce);

    std::vector<std::unique_ptr<Command>> undo_;
    std::vector<std::unique_ptr<Command>> redo_;
    std::vector<Group> open_groups_;
    size_t max_depth_;
    int group_depth_ = 0;
    bool coalesce_barrier_ = false;
    bool coalescing_active_ = false;
};

}  // namespace looks::doc
