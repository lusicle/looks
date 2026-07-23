#include "doc/command.h"
#include "test_framework.h"

using looks::doc::Command;
using looks::doc::Document;
using looks::doc::UndoStack;

namespace {

// Test command: sets master_seed, remembers the previous value.
class SetSeed final : public Command {
public:
    explicit SetSeed(uint64_t value) : value_(value) {}
    std::string name() const override { return "Set Seed"; }
    void apply(Document& doc) override {
        previous_ = doc.master_seed;
        doc.master_seed = value_;
    }
    void revert(Document& doc) override { doc.master_seed = previous_; }
    bool merge(const Command& next) override {
        if (auto* other = dynamic_cast<const SetSeed*>(&next)) {
            value_ = other->value_;   // keep our `previous_` — gesture start
            return true;
        }
        return false;
    }

private:
    uint64_t value_;
    uint64_t previous_ = 0;
};

class AppendName final : public Command {
public:
    explicit AppendName(std::string suffix) : suffix_(std::move(suffix)) {}
    std::string name() const override { return "Append Name"; }
    void apply(Document& doc) override { doc.name += suffix_; }
    void revert(Document& doc) override {
        doc.name.resize(doc.name.size() - suffix_.size());
    }

private:
    std::string suffix_;
};

}  // namespace

TEST(command_execute_undo_redo) {
    Document doc;
    UndoStack stack;
    CHECK(!stack.can_undo());
    CHECK(!stack.can_redo());

    stack.execute(doc, std::make_unique<SetSeed>(7));
    CHECK_EQ(doc.master_seed, uint64_t{7});
    CHECK(stack.can_undo());
    CHECK_EQ(stack.undo_name(), "Set Seed");

    stack.execute(doc, std::make_unique<SetSeed>(9));
    CHECK_EQ(doc.master_seed, uint64_t{9});

    CHECK(stack.undo(doc));
    CHECK_EQ(doc.master_seed, uint64_t{7});
    CHECK(stack.can_redo());
    CHECK(stack.undo(doc));
    CHECK_EQ(doc.master_seed, uint64_t{0});
    CHECK(!stack.undo(doc));

    CHECK(stack.redo(doc));
    CHECK(stack.redo(doc));
    CHECK_EQ(doc.master_seed, uint64_t{9});
    CHECK(!stack.redo(doc));
}

TEST(command_execute_clears_redo) {
    Document doc;
    UndoStack stack;
    stack.execute(doc, std::make_unique<SetSeed>(1));
    stack.execute(doc, std::make_unique<SetSeed>(2));
    stack.undo(doc);
    CHECK(stack.can_redo());
    stack.execute(doc, std::make_unique<SetSeed>(5));
    CHECK(!stack.can_redo());
    CHECK_EQ(doc.master_seed, uint64_t{5});
}

TEST(command_coalescing) {
    Document doc;
    UndoStack stack;
    // A drag: many coalesced updates -> one undo entry back to the start.
    stack.execute(doc, std::make_unique<SetSeed>(10), true);
    stack.execute(doc, std::make_unique<SetSeed>(20), true);
    stack.execute(doc, std::make_unique<SetSeed>(30), true);
    CHECK_EQ(doc.master_seed, uint64_t{30});
    CHECK_EQ(stack.undo_depth(), size_t{1});
    stack.undo(doc);
    CHECK_EQ(doc.master_seed, uint64_t{0});

    // break_coalescing starts a new gesture.
    stack.redo(doc);
    stack.break_coalescing();
    stack.execute(doc, std::make_unique<SetSeed>(40), true);
    CHECK_EQ(stack.undo_depth(), size_t{2});
}

TEST(command_groups) {
    Document doc;
    doc.name = "a";
    UndoStack stack;
    stack.begin_group("Compound Edit");
    stack.execute(doc, std::make_unique<AppendName>("b"));
    stack.execute(doc, std::make_unique<SetSeed>(99));
    CHECK(!stack.can_undo());   // group still open
    stack.end_group();
    CHECK_EQ(doc.name, "ab");
    CHECK_EQ(doc.master_seed, uint64_t{99});
    CHECK_EQ(stack.undo_depth(), size_t{1});
    CHECK_EQ(stack.undo_name(), "Compound Edit");

    stack.undo(doc);   // reverts both, in reverse order
    CHECK_EQ(doc.name, "a");
    CHECK_EQ(doc.master_seed, uint64_t{0});

    stack.redo(doc);
    CHECK_EQ(doc.name, "ab");
    CHECK_EQ(doc.master_seed, uint64_t{99});
}

TEST(command_empty_group_pushes_nothing) {
    Document doc;
    UndoStack stack;
    stack.begin_group("Nothing");
    stack.end_group();
    CHECK(!stack.can_undo());
}

TEST(command_revision_bumps) {
    Document doc;
    UndoStack stack;
    const uint64_t r0 = doc.revision;
    stack.execute(doc, std::make_unique<SetSeed>(1));
    CHECK(doc.revision > r0);
    const uint64_t r1 = doc.revision;
    stack.undo(doc);
    CHECK(doc.revision > r1);
}
