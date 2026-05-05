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
- Layout: magic(4) + chunks(4) + vertsPerChunk(4) + [baseHeight(4) + heights[145](580)] × 256
- Total size: 12 + 256 × 584 = 149,516 bytes

## WOM — Wowee Open Model (binary)
- Extension: `.wom`
- Magic: `WOM1` (0x314D4F57)
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

## All formats are novel, portable, and open for redistribution.
