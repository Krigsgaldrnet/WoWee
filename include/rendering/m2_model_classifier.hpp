#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstddef>

namespace wowee {
namespace rendering {

/**
 * Output of classifyM2Model(): all name/geometry-based flags for an M2 model.
 * Pure data — no Vulkan, GPU, or asset-manager dependencies.
 */
struct M2ClassificationResult {
    // --- Collision shape selectors ---
    bool collisionNoBlock            = false; ///< Foliage/soft-trees/rugs: no blocking
    bool collisionBridge             = false; ///< Walk-on-top bridge/plank/walkway
    bool collisionPlanter            = false; ///< Low stepped planter/curb
    bool collisionSteppedFountain    = false; ///< Stepped fountain base
    bool collisionSteppedLowPlatform = false; ///< Low stepped platform (curb/planter/bridge)
    bool collisionStatue             = false; ///< Statue/monument/sculpture
    bool collisionSmallSolidProp     = false; ///< Blockable solid prop (crate/chest/barrel)
    bool collisionNarrowVerticalProp = false; ///< Narrow tall prop (lamp/post/pole)
    bool collisionTreeTrunk          = false; ///< Tree trunk cylinder

    // --- Rendering / effect classification ---
    bool isFoliageLike      = false; ///< Foliage or tree (wind sway, disabled animation)
    bool isSpellEffect      = false; ///< Spell effect / particle-dominated visual
    bool isLavaModel        = false; ///< Lava surface (UV scroll animation)
    bool isInstancePortal   = false; ///< Instance portal (additive, spin, no collision)
    bool isWaterVegetation  = false; ///< Aquatic vegetation (cattails, kelp, reeds, etc.)
    bool isFireflyEffect    = false; ///< Ambient creature (exempt from particle dampeners)
    bool isElvenLike        = false; ///< Night elf / Blood elf themed model
    bool isLanternLike      = false; ///< Lantern/lamp/light model
    bool isKoboldFlame      = false; ///< Kobold candle/torch model
    bool isGroundDetail     = false; ///< Ground-clutter detail doodad (always non-blocking)
    bool isInvisibleTrap    = false; ///< Event-object invisible trap (no render, no collision)
    bool isSmoke            = false; ///< Smoke model (UV scroll animation)

    // --- Animation flags ---
    bool disableAnimation   = false; ///< Keep visually stable (foliage, chest lids, etc.)
    bool shadowWindFoliage  = false; ///< Apply wind sway in shadow pass for foliage/trees
};

/**
 * Classify an M2 model by name and geometry.
 *
 * Pure function — no Vulkan, VkContext, or AssetManager dependencies.
 * All results are derived solely from the model name string and tight vertex bounds.
 *
 * @param name         Full model path/name from the M2 header (any case)
 * @param boundsMin    Per-vertex tight bounding-box minimum
 * @param boundsMax    Per-vertex tight bounding-box maximum
 * @param vertexCount  Number of mesh vertices
 * @param emitterCount Number of particle emitters
 */
M2ClassificationResult classifyM2Model(
    const std::string& name,
    const glm::vec3&   boundsMin,
    const glm::vec3&   boundsMax,
    std::size_t        vertexCount,
    std::size_t        emitterCount);

// ---------------------------------------------------------------------------
// Batch texture classification
// ---------------------------------------------------------------------------

/**
 * Per-batch texture key classification — glow / tint token flags.
 * Input must be a lowercased, backslash-normalised texture path (as stored in
 * M2Renderer's textureKeysLower vector).  Pure data — no Vulkan dependencies.
 */
struct M2BatchTexClassification {
    bool exactLanternGlowTex = false; ///< One of the known exact lantern-glow texture paths
    bool hasGlowToken        = false; ///< glow / flare / halo / light
    bool hasFlameToken       = false; ///< flame / fire / flamelick / ember
    bool hasGlowCardToken    = false; ///< glow / flamelick / lensflare / t_vfx / lightbeam / glowball / genericglow
    bool likelyFlame         = false; ///< fire / flame / torch
    bool lanternFamily       = false; ///< lantern / lamp / elf / silvermoon / quel / thalas
    int  glowTint            = 0;     ///< 0 = neutral, 1 = cool (blue/arcane), 2 = warm (red/scarlet)
};

/**
 * Classify a batch texture by its lowercased path for glow/tint hinting.
 *
 * Pure function — no Vulkan, VkContext, or AssetManager dependencies.
 *
 * @param lowerTexKey Lowercased, backslash-normalised texture path (may be empty)
 */
M2BatchTexClassification classifyBatchTexture(const std::string& lowerTexKey);

} // namespace rendering
} // namespace wowee
