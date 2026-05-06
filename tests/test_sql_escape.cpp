// Tests for SQLExporter::escape — ensures user-provided strings can't
// produce malformed SQL when emitted into INSERT statements.
#include <catch_amalgamated.hpp>
#include "sql_exporter.hpp"

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
