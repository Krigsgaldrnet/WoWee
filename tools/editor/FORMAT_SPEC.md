# Wowee Open Format Specification v1.0

Novel file formats for custom WoW zone content. No Blizzard IP.

## WOT — Wowee Open Terrain (JSON metadata)
- Extension: `.wot`
- Contains: tile coords, texture list, per-chunk layers/holes, water data
- Key: `"format": "wot-1.0"`

## WHM — Wowee HeightMap (binary)
- Extension: `.whm`
- Magic: `WHM1` (0x314D4857)
- Version: 1 (embedded in magic — WHM2 for future revisions)
- Layout: magic(4) + chunks(4) + vertsPerChunk(4) + per-chunk data × 256
- Per-chunk: baseHeight(4) + heights[145](580) + alphaSize(4) + alphaData(alphaSize)
- Alpha data: raw alpha blend maps for texture layers (same format as ADT MCAL)
- Backward compatible: older WHM files without alpha data still load (alphaSize=0)

## WOM — Wowee Open Model (binary)
- Extension: `.wom`
- Magic: `WOM1` (0x314D4F57) — version 1
- Version: embedded in magic (WOM2 for future revisions with animation support)
- Layout: magic(4) + vertCount(4) + indexCount(4) + texCount(4) + bounds(28) + name + vertices + indices + texPaths
- Vertex: position(vec3) + normal(vec3) + texCoord(vec2) = 32 bytes

## WOB — Wowee Open Building (binary)
- Extension: `.wob`
- Magic: `WOB1` (0x31424F57)
- Layout: magic(4) + groupCount(4) + portalCount(4) + doodadCount(4) + bounds(4) + name + groups + portals + doodads
- Group: name + vertices(pos+normal+uv+color) + indices + texPaths + bounds + outdoor flag

## WCP — Wowee Content Pack (archive)
- Extension: `.wcp`
- Magic: `WCP1` (0x31504357)
- Layout: magic(4) + fileCount(4) + infoJsonSize(4) + infoJSON + [pathLen(2) + path + dataSize(4) + data] × N

## zone.json — Map Definition
- Replaces WDT
- Contains: mapName, mapId, tiles, biome, file references

## zone.json Fields
- `mapName`, `displayName`, `mapId`, `biome`, `baseHeight`
- `hasCreatures`, `description`, `tiles` array, `files` map
- `editorVersion` for compatibility tracking

## JSON DBC — Data Table Replacement
- Replaces binary DBC files
- Format: `{"format": "wowee-dbc-json-1.0", "records": [...], "fieldCount": N}`
- Records are arrays of mixed types: integers, floats, strings
- Client loads via DBCFile::loadJSON() when found in custom_zones/ or output/

## PNG Textures — Texture Replacement
- Replaces BLP texture files
- Standard PNG format, loaded by client's texture override system
- Editor auto-converts BLP→PNG on export via stb_image_write

## Open Format Scoring (0-6)
1. WOT terrain metadata present
2. WHM heightmap with valid magic
3. zone.json map definition
4. PNG textures present
5. WOM models with valid magic
6. WOB buildings with valid magic

## All formats are novel, portable, and open for redistribution.
## No Blizzard intellectual property is used in any format definition.
