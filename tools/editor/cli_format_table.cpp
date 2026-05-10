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
    {{'W','S','E','T'}, ".wset",  "items",     "--info-wset",      "Item set / tier-bonus catalog"},
    {{'W','G','T','P'}, ".wgtp",  "ui",        "--info-wgtp",      "Game tips / tutorial catalog"},
    {{'W','C','M','P'}, ".wcmp",  "pets",      "--info-wcmp",      "Companion / vanity pet catalog"},
    {{'W','S','M','C'}, ".wsmc",  "spells",    "--info-wsmc",      "Spell mechanic catalog"},
    {{'W','K','B','D'}, ".wkbd",  "input",     "--info-wkbd",      "Keybinding catalog"},
    {{'W','S','C','H'}, ".wsch",  "spells",    "--info-wsch",      "Spell school / damage type catalog"},
    {{'W','L','F','G'}, ".wlfg",  "social",    "--info-wlfg",      "LFG / Dungeon Finder catalog"},
    {{'W','M','A','C'}, ".wmac",  "ui",        "--info-wmac",      "Macro / slash command catalog"},
    {{'W','C','H','F'}, ".wchf",  "chars",     "--info-wchf",      "Character hair / face customization catalog"},
    {{'W','P','V','P'}, ".wpvp",  "pvp",       "--info-wpvp",      "PvP honor rank + arena tier catalog"},
    {{'W','B','N','K'}, ".wbnk",  "items",     "--info-wbnk",      "Bag / bank slot catalog"},
    {{'W','R','U','N'}, ".wrun",  "spells",    "--info-wrun",      "Death Knight rune cost catalog"},
    {{'W','L','D','S'}, ".wlds",  "ui",        "--info-wlds",      "Loading screen catalog"},
    {{'W','S','U','F'}, ".wsuf",  "items",     "--info-wsuf",      "Item random-suffix catalog"},
    {{'W','C','R','R'}, ".wcrr",  "stats",     "--info-wcrr",      "Combat rating conversion catalog"},
    {{'W','U','M','V'}, ".wumv",  "stats",     "--info-wumv",      "Unit movement type catalog"},
    {{'W','Q','S','O'}, ".wqso",  "quests",    "--info-wqso",      "Quest sort / category catalog"},
    {{'W','S','R','G'}, ".wsrg",  "spells",    "--info-wsrg",      "Spell range bucket catalog"},
    {{'W','S','C','T'}, ".wsct",  "spells",    "--info-wsct",      "Spell cast time bucket catalog"},
    {{'W','S','D','R'}, ".wsdr",  "spells",    "--info-wsdr",      "Spell duration bucket catalog"},
    {{'W','S','C','D'}, ".wscd",  "spells",    "--info-wscd",      "Spell cooldown category catalog"},
    {{'W','C','E','F'}, ".wcef",  "creatures", "--info-wcef",      "Creature / pet family catalog"},
    {{'W','S','P','C'}, ".wspc",  "spells",    "--info-wspc",      "Spell power cost bucket catalog"},
    {{'W','G','F','S'}, ".wgfs",  "glyphs",    "--info-wgfs",      "Glyph slot layout catalog"},
    {{'W','C','D','F'}, ".wcdf",  "creatures", "--info-wcdf",      "Creature difficulty variant catalog"},
    {{'W','M','A','T'}, ".wmat",  "items",     "--info-wmat",      "Item material catalog"},
    {{'W','P','S','P'}, ".wpsp",  "chars",     "--info-wpsp",      "Player spawn profile catalog"},
    {{'W','T','L','E'}, ".wtle",  "talents",   "--info-wtle",      "Talent tab / tree catalog"},
    {{'W','C','T','R'}, ".wctr",  "currency",  "--info-wctr",      "Currency type catalog"},
    {{'W','S','P','R'}, ".wspr",  "spells",    "--info-wspr",      "Spell reagent set catalog"},
    {{'W','A','C','R'}, ".wacr",  "achieve",   "--info-wacr",      "Achievement criteria catalog"},
    {{'W','S','E','F'}, ".wsef",  "spells",    "--info-wsef",      "Spell effect type catalog"},
    {{'W','A','U','R'}, ".waur",  "spells",    "--info-waur",      "Spell aura type catalog"},
    {{'W','I','Q','R'}, ".wiqr",  "items",     "--info-wiqr",      "Item quality tier catalog"},
    {{'W','S','C','S'}, ".wscs",  "skills",    "--info-wscs",      "Skill cost / training tier catalog"},
    {{'W','I','F','S'}, ".wifs",  "items",     "--info-wifs",      "Item flag bit catalog"},
    {{'W','B','K','D'}, ".wbkd",  "npcs",      "--info-wbkd",      "NPC service definition catalog"},
    {{'W','T','B','R'}, ".wtbr",  "tokens",    "--info-wtbr",      "Token reward redemption catalog"},
    {{'W','S','P','S'}, ".wsps",  "spells",    "--info-wsps",      "Spell proc trigger catalog"},
    {{'W','C','M','R'}, ".wcmr",  "creatures", "--info-wcmr",      "Creature patrol path catalog"},
    {{'W','B','O','S'}, ".wbos",  "raid",      "--info-wbos",      "Boss encounter definition catalog"},
    {{'W','H','L','D'}, ".whld",  "raid",      "--info-whld",      "Instance lockout schedule catalog"},
    {{'W','S','T','C'}, ".wstc",  "pets",      "--info-wstc",      "Hunter stable slot catalog"},
    {{'W','S','T','M'}, ".wstm",  "stats",     "--info-wstm",      "Stat modifier curve catalog"},
    {{'W','A','C','T'}, ".wact",  "ui",        "--info-wact",      "Action bar layout catalog"},
    {{'W','G','R','P'}, ".wgrp",  "social",    "--info-wgrp",      "Group composition catalog"},
    {{'W','H','R','T'}, ".whrt",  "social",    "--info-whrt",      "Hearthstone bind point catalog"},
    {{'W','S','C','B'}, ".wscb",  "server",    "--info-wscb",      "Server channel broadcast catalog"},
    {{'W','C','M','G'}, ".wcmg",  "spells",    "--info-wcmg",      "Combat maneuver group catalog"},
    {{'W','M','S','P'}, ".wmsp",  "server",    "--info-wmsp",      "Master server profile / realmlist catalog"},
    {{'W','E','M','O'}, ".wemo",  "social",    "--info-wemo",      "Emote definition catalog"},
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
