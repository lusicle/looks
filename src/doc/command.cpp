#include "doc/command.h"

#include <cassert>

namespace looks::doc {

namespace {

class CompositeCommand final : public Command {
public:
    CompositeCommand(std::string name, std::vector<std::unique_ptr<Command>> cmds)
        : name_(std::move(name)), commands_(std::move(cmds)) {}

    std::string name() const override { return name_; }

    void apply(Document& doc) override {
        for (auto& c : commands_) c->apply(doc);
    }

    void revert(Document& doc) override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it)
            (*it)->revert(doc);
    }

private:
    std::string name_;
    std::vector<std::unique_ptr<Command>> commands_;
};

}  // namespace

void UndoStack::execute(Document& doc, std::unique_ptr<Command> cmd, bool coalesce) {
    assert(cmd);
    cmd->apply(doc);
    ++doc.revision;
    coalescing_active_ = coalesce;
    push_entry(std::move(cmd), coalesce);
}

void UndoStack::push_entry(std::unique_ptr<Command> cmd, bool coalesce) {
    if (group_depth_ > 0) {
        auto& cmds = open_groups_.back().commands;
        if (coalesce && !coalesce_barrier_ && !cmds.empty() && cmds.back()->merge(*cmd))
            return;
        coalesce_barrier_ = false;
        cmds.push_back(std::move(cmd));
        return;
    }

    redo_.clear();
    if (coalesce && !coalesce_barrier_ && !undo_.empty() && undo_.back()->merge(*cmd))
        return;
    coalesce_barrier_ = false;
    undo_.push_back(std::move(cmd));
    if (undo_.size() > max_depth_)
        undo_.erase(undo_.begin());
}

bool UndoStack::undo(Document& doc) {
    if (!can_undo()) return false;
    std::unique_ptr<Command> cmd = std::move(undo_.back());
    undo_.pop_back();
    cmd->revert(doc);
    ++doc.revision;
    redo_.push_back(std::move(cmd));
    coalesce_barrier_ = true;
    return true;
}

bool UndoStack::redo(Document& doc) {
    if (!can_redo()) return false;
    std::unique_ptr<Command> cmd = std::move(redo_.back());
    redo_.pop_back();
    cmd->apply(doc);
    ++doc.revision;
    undo_.push_back(std::move(cmd));
    coalesce_barrier_ = true;
    return true;
}

std::string UndoStack::undo_name() const {
    return can_undo() ? undo_.back()->name() : std::string();
}

void UndoStack::begin_group(std::string name) {
    open_groups_.push_back({std::move(name), {}});
    ++group_depth_;
}

void UndoStack::end_group() {
    assert(group_depth_ > 0);
    --group_depth_;
    Group group = std::move(open_groups_.back());
    open_groups_.pop_back();
    if (group.commands.empty()) return;

    auto composite = std::make_unique<CompositeCommand>(
        std::move(group.name), std::move(group.commands));
    // The group commands ran already: record them without a re-apply.
    push_entry(std::move(composite), /*coalesce=*/false);
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
    open_groups_.clear();
    group_depth_ = 0;
    coalesce_barrier_ = false;
}

}  // namespace looks::doc
