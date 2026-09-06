// apply() must be the exact inverse of revert(): undo and redo replay them.

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
    // next is applied to the document already: absorb it, or return false.
    virtual bool merge(const Command& next) {
        (void)next;
        return false;
    }
};

// The target look is captured at construction, never from the UI scope.
class LookCommand : public Command {
public:
    explicit LookCommand(uint64_t look) : look_(look) {}

protected:
    Look& entity_of(Document& doc) const { return doc.look(look_); }
    // Every merge() override must check this before it coalesces.
    bool same_entity(const LookCommand& other) const {
        return other.look_ == look_;
    }

private:
    uint64_t look_ = 0;
};

// The target sequence is captured at construction, never from the UI scope.
class SequenceCommand : public Command {
public:
    explicit SequenceCommand(uint64_t sequence) : sequence_(sequence) {}

protected:
    Sequence& entity_of(Document& doc) const {
        return doc.sequence(sequence_);
    }
    bool same_entity(const SequenceCommand& other) const {
        return other.sequence_ == sequence_;
    }

private:
    uint64_t sequence_ = 0;
};

class UndoStack {
public:
    void execute(Document& doc, std::unique_ptr<Command> cmd, bool coalesce = false);

    bool undo(Document& doc);
    bool redo(Document& doc);
    bool can_undo() const { return open_groups_.empty() && !undo_.empty(); }
    bool can_redo() const { return open_groups_.empty() && !redo_.empty(); }

    std::string undo_name() const;

    // Groups nest: only the outermost end_group pushes an entry.
    void begin_group(std::string name);
    void end_group();

    // Call at gesture end. The next coalesced execute starts a new entry.
    void break_coalescing() {
        coalesce_barrier_ = true;
        coalescing_active_ = false;
    }

    // True during a drag: the render side skips cache hashing while it holds.
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
    size_t max_depth_ = 1024;
    bool coalesce_barrier_ = false;
    bool coalescing_active_ = false;
};

}  // namespace looks::doc
