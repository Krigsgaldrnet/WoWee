# Wowee Open Format Specification v1.2

Novel file formats for custom WoW zone content. No Blizzard IP.

## WOT — Wowee Open Terrain (JSON metadata)
- Extension: `.wot`
- Contains: tile coords, texture list, per-chunk layers/holes, water data,
  doodad placements (M2 objects), WMO placements (buildings)
- Key: `"format": "wot-1.0"`
- Placement fields: `doodadNames[]`, `doodads[]` (nameId, uniqueId, pos, rot, scale, flags)
- WMO fields: `wmoNames[]`, `wmos[]` (nameId, uniqueId, pos, rot, flags, doodadSet)

## WHM — Wowee HeightMap (binary)
- Extension: `.whm`
- Magic: `WHM1` (0x314D4857)
- Layout: magic(4) + chunks(4) + vertsPerChunk(4) + per-chunk data × 256
- Per-chunk: baseHeight(4) + heights[145](580) + alphaSize(4) + alphaData(alphaSize)
- Alpha data: raw alpha blend maps for texture layers (same format as ADT MCAL)
- Backward compatible: older WHM files without alpha data still load (alphaSize=0)

## WOM — Wowee Open Model (binary)
- Extension: `.wom`
- Magic: `WOM1` (0x314D4F57) static, `WOM2` (0x324D4F57) animated, `WOM3` (0x334D4F57) multi-batch
- Layout: magic(4) + vertCount(4) + indexCount(4) + texCount(4) + bounds(28) + name + vertices + indices + texPaths
- WOM1 Vertex: position(vec3) + normal(vec3) + texCoord(vec2) = 32 bytes
- WOM2/WOM3 Vertex: + boneWeights(4) + boneIndices(4) = 40 bytes
- WOM2/WOM3 Bones: boneCount(4) + [keyBoneId(4) + parentBone(2) + pivot(12) + flags(4)] × N
- WOM2/WOM3 Animations: animCount(4) + [id(4) + duration(4) + speed(4) + per-bone keyframes] × N
- Keyframe: timeMs(4) + translation(12) + rotation(16) + scale(12) = 44 bytes
- WOM3 Batches: batchCount(4) + [indexStart(4) + indexCount(4) + textureIndex(4) + blendMode(2) + flags(2)] × N
- WOM3 blendMode: 0=opaque, 1=alpha-test, 2=alpha, 3=add, 4=mod, 5=mod2x, 6=blendAdd, 7=screen
- WOM3 flags: bit 0 = unlit, bit 1 = two-sided, bit 2 = no z-write
- Backward compatible: WOM1 files load without bone/animation data; WOM3 falls back to single-batch when batches block missing

## WOB — Wowee Open Building (binary)
- Extension: `.wob`
- Magic: `WOB1` (0x31424F57)
- Layout: magic(4) + groupCount(4) + portalCount(4) + doodadCount(4) + bounds(4) + name + groups + portals + doodads
- Group: name + vertexCount(4) + indexCount(4) + texCount(4) + outdoor(1) + bounds(24)
  + vertices(pos+normal+uv+color) + indices + texPaths + materialCount(4) + materials
- Material: texturePath + flags(4) + shader(4) + blendMode(4)
- Doodad: modelPath + position(12) + rotation(12) + scale(4)
- Portal: groupA(4) + groupB(4) + vertexCount(4) + vertices

## WCP — Wowee Content Pack (archive)
- Extension: `.wcp`
- Magic: `WCP1` (0x31504357)
- Layout: magic(4) + fileCount(4) + infoJsonSize(4) + infoJSON + [pathLen(2) + path + dataSize(4) + data] × N
- Info JSON includes categorized file list (terrain/model/building/texture/data)

## zone.json — Map Definition
- Replaces WDT
- Fields: `mapName`, `displayName`, `mapId`, `biome`, `baseHeight`
- `hasCreatures`, `description`, `tiles` array, `files` map
- `doodadNames[]`, `doodads[]`, `wmoNames[]`, `wmos[]` for placed objects
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

## WOC — Wowee Open Collision (binary)
- Extension: `.woc`
- Magic: `WOC1` (0x31434F57)
- Layout: magic(4) + triCount(4) + tileX(4) + tileY(4) + boundsMin(12) + boundsMax(12) + triangles
- Triangle: v0(12) + v1(12) + v2(12) + flags(1) = 37 bytes
- Flags: 0x01=walkable, 0x02=water, 0x04=steep, 0x08=indoor
- Generated from terrain heightmap with slope classification (50 deg threshold)
- Respects terrain holes (skips triangles in hole regions)
- WoweeCollisionBuilder::addMesh appends placed WMO group geometry
  (transformed into world space, slope-classified) so collision covers
  buildings as well as terrain.

## Terrain Stamps (.json)
- Portable terrain feature snapshots (mountains, craters, etc.)
- Format: `{"format": "wowee-stamp-1.0", "vertices": [[dx, dy, height], ...]}`
- Can be saved/loaded across zones and sessions

## Open Format Scoring (0-7)
1. WOT terrain metadata present
2. WHM heightmap with valid magic
3. zone.json map definition
4. PNG textures present
5. WOM models with valid magic
6. WOB buildings with valid magic
7. WOC collision mesh with valid magic

## SQL Server Export
- AzerothCore-flavored INSERT statements for: `creature_template`, `creature`,
  `creature_addon`, `waypoint_data`, `quest_template`, `creature_queststarter`,
  `creature_questender`.
- Coordinates: editor render coords are converted to WoW canonical via
  `core::coords::renderToCanonical` (X/Y swap) before write.
- Orientation: editor degrees from +renderX (west) → WoW radians from +X (north).
  Conversion: `wowYaw = π/2 - editorYaw` then normalised to [0, 2π).
- Quest objectives: KillCreature targets fill RequiredNpcOrGo[1..4]; CollectItem
  targets fill RequiredItemId[1..6]. Target ID parsed from objective.targetName.
- Quest givers/turn-in NPCs: written as `creature_queststarter` /
  `creature_questender` rows linking npc.id ↔ quest.id.

## All formats are novel, portable, and open for redistribution.
## No Blizzard intellectual property is used in any format definition.
