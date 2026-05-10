#include "cli_format_table.hpp"

#include <cstring>

namespace wowee {
namespace editor {
namespace cli {

namespace {

constexpr FormatMagicEntry kFormats[] = {
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
    {{'W','S','V','K'}, ".wsvk",  "spellfx",   "--info-wsvk",      "Spell visual kit catalog"},
    {{'W','W','U','I'}, ".wwui",  "ui",        "--info-wwui",      "World-state UI catalog"},
    {{'W','P','C','N'}, ".wpcn",  "logic",     "--info-wpcn",      "Player condition catalog"},
    {{'W','T','S','K'}, ".wtsk",  "crafting",  "--info-wtsk",      "Trade skill recipe catalog"},
    {{'W','C','E','Q'}, ".wceq",  "creatures", "--info-wceq",      "Creature equipment loadout catalog"},
    {{'W','F','A','C'}, ".wfac",  "factions",  nullptr,            "Faction catalog"},
    {{'W','L','C','K'}, ".wlck",  "locks",     nullptr,            "Lock catalog"},
    {{'W','S','K','L'}, ".wskl",  "skills",    nullptr,            "Skill catalog"},
    {{'W','O','L','A'}, ".wola",  "light",     nullptr,            "Outdoor light catalog"},
    {{'W','O','W','A'}, ".wowa",  "weather",   nullptr,            "Weather schedule catalog"},
    {{'W','M','P','X'}, ".wmpx",  "worldmap",  nullptr,            "World map catalog"},
};

constexpr size_t kFormatsCount =
    sizeof(kFormats) / sizeof(kFormats[0]);

} // namespace

const FormatMagicEntry* findFormatByMagic(const char magic[4]) {
    for (const auto& row : kFormats) {
        if (std::memcmp(row.magic, magic, 4) == 0) return &row;
    }
    return nullptr;
}

const FormatMagicEntry* formatTableBegin() { return kFormats; }
const FormatMagicEntry* formatTableEnd() { return kFormats + kFormatsCount; }
size_t formatTableSize() { return kFormatsCount; }

} // namespace cli
} // namespace editor
} // namespace wowee
