#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace audio { enum class FootstepSurface : uint8_t; }
namespace rendering {

class Renderer;

// ============================================================================
// AnimationController — extracted from Renderer (§4.2)
//
// Owns the character locomotion state machine, mount animation state,
// emote system, footstep triggering, surface detection, melee combat
// animation, and activity SFX transition tracking.
// ============================================================================
class AnimationController {
public:
    AnimationController();
    ~AnimationController();

    void initialize(Renderer* renderer);

    // ── Per-frame update hooks (called from Renderer::update) ──────────────
    // Runs the character animation state machine (mounted + unmounted).
    void updateCharacterAnimation();
    // Processes animation-driven footstep events (player + mount).
    void updateFootsteps(float deltaTime);
    // Tracks state transitions for activity SFX (jump, landing, swim) and
    // mount ambient sounds.
    void updateSfxState(float deltaTime);
    // Decrements melee swing timer / cooldown.
    void updateMeleeTimers(float deltaTime);
    // Store per-frame delta time (used inside animation state machine).
    void setDeltaTime(float dt) { lastDeltaTime_ = dt; }

    // ── Character follow ───────────────────────────────────────────────────
    void onCharacterFollow(uint32_t instanceId);

    // ── Emote support ──────────────────────────────────────────────────────
    void playEmote(const std::string& emoteName);
    void cancelEmote();
    bool isEmoteActive() const { return emoteActive_; }
    static std::string getEmoteText(const std::string& emoteName,
                                    const std::string* targetName = nullptr);
    static uint32_t getEmoteDbcId(const std::string& emoteName);
    static std::string getEmoteTextByDbcId(uint32_t dbcId,
                                           const std::string& senderName,
                                           const std::string* targetName = nullptr);
    static uint32_t getEmoteAnimByDbcId(uint32_t dbcId);

    // ── Targeting / combat ─────────────────────────────────────────────────
    void setTargetPosition(const glm::vec3* pos);
    void setInCombat(bool combat) { inCombat_ = combat; }
    bool isInCombat() const { return inCombat_; }
    const glm::vec3* getTargetPosition() const { return targetPosition_; }
    void resetCombatVisualState();
    bool isMoving() const;

    // ── Melee combat ───────────────────────────────────────────────────────
    void triggerMeleeSwing();
    void setEquippedWeaponType(uint32_t inventoryType) { equippedWeaponInvType_ = inventoryType; meleeAnimId_ = 0; }
    void setCharging(bool charging) { charging_ = charging; }
    bool isCharging() const { return charging_; }

    // ── Effect triggers ────────────────────────────────────────────────────
    void triggerLevelUpEffect(const glm::vec3& position);
    void startChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void emitChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void stopChargeEffect();

    // ── Mount ──────────────────────────────────────────────────────────────
    void setMounted(uint32_t mountInstId, uint32_t mountDisplayId,
                    float heightOffset, const std::string& modelPath = "");
    void setTaxiFlight(bool onTaxi) { taxiFlight_ = onTaxi; }
    void setMountPitchRoll(float pitch, float roll) { mountPitch_ = pitch; mountRoll_ = roll; }
    void clearMount();
    bool isMounted() const { return mountInstanceId_ != 0; }
    uint32_t getMountInstanceId() const { return mountInstanceId_; }

    // ── Query helpers (used by Renderer) ───────────────────────────────────
    bool isFootstepAnimationState() const;
    float getMeleeSwingTimer() const { return meleeSwingTimer_; }
    float getMountHeightOffset() const { return mountHeightOffset_; }
    bool isTaxiFlight() const { return taxiFlight_; }

private:
    Renderer* renderer_ = nullptr;

    // Character animation state machine
    enum class CharAnimState {
        IDLE, WALK, RUN, JUMP_START, JUMP_MID, JUMP_END, SIT_DOWN, SITTING,
        EMOTE, SWIM_IDLE, SWIM, MELEE_SWING, MOUNT, CHARGE, COMBAT_IDLE
    };
    CharAnimState charAnimState_ = CharAnimState::IDLE;
    float locomotionStopGraceTimer_ = 0.0f;
    bool locomotionWasSprinting_ = false;
    uint32_t lastPlayerAnimRequest_ = UINT32_MAX;
    bool lastPlayerAnimLoopRequest_ = true;

    // Emote state
    bool emoteActive_ = false;
    uint32_t emoteAnimId_ = 0;
    bool emoteLoop_ = false;

    // Target facing
    const glm::vec3* targetPosition_ = nullptr;
    bool inCombat_ = false;

    // Footstep event tracking (animation-driven)
    uint32_t footstepLastAnimationId_ = 0;
    float footstepLastNormTime_ = 0.0f;
    bool footstepNormInitialized_ = false;

    // Footstep surface cache (avoid expensive queries every step)
    mutable audio::FootstepSurface cachedFootstepSurface_{};
    mutable glm::vec3 cachedFootstepPosition_{0.0f, 0.0f, 0.0f};
    mutable float cachedFootstepUpdateTimer_{999.0f};

    // Mount footstep tracking (separate from player's)
    uint32_t mountFootstepLastAnimId_ = 0;
    float mountFootstepLastNormTime_ = 0.0f;
    bool mountFootstepNormInitialized_ = false;

    // SFX transition state
    bool sfxStateInitialized_ = false;
    bool sfxPrevGrounded_ = true;
    bool sfxPrevJumping_ = false;
    bool sfxPrevFalling_ = false;
    bool sfxPrevSwimming_ = false;

    // Melee combat
    bool charging_ = false;
    float meleeSwingTimer_ = 0.0f;
    float meleeSwingCooldown_ = 0.0f;
    float meleeAnimDurationMs_ = 0.0f;
    uint32_t meleeAnimId_ = 0;
    uint32_t equippedWeaponInvType_ = 0;

    // Mount animation capabilities (discovered at mount time, varies per model)
    struct MountAnimSet {
        uint32_t jumpStart = 0;  // Jump start animation
        uint32_t jumpLoop = 0;   // Jump airborne loop
        uint32_t jumpEnd = 0;    // Jump landing
        uint32_t rearUp = 0;     // Rear-up / special flourish
        uint32_t run = 0;        // Run animation (discovered, don't assume)
        uint32_t stand = 0;      // Stand animation (discovered)
        std::vector<uint32_t> fidgets;  // Idle fidget animations (head turn, tail swish, etc.)
    };

    enum class MountAction { None, Jump, RearUp };

    uint32_t mountInstanceId_ = 0;
    float mountHeightOffset_ = 0.0f;
    float mountPitch_ = 0.0f;  // Up/down tilt (radians)
    float mountRoll_ = 0.0f;   // Left/right banking (radians)
    int mountSeatAttachmentId_ = -1;  // -1 unknown, -2 unavailable
    glm::vec3 smoothedMountSeatPos_ = glm::vec3(0.0f);
    bool mountSeatSmoothingInit_ = false;
    float prevMountYaw_ = 0.0f; // Previous yaw for turn rate calculation (procedural lean)
    float lastDeltaTime_ = 0.0f; // Cached for use in updateCharacterAnimation()
    MountAction mountAction_ = MountAction::None;  // Current mount action (jump/rear-up)
    uint32_t mountActionPhase_ = 0;  // 0=start, 1=loop, 2=end (for jump chaining)
    MountAnimSet mountAnims_;  // Cached animation IDs for current mount
    float mountIdleFidgetTimer_ = 0.0f;  // Timer for random idle fidgets
    float mountIdleSoundTimer_ = 0.0f;   // Timer for ambient idle sounds
    uint32_t mountActiveFidget_ = 0;     // Currently playing fidget animation ID (0 = none)
    bool taxiFlight_ = false;
    bool taxiAnimsLogged_ = false;

    // Private animation helpers
    bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs);
    audio::FootstepSurface resolveFootstepSurface() const;
    uint32_t resolveMeleeAnimId();
};

} // namespace rendering
} // namespace wowee
