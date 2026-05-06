// Editor unit tests:
//   - SQLExporter::escape — ensures user-provided strings can't produce
//     malformed SQL when emitted into INSERT statements.
//   - QuestEditor::validateChains — orphan/cycle detection.
#include <catch_amalgamated.hpp>
#include "sql_exporter.hpp"
#include "quest_editor.hpp"

using namespace wowee::editor;

TEST_CASE("SQLExporter::escape doubles single quotes", "[sql]") {
    REQUIRE(SQLExporter::escape("King's Land") == "King''s Land");
    REQUIRE(SQLExporter::escape("''''") == "''''''''");
}

TEST_CASE("SQLExporter::escape escapes backslashes", "[sql]") {
    REQUIRE(SQLExporter::escape("path\\to\\file") == "path\\\\to\\\\file");
}

TEST_CASE("SQLExporter::escape passes through ordinary text unchanged", "[sql]") {
    REQUIRE(SQLExporter::escape("Hello, world!") == "Hello, world!");
    REQUIRE(SQLExporter::escape("") == "");
    REQUIRE(SQLExporter::escape("Some-Name_123") == "Some-Name_123");
}

TEST_CASE("SQLExporter::escape handles control characters", "[sql]") {
    // NUL is dropped (some clients don't respect length-prefixed strings)
    std::string withNul("a", 1);
    withNul += '\0';
    withNul += 'b';
    REQUIRE(SQLExporter::escape(withNul) == "ab");

    // Newlines/CR/tab become escape sequences so each INSERT stays on one line
    REQUIRE(SQLExporter::escape("a\nb") == "a\\nb");
    REQUIRE(SQLExporter::escape("a\rb") == "a\\rb");
    REQUIRE(SQLExporter::escape("a\tb") == "a\\tb");

    // Ctrl-Z (historical MySQL string terminator on Windows)
    std::string withCtrlZ;
    withCtrlZ += 'a';
    withCtrlZ += static_cast<char>(26);
    withCtrlZ += 'b';
    REQUIRE(SQLExporter::escape(withCtrlZ) == "a\\Zb");
}

TEST_CASE("SQLExporter::escape combines escapes correctly", "[sql]") {
    REQUIRE(SQLExporter::escape("O'Brien\\path") == "O''Brien\\\\path");
}

// ============== Quest validateChains tests ==============

TEST_CASE("Quest::validateChains flags non-existent next quest", "[quest]") {
    QuestEditor qe;
    Quest a;
    a.title = "First";
    a.questGiverNpcId = 1;
    a.nextQuestId = 999; // Doesn't exist
    qe.addQuest(a);

    std::vector<std::string> errors;
    REQUIRE_FALSE(qe.validateChains(errors));
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].find("non-existent quest 999") != std::string::npos);
}

TEST_CASE("Quest::validateChains flags orphans with no questgiver/turn-in", "[quest]") {
    QuestEditor qe;
    Quest unreachable;
    unreachable.title = "Floating Quest";
    qe.addQuest(unreachable);

    std::vector<std::string> errors;
    REQUIRE_FALSE(qe.validateChains(errors));
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].find("unreachable") != std::string::npos);
}

TEST_CASE("Quest::validateChains accepts a quest with only a turn-in NPC", "[quest]") {
    QuestEditor qe;
    Quest a;
    a.title = "Drop quest";
    a.turnInNpcId = 42; // Reachable via turn-in (auto-completed quest)
    qe.addQuest(a);

    std::vector<std::string> errors;
    REQUIRE(qe.validateChains(errors));
    REQUIRE(errors.empty());
}

TEST_CASE("Quest::validateChains detects circular chain", "[quest]") {
    QuestEditor qe;
    Quest a; a.title = "Q1"; a.questGiverNpcId = 1; qe.addQuest(a);
    Quest b; b.title = "Q2"; b.questGiverNpcId = 1; qe.addQuest(b);
    // Set the chain (the addQuest assigns ids 1 and 2 sequentially)
    auto* qa = qe.getQuest(0);
    auto* qb = qe.getQuest(1);
    qa->nextQuestId = qb->id;
    qb->nextQuestId = qa->id; // cycle

    std::vector<std::string> errors;
    REQUIRE_FALSE(qe.validateChains(errors));
    bool foundCycle = false;
    for (const auto& e : errors) {
        if (e.find("Circular") != std::string::npos) foundCycle = true;
    }
    REQUIRE(foundCycle);
}
