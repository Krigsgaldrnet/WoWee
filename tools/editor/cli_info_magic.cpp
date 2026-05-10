#include "cli_info_magic.hpp"
#include "cli_arg_parse.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace wowee {
namespace editor {
namespace cli {

namespace {

// Format identification table — duplicated semantics with
// cli_list_formats.cpp because the two flags can drift in
// what they expose. Keep this list aligned when adding new
// formats. Entries ordered as they appear in --list-formats.
struct MagicEntry {
    char magic[4];
    const char* extension;
    const char* category;
    const char* infoFlag;       // suggested --info-* flag (or null)
    const char* description;
};

constexpr MagicEntry kMagicTable[] = {
    {{'W','O','M',' '}, ".wom",   "asset",     nullptr,            "M2 model"},
    {{'W','O','B',' '}, ".wob",   "asset",     nullptr,            "WMO building"},
    {{'W','H','M',' '}, ".whm",   "world",     nullptr,            "ADT heightmap"},
    {{'W','O','T',' '}, ".wot",   "world",     nullptr,            "ADT textures"},
    {{'W','O','W',' '}, ".wow",   "world",     nullptr,            "Per-zone world manifest"},
    {{'W','I','T','M'}, ".wit",   "items",     "--info-witm",      "Item catalog"},
    {{'W','C','R','T'}, ".wcrt",  "creatures", "--info-creatures", "Creature catalog"},
    {{'W','S','P','N'}, ".wspn",  "spawns",    "--info-spawns",    "Spawn catalog"},
    {{'W','L','O','T'}, ".wlot",  "loot",      "--info-loot",      "Loot tables"},
    {{'W','G','O','T'}, ".wgot",  "objects",   "--info-objects",   "GameObject catalog"},
    {{'W','S','N','D'}, ".wsnd",  "audio",     "--info-sound",     "Sound entries"},
    {{'W','S','P','L'}, ".wspl",  "spells",    "--info-wspl",      "Spell catalog"},
    {{'W','Q','T','M'}, ".wqt",   "quests",    "--info-quests",    "Quest catalog"},
    {{'W','M','S','X'}, ".wms",   "maps",      "--info-wms",       "Map / area catalog"},
    {{'W','C','H','C'}, ".wchc",  "chars",     "--info-wchc",      "Class + race catalog"},
    {{'W','A','C','H'}, ".wach",  "achieve",   "--info-wach",      "Achievement catalog"},
    {{'W','T','R','N'}, ".wtrr",  "trainers",  "--info-wtrr",      "Trainer catalog"},
    {{'W','G','S','P'}, ".wgoss", "gossip",    "--info-wgoss",     "Gossip menu catalog"},
    {{'W','T','A','X'}, ".wtax",  "taxi",      "--info-wtax",      "Taxi node catalog"},
    {{'W','T','A','L'}, ".wtal",  "talents",   "--info-wtal",      "Talent catalog"},
    {{'W','T','K','N'}, ".wtkn",  "tokens",    "--info-wtkn",      "Token catalog"},
    {{'W','T','R','G'}, ".wtrg",  "triggers",  "--info-wtrg",      "Trigger catalog"},
    {{'W','T','I','T'}, ".wttl",  "titles",    "--info-wttl",      "Title catalog"},
    {{'W','S','E','A'}, ".wevt",  "events",    "--info-wevt",      "Event catalog"},
    {{'W','M','O','U'}, ".wmnt",  "mounts",    "--info-wmnt",      "Mount catalog"},
    {{'W','B','G','D'}, ".wbgd",  "battle",    "--info-wbgd",      "Battleground catalog"},
    {{'W','M','A','L'}, ".wmal",  "mail",      "--info-wmal",      "Mail catalog"},
    {{'W','G','E','M'}, ".wgem",  "gems",      "--info-wgem",      "Gem catalog"},
    {{'W','G','L','D'}, ".wgld",  "guilds",    "--info-wgld",      "Guild catalog"},
    {{'W','P','C','D'}, ".wcnd",  "cond",      "--info-wcnd",      "Condition catalog"},
    {{'W','P','E','T'}, ".wpet",  "pets",      "--info-wpet",      "Pet catalog"},
    {{'W','A','U','C'}, ".wauc",  "auction",   "--info-wauc",      "Auction catalog"},
    {{'W','C','H','N'}, ".wchn",  "channels",  "--info-wchn",      "Channel catalog"},
    {{'W','C','M','S'}, ".wcms",  "cinematic", "--info-wcms",      "Cinematic catalog"},
    {{'W','G','L','Y'}, ".wgly",  "glyphs",    "--info-wgly",      "Glyph catalog"},
    {{'W','V','H','C'}, ".wvhc",  "vehicles",  "--info-wvhc",      "Vehicle catalog"},
    {{'W','H','O','L'}, ".whol",  "holiday",   "--info-whol",      "Holiday catalog"},
    {{'W','L','I','Q'}, ".wliq",  "liquids",   "--info-wliq",      "Liquid catalog"},
    {{'W','A','N','I'}, ".wani",  "anim",      "--info-wani",      "Animation catalog"},
    {{'W','F','A','C'}, ".wfac",  "factions",  nullptr,            "Faction catalog"},
    {{'W','L','C','K'}, ".wlck",  "locks",     nullptr,            "Lock catalog"},
    {{'W','S','K','L'}, ".wskl",  "skills",    nullptr,            "Skill catalog"},
    {{'W','O','L','A'}, ".wola",  "light",     nullptr,            "Outdoor light catalog"},
    {{'W','O','W','A'}, ".wowa",  "weather",   nullptr,            "Weather schedule catalog"},
    {{'W','M','P','X'}, ".wmpx",  "worldmap",  nullptr,            "World map catalog"},
};

constexpr size_t kMagicTableCount =
    sizeof(kMagicTable) / sizeof(kMagicTable[0]);

const MagicEntry* findEntry(const char magic[4]) {
    for (const auto& row : kMagicTable) {
        if (std::memcmp(row.magic, magic, 4) == 0) return &row;
    }
    return nullptr;
}

// Read the 4-byte magic + 4-byte version + length-prefixed
// catalog name + 4-byte entry count from the standard
// header that every Wowee catalog format shares. Asset and
// world formats use different headers, so we report only
// magic+version for those.
struct StandardHeader {
    uint32_t version = 0;
    std::string catalogName;
    uint32_t entryCount = 0;
    bool hasCount = false;
};

bool readStandardHeader(std::ifstream& is, StandardHeader& out) {
    if (!is.read(reinterpret_cast<char*>(&out.version), 4)) return false;
    uint32_t nameLen = 0;
    if (!is.read(reinterpret_cast<char*>(&nameLen), 4)) return false;
    if (nameLen > (1u << 20)) return false;
    out.catalogName.resize(nameLen);
    if (nameLen > 0) {
        if (!is.read(out.catalogName.data(), nameLen)) return false;
    }
    if (!is.read(reinterpret_cast<char*>(&out.entryCount), 4)) {
        // No count — version+name only (older variants)
        return true;
    }
    out.hasCount = true;
    return true;
}

int handleMagic(int& i, int argc, char** argv) {
    std::string path = argv[++i];
    bool jsonOut = consumeJsonFlag(i, argc, argv);
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        std::fprintf(stderr,
            "info-magic: cannot read %s\n", path.c_str());
        return 1;
    }
    char magic[4];
    if (!is.read(magic, 4) || is.gcount() != 4) {
        std::fprintf(stderr,
            "info-magic: file too short to read 4-byte magic: %s\n",
            path.c_str());
        return 1;
    }
    const MagicEntry* entry = findEntry(magic);
    StandardHeader hdr;
    bool standardOk = false;
    // World/asset formats have non-standard headers — skip
    // the version+name+count probe for those (entry->infoFlag
    // is null for them).
    if (entry && entry->infoFlag != nullptr) {
        standardOk = readStandardHeader(is, hdr);
    }
    if (jsonOut) {
        nlohmann::json j;
        j["path"] = path;
        char magicStr[5] = {magic[0], magic[1], magic[2], magic[3], 0};
        j["magic"] = magicStr;
        if (entry) {
            j["recognized"] = true;
            j["extension"] = entry->extension;
            j["category"] = entry->category;
            j["description"] = entry->description;
            if (entry->infoFlag) j["infoFlag"] = entry->infoFlag;
            if (standardOk) {
                j["version"] = hdr.version;
                j["catalogName"] = hdr.catalogName;
                if (hdr.hasCount) j["entryCount"] = hdr.entryCount;
            }
        } else {
            j["recognized"] = false;
        }
        std::printf("%s\n", j.dump(2).c_str());
        return entry ? 0 : 1;
    }
    char magicStr[5] = {magic[0], magic[1], magic[2], magic[3], 0};
    if (!entry) {
        std::printf("info-magic: %s\n", path.c_str());
        std::printf("  magic       : '%s' (unrecognized)\n", magicStr);
        std::printf("  hint        : not a Wowee open format file\n");
        return 1;
    }
    std::printf("info-magic: %s\n", path.c_str());
    std::printf("  magic       : '%s'\n", magicStr);
    std::printf("  format      : %s (%s)\n", entry->description, entry->extension);
    std::printf("  category    : %s\n", entry->category);
    if (standardOk) {
        std::printf("  version     : %u\n", hdr.version);
        std::printf("  catalogName : %s\n", hdr.catalogName.c_str());
        if (hdr.hasCount) {
            std::printf("  entryCount  : %u\n", hdr.entryCount);
        }
    }
    if (entry->infoFlag) {
        std::printf("  inspect with: %s %s\n",
                    entry->infoFlag, path.c_str());
    } else {
        std::printf("  inspect with: (no --info-* flag — load via "
                     "engine or asset extractor)\n");
    }
    return 0;
}

} // namespace

bool handleInfoMagic(int& i, int argc, char** argv, int& outRc) {
    if (std::strcmp(argv[i], "--info-magic") == 0 && i + 1 < argc) {
        outRc = handleMagic(i, argc, argv); return true;
    }
    return false;
}

} // namespace cli
} // namespace editor
} // namespace wowee
