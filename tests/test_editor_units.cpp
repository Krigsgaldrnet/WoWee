// Editor unit tests:
//   - SQLExporter::escape — ensures user-provided strings can't produce
//     malformed SQL when emitted into INSERT statements.
//   - QuestEditor::validateChains — orphan/cycle detection.
//   - ContentPacker::unpackZone — security guards (path traversal,
//     header bounds, name sanitization).
#include <catch_amalgamated.hpp>
#include "sql_exporter.hpp"
#include "quest_editor.hpp"
#include "content_pack.hpp"
#include <fstream>
#include <filesystem>

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

// ============== ContentPacker::unpackZone security tests ==============

namespace { // helpers used only by the WCP tests below

constexpr uint32_t kWCP_MAGIC = 0x31504357; // "WCP1"

// Hand-write a minimal WCP with given header values and a single file
// entry. Used to exercise the loader's defensive bounds without standing
// up the full packZone path.
void writeMalformedWcp(const std::string& path,
                       uint32_t fileCount,
                       uint32_t infoSize,
                       const std::string& infoJson,
                       const std::string& filePath = "",
                       uint32_t fileDataSize = 0) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&kWCP_MAGIC), 4);
    f.write(reinterpret_cast<const char*>(&fileCount), 4);
    f.write(reinterpret_cast<const char*>(&infoSize), 4);
    if (!infoJson.empty())
        f.write(infoJson.data(), std::min<size_t>(infoSize, infoJson.size()));
    if (!filePath.empty()) {
        uint16_t pathLen = static_cast<uint16_t>(filePath.size());
        f.write(reinterpret_cast<const char*>(&pathLen), 2);
        f.write(filePath.data(), pathLen);
        f.write(reinterpret_cast<const char*>(&fileDataSize), 4);
        std::string data(fileDataSize, 'X');
        f.write(data.data(), fileDataSize);
    }
}

} // namespace

TEST_CASE("WCP unpack rejects absurd fileCount", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    writeMalformedWcp("test_wcp_out/huge.wcp", 10'000'000, 16, "{\"name\":\"x\"}");
    REQUIRE_FALSE(ContentPacker::unpackZone("test_wcp_out/huge.wcp", "test_wcp_out/dest"));
    fs::remove_all("test_wcp_out");
}

TEST_CASE("WCP unpack rejects absurd infoSize", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    // 32MB infoSize — past the 16MB cap.
    writeMalformedWcp("test_wcp_out/big_info.wcp", 1, 32 * 1024 * 1024, "{}");
    REQUIRE_FALSE(ContentPacker::unpackZone("test_wcp_out/big_info.wcp",
                                              "test_wcp_out/dest"));
    fs::remove_all("test_wcp_out");
}

TEST_CASE("WCP unpack rejects path traversal entries", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    std::string info = R"({"name":"safe"})";
    writeMalformedWcp("test_wcp_out/trav.wcp",
                      /*fileCount*/ 1,
                      /*infoSize*/ static_cast<uint32_t>(info.size()),
                      info,
                      /*path*/ "../../etc/passwd_clone",
                      /*dataSize*/ 4);
    REQUIRE_FALSE(ContentPacker::unpackZone("test_wcp_out/trav.wcp",
                                              "test_wcp_out/dest"));
    // Confirm the file did NOT escape the test dir
    REQUIRE_FALSE(fs::exists("test_wcp_out/etc/passwd_clone"));
    fs::remove_all("test_wcp_out");
}

TEST_CASE("WCP unpack rejects absolute paths", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    std::string info = R"({"name":"safe"})";
    writeMalformedWcp("test_wcp_out/abs.wcp",
                      1, static_cast<uint32_t>(info.size()), info,
                      "/tmp/wowee_should_not_appear", 4);
    REQUIRE_FALSE(ContentPacker::unpackZone("test_wcp_out/abs.wcp",
                                              "test_wcp_out/dest"));
    REQUIRE_FALSE(fs::exists("/tmp/wowee_should_not_appear"));
    fs::remove_all("test_wcp_out");
}

TEST_CASE("WCP unpack sanitizes zone name", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    std::string info = R"({"name":"../escape_attempt"})";
    writeMalformedWcp("test_wcp_out/badname.wcp",
                      1, static_cast<uint32_t>(info.size()), info,
                      "good.txt", 4);
    REQUIRE(ContentPacker::unpackZone("test_wcp_out/badname.wcp",
                                       "test_wcp_out/dest"));
    // The file should land under the slugified name, not under "../"
    REQUIRE(fs::exists("test_wcp_out/dest/escape_attempt/good.txt"));
    REQUIRE_FALSE(fs::exists("test_wcp_out/escape_attempt/good.txt"));
    fs::remove_all("test_wcp_out");
}

TEST_CASE("WCP unpack rejects bad magic", "[wcp]") {
    namespace fs = std::filesystem;
    fs::create_directories("test_wcp_out");
    {
        std::ofstream f("test_wcp_out/bad.wcp", std::ios::binary);
        uint32_t bad = 0xDEADBEEF;
        f.write(reinterpret_cast<const char*>(&bad), 4);
    }
    REQUIRE_FALSE(ContentPacker::unpackZone("test_wcp_out/bad.wcp",
                                              "test_wcp_out/dest"));
    fs::remove_all("test_wcp_out");
}

// ============== EditorBrush::getInfluence tests ==============

#include "editor_brush.hpp"
#include <cmath>
#include <limits>

TEST_CASE("EditorBrush::getInfluence is full inside the inner radius", "[brush]") {
    EditorBrush b;
    b.settings().radius = 10.0f;
    b.settings().falloff = 0.5f; // inner radius = 5
    REQUIRE(b.getInfluence(0.0f) == 1.0f);
    REQUIRE(b.getInfluence(4.0f) == 1.0f);
    REQUIRE(b.getInfluence(5.0f) == 1.0f);
}

TEST_CASE("EditorBrush::getInfluence is zero at or beyond radius", "[brush]") {
    EditorBrush b;
    b.settings().radius = 10.0f;
    b.settings().falloff = 0.5f;
    REQUIRE(b.getInfluence(10.0f) == 0.0f);
    REQUIRE(b.getInfluence(20.0f) == 0.0f);
}

TEST_CASE("EditorBrush::getInfluence smoothly falls off in the rim", "[brush]") {
    EditorBrush b;
    b.settings().radius = 10.0f;
    b.settings().falloff = 1.0f; // inner radius = 0
    // At t=0.5 the falloff should be 1 - 0.5^2 = 0.75
    REQUIRE(b.getInfluence(5.0f) == Catch::Approx(0.75f).margin(0.001f));
}

TEST_CASE("EditorBrush::getInfluence rejects NaN distance", "[brush]") {
    EditorBrush b;
    b.settings().radius = 10.0f;
    b.settings().falloff = 0.5f;
    REQUIRE(b.getInfluence(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    REQUIRE(b.getInfluence(std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("EditorBrush::getInfluence rejects non-positive radius", "[brush]") {
    EditorBrush b;
    b.settings().radius = 0.0f;
    REQUIRE(b.getInfluence(5.0f) == 0.0f);
    b.settings().radius = -10.0f;
    REQUIRE(b.getInfluence(5.0f) == 0.0f);
    b.settings().radius = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(b.getInfluence(5.0f) == 0.0f);
}

TEST_CASE("EditorBrush::getInfluence handles zero falloff (hard edge)", "[brush]") {
    EditorBrush b;
    b.settings().radius = 10.0f;
    b.settings().falloff = 0.0f;
    REQUIRE(b.getInfluence(0.0f) == 1.0f);
    REQUIRE(b.getInfluence(9.99f) == 1.0f);
    REQUIRE(b.getInfluence(10.0f) == 0.0f);
}
