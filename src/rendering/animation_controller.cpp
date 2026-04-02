#include "rendering/animation_controller.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/swim_effects.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <random>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// ── Static emote data (shared across all AnimationController instances) ──────

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    std::string command;
};

static std::unordered_map<std::string, EmoteInfo> EMOTE_TABLE;
static std::unordered_map<uint32_t, const EmoteInfo*> EMOTE_BY_DBCID;
static bool emoteTableLoaded = false;

static std::vector<std::string> parseEmoteCommands(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool isLoopingEmote(const std::string& command) {
    static const std::unordered_set<std::string> kLooping = {
        "dance",
        "train",
    };
    return kLooping.find(command) != kLooping.end();
}

static void loadFallbackEmotes() {
    if (!EMOTE_TABLE.empty()) return;
    EMOTE_TABLE = {
        {"wave",    {67,  0, false, "You wave.", "You wave at %s.", "%s waves.", "%s waves at %s.", "wave"}},
        {"bow",     {66,  0, false, "You bow down graciously.", "You bow down before %s.", "%s bows down graciously.", "%s bows down before %s.", "bow"}},
        {"laugh",   {70,  0, false, "You laugh.", "You laugh at %s.", "%s laughs.", "%s laughs at %s.", "laugh"}},
        {"point",   {84,  0, false, "You point over yonder.", "You point at %s.", "%s points over yonder.", "%s points at %s.", "point"}},
        {"cheer",   {68,  0, false, "You cheer!", "You cheer at %s.", "%s cheers!", "%s cheers at %s.", "cheer"}},
        {"dance",   {69,  0, true,  "You burst into dance.", "You dance with %s.", "%s bursts into dance.", "%s dances with %s.", "dance"}},
        {"kneel",   {75,  0, false, "You kneel down.", "You kneel before %s.", "%s kneels down.", "%s kneels before %s.", "kneel"}},
        {"applaud", {80,  0, false, "You applaud. Bravo!", "You applaud at %s. Bravo!", "%s applauds. Bravo!", "%s applauds at %s. Bravo!", "applaud"}},
        {"shout",   {81,  0, false, "You shout.", "You shout at %s.", "%s shouts.", "%s shouts at %s.", "shout"}},
        {"chicken", {78,  0, false, "With arms flapping, you strut around. Cluck, Cluck, Chicken!",
                     "With arms flapping, you strut around %s. Cluck, Cluck, Chicken!",
                     "%s struts around. Cluck, Cluck, Chicken!", "%s struts around %s. Cluck, Cluck, Chicken!", "chicken"}},
        {"cry",     {77,  0, false, "You cry.", "You cry on %s's shoulder.", "%s cries.", "%s cries on %s's shoulder.", "cry"}},
        {"kiss",    {76,  0, false, "You blow a kiss into the wind.", "You blow a kiss to %s.", "%s blows a kiss into the wind.", "%s blows a kiss to %s.", "kiss"}},
        {"roar",    {74,  0, false, "You roar with bestial vigor. So fierce!", "You roar with bestial vigor at %s. So fierce!", "%s roars with bestial vigor. So fierce!", "%s roars with bestial vigor at %s. So fierce!", "roar"}},
        {"salute",  {113, 0, false, "You salute.", "You salute %s with respect.", "%s salutes.", "%s salutes %s with respect.", "salute"}},
        {"rude",    {73,  0, false, "You make a rude gesture.", "You make a rude gesture at %s.", "%s makes a rude gesture.", "%s makes a rude gesture at %s.", "rude"}},
        {"flex",    {82,  0, false, "You flex your muscles. Oooooh so strong!", "You flex at %s. Oooooh so strong!", "%s flexes. Oooooh so strong!", "%s flexes at %s. Oooooh so strong!", "flex"}},
        {"shy",     {83,  0, false, "You smile shyly.", "You smile shyly at %s.", "%s smiles shyly.", "%s smiles shyly at %s.", "shy"}},
        {"beg",     {79,  0, false, "You beg everyone around you. How pathetic.", "You beg %s. How pathetic.", "%s begs everyone around. How pathetic.", "%s begs %s. How pathetic.", "beg"}},
        {"eat",     {61,  0, false, "You begin to eat.", "You begin to eat in front of %s.", "%s begins to eat.", "%s begins to eat in front of %s.", "eat"}},
    };
}

static std::string replacePlaceholders(const std::string& text, const std::string* targetName) {
    if (text.empty()) return text;
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
            if (targetName && !targetName->empty()) out += *targetName;
            i++;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

static void loadEmotesFromDbc() {
    if (emoteTableLoaded) return;
    emoteTableLoaded = true;

    auto* assetManager = core::Application::getInstance().getAssetManager();
    if (!assetManager) {
        LOG_WARNING("Emotes: no AssetManager");
        loadFallbackEmotes();
        return;
    }

    auto emotesTextDbc = assetManager->loadDBC("EmotesText.dbc");
    auto emotesTextDataDbc = assetManager->loadDBC("EmotesTextData.dbc");
    if (!emotesTextDbc || !emotesTextDataDbc || !emotesTextDbc->isLoaded() || !emotesTextDataDbc->isLoaded()) {
        LOG_WARNING("Emotes: DBCs not available (EmotesText/EmotesTextData)");
        loadFallbackEmotes();
        return;
    }

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* etdL = activeLayout ? activeLayout->getLayout("EmotesTextData") : nullptr;
    const auto* emL  = activeLayout ? activeLayout->getLayout("Emotes") : nullptr;
    const auto* etL  = activeLayout ? activeLayout->getLayout("EmotesText") : nullptr;

    std::unordered_map<uint32_t, std::string> textData;
    textData.reserve(emotesTextDataDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDataDbc->getRecordCount(); ++r) {
        uint32_t id = emotesTextDataDbc->getUInt32(r, etdL ? (*etdL)["ID"] : 0);
        std::string text = emotesTextDataDbc->getString(r, etdL ? (*etdL)["Text"] : 1);
        if (!text.empty()) textData.emplace(id, std::move(text));
    }

    std::unordered_map<uint32_t, uint32_t> emoteIdToAnim;
    if (auto emotesDbc = assetManager->loadDBC("Emotes.dbc"); emotesDbc && emotesDbc->isLoaded()) {
        emoteIdToAnim.reserve(emotesDbc->getRecordCount());
        for (uint32_t r = 0; r < emotesDbc->getRecordCount(); ++r) {
            uint32_t emoteId = emotesDbc->getUInt32(r, emL ? (*emL)["ID"] : 0);
            uint32_t animId = emotesDbc->getUInt32(r, emL ? (*emL)["AnimID"] : 2);
            if (animId != 0) emoteIdToAnim[emoteId] = animId;
        }
    }

    EMOTE_TABLE.clear();
    EMOTE_TABLE.reserve(emotesTextDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDbc->getRecordCount(); ++r) {
        uint32_t recordId = emotesTextDbc->getUInt32(r, etL ? (*etL)["ID"] : 0);
        std::string cmdRaw = emotesTextDbc->getString(r, etL ? (*etL)["Command"] : 1);
        if (cmdRaw.empty()) continue;

        uint32_t emoteRef = emotesTextDbc->getUInt32(r, etL ? (*etL)["EmoteRef"] : 2);
        uint32_t animId = 0;
        auto animIt = emoteIdToAnim.find(emoteRef);
        if (animIt != emoteIdToAnim.end()) {
            animId = animIt->second;
        } else {
            animId = emoteRef;
        }

        uint32_t senderTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderTargetTextID"] : 5);
        uint32_t senderNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderNoTargetTextID"] : 9);
        uint32_t othersTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersTargetTextID"] : 3);
        uint32_t othersNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersNoTargetTextID"] : 7);

        std::string textTarget, textNoTarget, oTarget, oNoTarget;
        if (auto it = textData.find(senderTargetTextId); it != textData.end()) textTarget = it->second;
        if (auto it = textData.find(senderNoTargetTextId); it != textData.end()) textNoTarget = it->second;
        if (auto it = textData.find(othersTargetTextId); it != textData.end()) oTarget = it->second;
        if (auto it = textData.find(othersNoTargetTextId); it != textData.end()) oNoTarget = it->second;

        for (const std::string& cmd : parseEmoteCommands(cmdRaw)) {
            if (cmd.empty()) continue;
            EmoteInfo info;
            info.animId = animId;
            info.dbcId = recordId;
            info.loop = isLoopingEmote(cmd);
            info.textNoTarget = textNoTarget;
            info.textTarget = textTarget;
            info.othersNoTarget = oNoTarget;
            info.othersTarget = oTarget;
            info.command = cmd;
            EMOTE_TABLE.emplace(cmd, std::move(info));
        }
    }

    if (EMOTE_TABLE.empty()) {
        LOG_WARNING("Emotes: DBC loaded but no commands parsed, using fallback list");
        loadFallbackEmotes();
    } else {
        LOG_INFO("Emotes: loaded ", EMOTE_TABLE.size(), " commands from DBC");
    }

    EMOTE_BY_DBCID.clear();
    for (auto& [cmd, info] : EMOTE_TABLE) {
        if (info.dbcId != 0) {
            EMOTE_BY_DBCID.emplace(info.dbcId, &info);
        }
    }
}

// ── AnimationController implementation ───────────────────────────────────────

AnimationController::AnimationController() = default;
AnimationController::~AnimationController() = default;

void AnimationController::initialize(Renderer* renderer) {
    renderer_ = renderer;
}

void AnimationController::onCharacterFollow(uint32_t /*instanceId*/) {
    // Reset animation state when follow target changes
}

// ── Emote support ────────────────────────────────────────────────────────────

void AnimationController::playEmote(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it == EMOTE_TABLE.end()) return;

    const auto& info = it->second;
    if (info.animId == 0) return;
    emoteActive_ = true;
    emoteAnimId_ = info.animId;
    emoteLoop_ = info.loop;
    charAnimState_ = CharAnimState::EMOTE;

    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (characterRenderer && characterInstanceId > 0) {
        characterRenderer->playAnimation(characterInstanceId, emoteAnimId_, emoteLoop_);
    }
}

void AnimationController::cancelEmote() {
    emoteActive_ = false;
    emoteAnimId_ = 0;
    emoteLoop_ = false;
}

std::string AnimationController::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        const auto& info = it->second;
        const std::string& base = (targetName ? info.textTarget : info.textNoTarget);
        if (!base.empty()) {
            return replacePlaceholders(base, targetName);
        }
        if (targetName && !targetName->empty()) {
            return "You " + info.command + " at " + *targetName + ".";
        }
        return "You " + info.command + ".";
    }
    return "";
}

uint32_t AnimationController::getEmoteDbcId(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        return it->second.dbcId;
    }
    return 0;
}

std::string AnimationController::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                                      const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it == EMOTE_BY_DBCID.end()) return "";

    const EmoteInfo& info = *it->second;

    if (targetName && !targetName->empty()) {
        if (!info.othersTarget.empty()) {
            std::string out;
            out.reserve(info.othersTarget.size() + senderName.size() + targetName->size());
            bool firstReplaced = false;
            for (size_t i = 0; i < info.othersTarget.size(); ++i) {
                if (info.othersTarget[i] == '%' && i + 1 < info.othersTarget.size() && info.othersTarget[i + 1] == 's') {
                    out += firstReplaced ? *targetName : senderName;
                    firstReplaced = true;
                    ++i;
                } else {
                    out.push_back(info.othersTarget[i]);
                }
            }
            return out;
        }
        return senderName + " " + info.command + "s at " + *targetName + ".";
    } else {
        if (!info.othersNoTarget.empty()) {
            return replacePlaceholders(info.othersNoTarget, &senderName);
        }
        return senderName + " " + info.command + "s.";
    }
}

uint32_t AnimationController::getEmoteAnimByDbcId(uint32_t dbcId) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it != EMOTE_BY_DBCID.end()) {
        return it->second->animId;
    }
    return 0;
}

// ── Targeting / combat ───────────────────────────────────────────────────────

void AnimationController::setTargetPosition(const glm::vec3* pos) {
    targetPosition_ = pos;
}

void AnimationController::resetCombatVisualState() {
    inCombat_ = false;
    targetPosition_ = nullptr;
    meleeSwingTimer_ = 0.0f;
    meleeSwingCooldown_ = 0.0f;
    if (auto* svs = renderer_->getSpellVisualSystem()) svs->reset();
}

bool AnimationController::isMoving() const {
    auto* cameraController = renderer_->getCameraController();
    return cameraController && cameraController->isMoving();
}

// ── Melee combat ─────────────────────────────────────────────────────────────

void AnimationController::triggerMeleeSwing() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown_ > 0.0f) return;
    if (emoteActive_) {
        cancelEmote();
    }
    resolveMeleeAnimId();
    meleeSwingCooldown_ = 0.1f;
    float durationSec = meleeAnimDurationMs_ > 0.0f ? meleeAnimDurationMs_ / 1000.0f : 0.6f;
    if (durationSec < 0.25f) durationSec = 0.25f;
    if (durationSec > 1.0f) durationSec = 1.0f;
    meleeSwingTimer_ = durationSec;
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

uint32_t AnimationController::resolveMeleeAnimId() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    if (meleeAnimId_ != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId_)) {
        return meleeAnimId_;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    auto findDuration = [&](uint32_t id) -> float {
        for (const auto& seq : sequences) {
            if (seq.id == id && seq.duration > 0) {
                return static_cast<float>(seq.duration);
            }
        }
        return 0.0f;
    };

    const uint32_t* attackCandidates;
    size_t candidateCount;
    static const uint32_t candidates2H[] = {18, 17, 16, 19, 20, 21};
    static const uint32_t candidates1H[] = {17, 18, 16, 19, 20, 21};
    static const uint32_t candidatesUnarmed[] = {16, 17, 18, 19, 20, 21};
    if (equippedWeaponInvType_ == 17) {
        attackCandidates = candidates2H;
        candidateCount = 6;
    } else if (equippedWeaponInvType_ == 0) {
        attackCandidates = candidatesUnarmed;
        candidateCount = 6;
    } else {
        attackCandidates = candidates1H;
        candidateCount = 6;
    }
    for (size_t ci = 0; ci < candidateCount; ci++) {
        uint32_t id = attackCandidates[ci];
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId_ = id;
            meleeAnimDurationMs_ = findDuration(id);
            return meleeAnimId_;
        }
    }

    const uint32_t avoidIds[] = {0, 1, 4, 5, 11, 12, 13, 37, 38, 39, 41, 42, 97};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId_ = bestId;
    meleeAnimDurationMs_ = static_cast<float>(bestDuration);
    return meleeAnimId_;
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position) {
    auto* levelUpEffect = renderer_->getLevelUpEffect();
    if (!levelUpEffect) return;

    if (!levelUpEffect->isModelLoaded()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (!assetManager) {
                LOG_WARNING("LevelUpEffect: no asset manager available");
            } else {
                auto m2Data = assetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
                auto skinData = assetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
                LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
                if (!m2Data.empty()) {
                    levelUpEffect->loadModel(m2Renderer, m2Data, skinData);
                } else {
                    LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
                }
            }
        }
    }

    levelUpEffect->trigger(position);
}

void AnimationController::startChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    auto* chargeEffect = renderer_->getChargeEffect();
    if (!chargeEffect) return;

    if (!chargeEffect->isActive()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (assetManager) {
                chargeEffect->tryLoadM2Models(m2Renderer, assetManager);
            }
        }
    }

    chargeEffect->start(position, direction);
}

void AnimationController::emitChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (auto* chargeEffect = renderer_->getChargeEffect()) {
        chargeEffect->emit(position, direction);
    }
}

void AnimationController::stopChargeEffect() {
    if (auto* chargeEffect = renderer_->getChargeEffect()) {
        chargeEffect->stop();
    }
}

// ── Mount ────────────────────────────────────────────────────────────────────

void AnimationController::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();

    mountInstanceId_ = mountInstId;
    mountHeightOffset_ = heightOffset;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = renderer_->getCharacterPosition();
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;
    mountActionPhase_ = 0;
    charAnimState_ = CharAnimState::MOUNT;
    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    if (characterRenderer && mountInstId > 0) {
        characterRenderer->dumpAnimations(mountInstId);
    }

    // Discover mount animation capabilities (property-based, not hardcoded IDs)
    LOG_DEBUG("=== Mount Animation Dump (Display ID ", mountDisplayId, ") ===");
    if (characterRenderer) characterRenderer->dumpAnimations(mountInstId);

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer || !characterRenderer->getAnimationSequences(mountInstId, sequences)) {
        LOG_WARNING("Failed to get animation sequences for mount, using fallback IDs");
        sequences.clear();
    }

    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer && characterRenderer->hasAnimation(mountInstId, id)) {
                return id;
            }
        }
        return 0;
    };

    // Property-based jump animation discovery with chain-based scoring
    auto discoverJumpSet = [&]() {
        LOG_DEBUG("=== Full sequence table for mount ===");
        for (const auto& seq : sequences) {
            LOG_DEBUG("SEQ id=", seq.id,
                     " dur=", seq.duration,
                     " flags=0x", std::hex, seq.flags, std::dec,
                     " moveSpd=", seq.movingSpeed,
                     " blend=", seq.blendTime,
                     " next=", seq.nextAnimation,
                     " alias=", seq.aliasNext);
        }
        LOG_DEBUG("=== End sequence table ===");

        std::set<uint32_t> forbiddenIds = {53, 54, 16};

        auto scoreNear = [](int a, int b) -> int {
            int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0;
        };

        auto isForbidden = [&](uint32_t id) {
            return forbiddenIds.count(id) != 0;
        };

        auto findSeqById = [&](uint32_t id) -> const pipeline::M2Sequence* {
            for (const auto& s : sequences) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        uint32_t runId = findFirst({5, 4});
        uint32_t standId = findFirst({0});

        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.duration >= 350 && seq.duration <= 1000 &&
                seq.id != runId && seq.id != standId) {
                loops.push_back(seq.id);
            }
        }

        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear(static_cast<int>(id), 38);
                const auto* s = findSeqById(id);
                if (s) sc += (s->duration >= 500 && s->duration <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }

        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;

        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;

            if (seq.duration >= 450 && seq.duration <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (loop && (seq.nextAnimation == static_cast<int16_t>(loop) || seq.aliasNext == loop)) sc += 30;
                if (loop && scoreNear(seq.nextAnimation, static_cast<int>(loop)) > 0) sc += 10;
                if (seq.blendTime > 400) sc -= 5;

                if (sc > bestStart) {
                    bestStart = sc;
                    start = seq.id;
                }
            }

            if (seq.duration >= 650 && seq.duration <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (seq.nextAnimation == static_cast<int16_t>(runId) || seq.nextAnimation == static_cast<int16_t>(standId)) sc += 10;
                if (seq.nextAnimation < 0) sc += 5;
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }

        LOG_DEBUG("Property-based jump discovery: start=", start, " loop=", loop, " end=", end,
                 " scores: start=", bestStart, " end=", bestEnd);
        return std::make_tuple(start, loop, end);
    };

    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    mountAnims_.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({40, 37});
    mountAnims_.jumpLoop  = discoveredLoop > 0 ? discoveredLoop : findFirst({38});
    mountAnims_.jumpEnd   = discoveredEnd > 0 ? discoveredEnd : findFirst({39});
    mountAnims_.rearUp    = findFirst({94, 92, 40});
    mountAnims_.run       = findFirst({5, 4});
    mountAnims_.stand     = findFirst({0});

    // Discover idle fidget animations using proper WoW M2 metadata
    mountAnims_.fidgets.clear();
    core::Logger::getInstance().debug("Scanning for fidget animations in ", sequences.size(), " sequences");

    core::Logger::getInstance().debug("=== ALL potential fidgets (no metadata filter) ===");
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary) {
            core::Logger::getInstance().debug("  ALL: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " flags=0x", std::hex, seq.flags, std::dec,
                " next=", seq.nextAnimation);
        }
    }

    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool hasFrequency = seq.frequency > 0;
        bool hasReplay = seq.replayMax > 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary && (hasFrequency || hasReplay)) {
            core::Logger::getInstance().debug("  Candidate: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " next=", seq.nextAnimation,
                " speed=", seq.movingSpeed);
        }

        bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        bool isSpecial = (seq.id == 2 || seq.id == 3);

        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration &&
            !isDeathOrWound && !isAttackOrCombat && !isSpecial) {
            bool chainsToStand = (seq.nextAnimation == static_cast<int16_t>(mountAnims_.stand)) ||
                                 (seq.aliasNext == mountAnims_.stand) ||
                                 (seq.nextAnimation == -1);

            mountAnims_.fidgets.push_back(seq.id);
            core::Logger::getInstance().debug("  >> Selected fidget: id=", seq.id,
                (chainsToStand ? " (chains to stand)" : ""));
        }
    }

    if (mountAnims_.run == 0) mountAnims_.run = mountAnims_.stand;

    core::Logger::getInstance().debug("Mount animation set: jumpStart=", mountAnims_.jumpStart,
        " jumpLoop=", mountAnims_.jumpLoop,
        " jumpEnd=", mountAnims_.jumpEnd,
        " rearUp=", mountAnims_.rearUp,
        " run=", mountAnims_.run,
        " stand=", mountAnims_.stand,
        " fidgets=", mountAnims_.fidgets.size());

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        bool isFlying = taxiFlight_;
        renderer_->getAudioCoordinator()->getMountSoundManager()->onMount(mountDisplayId, isFlying, modelPath);
    }
}

void AnimationController::clearMount() {
    mountInstanceId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = glm::vec3(0.0f);
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;
    mountActionPhase_ = 0;
    charAnimState_ = CharAnimState::IDLE;
    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->onDismount();
    }
}

// ── Query helpers ────────────────────────────────────────────────────────────

bool AnimationController::isFootstepAnimationState() const {
    return charAnimState_ == CharAnimState::WALK || charAnimState_ == CharAnimState::RUN;
}

// ── Melee timers ─────────────────────────────────────────────────────────────

void AnimationController::updateMeleeTimers(float deltaTime) {
    if (meleeSwingCooldown_ > 0.0f) {
        meleeSwingCooldown_ = std::max(0.0f, meleeSwingCooldown_ - deltaTime);
    }
    if (meleeSwingTimer_ > 0.0f) {
        meleeSwingTimer_ = std::max(0.0f, meleeSwingTimer_ - deltaTime);
    }
}

// ── Character animation state machine ────────────────────────────────────────

void AnimationController::updateCharacterAnimation() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();

    // WoW WotLK AnimationData.dbc IDs
    constexpr uint32_t ANIM_STAND      = 0;
    constexpr uint32_t ANIM_WALK       = 4;
    constexpr uint32_t ANIM_RUN        = 5;
    constexpr uint32_t ANIM_STRAFE_RUN_RIGHT  = 92;
    constexpr uint32_t ANIM_STRAFE_RUN_LEFT   = 93;
    constexpr uint32_t ANIM_STRAFE_WALK_LEFT  = 11;
    constexpr uint32_t ANIM_STRAFE_WALK_RIGHT = 12;
    constexpr uint32_t ANIM_BACKPEDAL         = 13;
    constexpr uint32_t ANIM_JUMP_START = 37;
    constexpr uint32_t ANIM_JUMP_MID   = 38;
    constexpr uint32_t ANIM_JUMP_END   = 39;
    constexpr uint32_t ANIM_SIT_DOWN   = 97;
    constexpr uint32_t ANIM_SITTING    = 97;
    constexpr uint32_t ANIM_SWIM_IDLE  = 41;
    constexpr uint32_t ANIM_SWIM       = 42;
    constexpr uint32_t ANIM_MOUNT      = 91;
    constexpr uint32_t ANIM_READY_UNARMED = 22;
    constexpr uint32_t ANIM_READY_1H      = 23;
    constexpr uint32_t ANIM_READY_2H      = 24;
    constexpr uint32_t ANIM_READY_2H_L    = 25;
    constexpr uint32_t ANIM_FLY_IDLE   = 158;
    constexpr uint32_t ANIM_FLY_FORWARD = 159;

    CharAnimState newState = charAnimState_;

    const bool rawMoving = cameraController->isMoving();
    const bool rawSprinting = cameraController->isSprinting();
    constexpr float kLocomotionStopGraceSec = 0.12f;
    if (rawMoving) {
        locomotionStopGraceTimer_ = kLocomotionStopGraceSec;
        locomotionWasSprinting_ = rawSprinting;
    } else {
        locomotionStopGraceTimer_ = std::max(0.0f, locomotionStopGraceTimer_ - lastDeltaTime_);
    }
    bool moving = rawMoving || locomotionStopGraceTimer_ > 0.0f;
    bool movingForward = cameraController->isMovingForward();
    bool movingBackward = cameraController->isMovingBackward();
    bool autoRunning = cameraController->isAutoRunning();
    bool strafeLeft = cameraController->isStrafingLeft();
    bool strafeRight = cameraController->isStrafingRight();
    bool pureStrafe = !movingForward && !movingBackward && !autoRunning;
    bool anyStrafeLeft = strafeLeft && !strafeRight && pureStrafe;
    bool anyStrafeRight = strafeRight && !strafeLeft && pureStrafe;
    bool grounded = cameraController->isGrounded();
    bool jumping = cameraController->isJumping();
    bool sprinting = rawSprinting || (!rawMoving && moving && locomotionWasSprinting_);
    bool sitting = cameraController->isSitting();
    bool swim = cameraController->isSwimming();
    bool forceMelee = meleeSwingTimer_ > 0.0f && grounded && !swim;

    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
    float characterYaw = renderer_->getCharacterYaw();

    // When mounted, force MOUNT state and skip normal transitions
    if (isMounted()) {
        newState = CharAnimState::MOUNT;
        charAnimState_ = newState;

        uint32_t currentAnimId = 0;
        float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
        bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
        if (!haveState || currentAnimId != ANIM_MOUNT) {
            characterRenderer->playAnimation(characterInstanceId, ANIM_MOUNT, true);
        }

        float mountBob = 0.0f;
        float mountYawRad = glm::radians(characterYaw);
        if (mountInstanceId_ > 0) {
            characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);

            if (!taxiFlight_ && moving && lastDeltaTime_ > 0.0f) {
                float currentYawDeg = characterYaw;
                float turnRate = (currentYawDeg - prevMountYaw_) / lastDeltaTime_;
                while (turnRate > 180.0f) turnRate -= 360.0f;
                while (turnRate < -180.0f) turnRate += 360.0f;

                float targetLean = glm::clamp(turnRate * 0.15f, -0.25f, 0.25f);
                mountRoll_ = glm::mix(mountRoll_, targetLean, lastDeltaTime_ * 6.0f);
                prevMountYaw_ = currentYawDeg;
            } else {
                mountRoll_ = glm::mix(mountRoll_, 0.0f, lastDeltaTime_ * 8.0f);
            }

            characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll_, mountYawRad));

            auto pickMountAnim = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
                for (uint32_t id : candidates) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, id)) {
                        return id;
                    }
                }
                return fallback;
            };

            uint32_t mountAnimId = ANIM_STAND;

            uint32_t curMountAnim = 0;
            float curMountTime = 0, curMountDur = 0;
            bool haveMountState = characterRenderer->getAnimationState(mountInstanceId_, curMountAnim, curMountTime, curMountDur);

            if (taxiFlight_) {
                if (!taxiAnimsLogged_) {
                    taxiAnimsLogged_ = true;
                    LOG_INFO("Taxi flight active: mountInstanceId_=", mountInstanceId_,
                             " curMountAnim=", curMountAnim, " haveMountState=", haveMountState);
                    std::vector<pipeline::M2Sequence> seqs;
                    if (characterRenderer->getAnimationSequences(mountInstanceId_, seqs)) {
                        std::string animList;
                        for (const auto& s : seqs) {
                            if (!animList.empty()) animList += ", ";
                            animList += std::to_string(s.id);
                        }
                        LOG_INFO("Taxi mount available animations: [", animList, "]");
                    }
                }

                uint32_t flyAnims[] = {ANIM_FLY_FORWARD, ANIM_FLY_IDLE, 234, 229, 233, 141, 369, 6, ANIM_RUN};
                mountAnimId = ANIM_STAND;
                for (uint32_t fa : flyAnims) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, fa)) {
                        mountAnimId = fa;
                        break;
                    }
                }

                if (!haveMountState || curMountAnim != mountAnimId) {
                    LOG_INFO("Taxi mount: playing animation ", mountAnimId);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                }

                goto taxi_mount_done;
            } else {
                taxiAnimsLogged_ = false;
            }

            // Check for jump trigger
            if (cameraController->isJumpKeyPressed() && grounded && mountAction_ == MountAction::None) {
                if (moving && mountAnims_.jumpLoop > 0) {
                    LOG_DEBUG("Mount jump triggered while moving: using jumpLoop anim ", mountAnims_.jumpLoop);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                    mountAction_ = MountAction::Jump;
                    mountActionPhase_ = 1;
                    mountAnimId = mountAnims_.jumpLoop;
                    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                        renderer_->getAudioCoordinator()->getMountSoundManager()->playJumpSound();
                    }
                    if (cameraController) {
                        cameraController->triggerMountJump();
                    }
                } else if (!moving && mountAnims_.rearUp > 0) {
                    LOG_DEBUG("Mount rear-up triggered: playing rearUp anim ", mountAnims_.rearUp);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.rearUp, false);
                    mountAction_ = MountAction::RearUp;
                    mountActionPhase_ = 0;
                    mountAnimId = mountAnims_.rearUp;
                    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                        renderer_->getAudioCoordinator()->getMountSoundManager()->playRearUpSound();
                    }
                }
            }

            // Handle active mount actions (jump chaining or rear-up)
            if (mountAction_ != MountAction::None) {
                bool animFinished = haveMountState && curMountDur > 0.1f &&
                                   (curMountTime >= curMountDur - 0.05f);

                if (mountAction_ == MountAction::Jump) {
                    if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop > 0) {
                        LOG_DEBUG("Mount jump: phase 0→1 (JumpStart→JumpLoop anim ", mountAnims_.jumpLoop, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                        mountActionPhase_ = 1;
                        mountAnimId = mountAnims_.jumpLoop;
                    } else if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop == 0) {
                        LOG_DEBUG("Mount jump: phase 0→1 (no JumpLoop, holding JumpStart)");
                        mountActionPhase_ = 1;
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd > 0) {
                        LOG_DEBUG("Mount jump: phase 1→2 (landed, JumpEnd anim ", mountAnims_.jumpEnd, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpEnd, false);
                        mountActionPhase_ = 2;
                        mountAnimId = mountAnims_.jumpEnd;
                        if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                            renderer_->getAudioCoordinator()->getMountSoundManager()->playLandSound();
                        }
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd == 0) {
                        LOG_DEBUG("Mount jump: phase 1→done (landed, no JumpEnd, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else if (mountActionPhase_ == 2 && animFinished) {
                        LOG_DEBUG("Mount jump: phase 2→done (JumpEnd finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;
                    }
                } else if (mountAction_ == MountAction::RearUp) {
                    if (animFinished) {
                        LOG_DEBUG("Mount rear-up: finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand));
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;
                    }
                }
            } else if (moving) {
                if (anyStrafeLeft) {
                    mountAnimId = pickMountAnim({ANIM_STRAFE_RUN_LEFT, ANIM_STRAFE_WALK_LEFT, ANIM_RUN}, ANIM_RUN);
                } else if (anyStrafeRight) {
                    mountAnimId = pickMountAnim({ANIM_STRAFE_RUN_RIGHT, ANIM_STRAFE_WALK_RIGHT, ANIM_RUN}, ANIM_RUN);
                } else if (movingBackward) {
                    mountAnimId = pickMountAnim({ANIM_BACKPEDAL}, ANIM_RUN);
                } else {
                    mountAnimId = ANIM_RUN;
                }
            }

            // Cancel active fidget immediately if movement starts
            if (moving && mountActiveFidget_ != 0) {
                mountActiveFidget_ = 0;
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
            }

            // Check if active fidget has completed
            if (!moving && mountActiveFidget_ != 0) {
                uint32_t curAnim = 0;
                float curTime = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(mountInstanceId_, curAnim, curTime, curDur)) {
                    if (curAnim != mountActiveFidget_ || curTime >= curDur * 0.95f) {
                        mountActiveFidget_ = 0;
                        LOG_DEBUG("Mount fidget completed");
                    }
                }
            }

            // Idle fidgets
            if (!moving && mountAction_ == MountAction::None && mountActiveFidget_ == 0 && !mountAnims_.fidgets.empty()) {
                mountIdleFidgetTimer_ += lastDeltaTime_;
                static std::mt19937 idleRng(std::random_device{}());
                static float nextFidgetTime = std::uniform_real_distribution<float>(6.0f, 12.0f)(idleRng);

                if (mountIdleFidgetTimer_ >= nextFidgetTime) {
                    std::uniform_int_distribution<size_t> dist(0, mountAnims_.fidgets.size() - 1);
                    uint32_t fidgetAnim = mountAnims_.fidgets[dist(idleRng)];

                    characterRenderer->playAnimation(mountInstanceId_, fidgetAnim, false);
                    mountActiveFidget_ = fidgetAnim;
                    mountIdleFidgetTimer_ = 0.0f;
                    nextFidgetTime = std::uniform_real_distribution<float>(6.0f, 12.0f)(idleRng);

                    LOG_DEBUG("Mount idle fidget: playing anim ", fidgetAnim);
                }
            }
            if (moving) {
                mountIdleFidgetTimer_ = 0.0f;
            }

            // Idle ambient sounds
            if (!moving && renderer_->getAudioCoordinator()->getMountSoundManager()) {
                mountIdleSoundTimer_ += lastDeltaTime_;
                static std::mt19937 soundRng(std::random_device{}());
                static float nextIdleSoundTime = std::uniform_real_distribution<float>(45.0f, 90.0f)(soundRng);

                if (mountIdleSoundTimer_ >= nextIdleSoundTime) {
                    renderer_->getAudioCoordinator()->getMountSoundManager()->playIdleSound();
                    mountIdleSoundTimer_ = 0.0f;
                    nextIdleSoundTime = std::uniform_real_distribution<float>(45.0f, 90.0f)(soundRng);
                }
            } else if (moving) {
                mountIdleSoundTimer_ = 0.0f;
            }

            // Only update animation if changed and not in action or fidget
            if (mountAction_ == MountAction::None && mountActiveFidget_ == 0 && (!haveMountState || curMountAnim != mountAnimId)) {
                bool loop = true;
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, loop);
            }

            taxi_mount_done:
            mountBob = 0.0f;
            if (moving && haveMountState && curMountDur > 1.0f) {
                float wrappedTime = curMountTime;
                while (wrappedTime >= curMountDur) {
                    wrappedTime -= curMountDur;
                }
                float norm = wrappedTime / curMountDur;
                float bobSpeed = taxiFlight_ ? 2.0f : 1.0f;
                mountBob = std::sin(norm * 2.0f * 3.14159f * bobSpeed) * 0.12f;
            }
        }

        // Use mount's attachment point for proper bone-driven rider positioning.
        if (taxiFlight_) {
            glm::mat4 mountSeatTransform(1.0f);
            bool haveSeat = false;
            static constexpr uint32_t kTaxiSeatAttachmentId = 0;
            if (mountSeatAttachmentId_ == -1) {
                mountSeatAttachmentId_ = static_cast<int>(kTaxiSeatAttachmentId);
            }
            if (mountSeatAttachmentId_ >= 0) {
                haveSeat = characterRenderer->getAttachmentTransform(
                    mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }

            if (haveSeat) {
                glm::vec3 targetRiderPos = glm::vec3(mountSeatTransform[3]) + glm::vec3(0.0f, 0.0f, 0.02f);
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
                characterRenderer->setInstancePosition(characterInstanceId, targetRiderPos);
            } else {
                mountSeatSmoothingInit_ = false;
                glm::vec3 playerPos = characterPosition + glm::vec3(0.0f, 0.0f, mountHeightOffset_ + 0.10f);
                characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            }

            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            float mountYawRadVal = glm::radians(characterYaw);
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, mountYawRadVal));
            return;
        }

        // Ground mounts: try a seat attachment first.
        glm::mat4 mountSeatTransform;
        bool haveSeat = false;
        if (mountSeatAttachmentId_ >= 0) {
            haveSeat = characterRenderer->getAttachmentTransform(
                mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
        } else if (mountSeatAttachmentId_ == -1) {
            static constexpr uint32_t kSeatAttachments[] = {0, 5, 6, 7, 8};
            for (uint32_t attId : kSeatAttachments) {
                if (characterRenderer->getAttachmentTransform(mountInstanceId_, attId, mountSeatTransform)) {
                    mountSeatAttachmentId_ = static_cast<int>(attId);
                    haveSeat = true;
                    break;
                }
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }
        }

        if (haveSeat) {
            glm::vec3 mountSeatPos = glm::vec3(mountSeatTransform[3]);
            glm::vec3 seatOffset = glm::vec3(0.0f, 0.0f, taxiFlight_ ? 0.04f : 0.08f);
            glm::vec3 targetRiderPos = mountSeatPos + seatOffset;
            if (moving) {
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
            } else if (!mountSeatSmoothingInit_) {
                smoothedMountSeatPos_ = targetRiderPos;
                mountSeatSmoothingInit_ = true;
            } else {
                float smoothHz = taxiFlight_ ? 10.0f : 14.0f;
                float alpha = 1.0f - std::exp(-smoothHz * std::max(lastDeltaTime_, 0.001f));
                smoothedMountSeatPos_ = glm::mix(smoothedMountSeatPos_, targetRiderPos, alpha);
            }

            characterRenderer->setInstancePosition(characterInstanceId, smoothedMountSeatPos_);

            float yawRad = glm::radians(characterYaw);
            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, yawRad));
        } else {
            mountSeatSmoothingInit_ = false;
            float yawRad = glm::radians(characterYaw);
            glm::mat4 mountRotation = glm::mat4(1.0f);
            mountRotation = glm::rotate(mountRotation, yawRad, glm::vec3(0.0f, 0.0f, 1.0f));
            mountRotation = glm::rotate(mountRotation, mountRoll_, glm::vec3(1.0f, 0.0f, 0.0f));
            mountRotation = glm::rotate(mountRotation, mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
            glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
            glm::vec3 playerPos = characterPosition + worldOffset;
            characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll_, yawRad));
        }
        return;
    }

    if (!forceMelee) switch (charAnimState_) {
        case CharAnimState::IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (inCombat_ && grounded) {
                newState = CharAnimState::COMBAT_IDLE;
            }
            break;

        case CharAnimState::WALK:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (sprinting) {
                newState = CharAnimState::RUN;
            }
            break;

        case CharAnimState::RUN:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (!sprinting) {
                newState = CharAnimState::WALK;
            }
            break;

        case CharAnimState::JUMP_START:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            } else {
                newState = CharAnimState::JUMP_MID;
            }
            break;

        case CharAnimState::JUMP_MID:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            }
            break;

        case CharAnimState::JUMP_END:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::SIT_DOWN:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::SITTING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::EMOTE:
            if (swim) {
                cancelEmote();
                newState = CharAnimState::SWIM_IDLE;
            } else if (jumping || !grounded) {
                cancelEmote();
                newState = CharAnimState::JUMP_START;
            } else if (moving) {
                cancelEmote();
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (sitting) {
                cancelEmote();
                newState = CharAnimState::SIT_DOWN;
            } else if (!emoteLoop_ && characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)
                        && curDur > 0.1f && curT >= curDur - 0.05f) {
                    cancelEmote();
                    newState = CharAnimState::IDLE;
                }
            }
            break;

        case CharAnimState::SWIM_IDLE:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (moving) {
                newState = CharAnimState::SWIM;
            }
            break;

        case CharAnimState::SWIM:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (!moving) {
                newState = CharAnimState::SWIM_IDLE;
            }
            break;

        case CharAnimState::MELEE_SWING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (sitting) {
                newState = CharAnimState::SIT_DOWN;
            } else if (inCombat_) {
                newState = CharAnimState::COMBAT_IDLE;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::MOUNT:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::COMBAT_IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (!inCombat_) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::CHARGE:
            break;
    }

    if (forceMelee) {
        newState = CharAnimState::MELEE_SWING;
    }

    if (charging_) {
        newState = CharAnimState::CHARGE;
    }

    if (newState != charAnimState_) {
        charAnimState_ = newState;
    }

    auto pickFirstAvailable = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer->hasAnimation(characterInstanceId, id)) {
                return id;
            }
        }
        return fallback;
    };

    uint32_t animId = ANIM_STAND;
    bool loop = true;

    switch (charAnimState_) {
        case CharAnimState::IDLE:       animId = ANIM_STAND;      loop = true;  break;
        case CharAnimState::WALK:
            if (movingBackward) {
                animId = pickFirstAvailable({ANIM_BACKPEDAL}, ANIM_WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({ANIM_STRAFE_WALK_LEFT, ANIM_STRAFE_RUN_LEFT}, ANIM_WALK);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({ANIM_STRAFE_WALK_RIGHT, ANIM_STRAFE_RUN_RIGHT}, ANIM_WALK);
            } else {
                animId = pickFirstAvailable({ANIM_WALK, ANIM_RUN}, ANIM_STAND);
            }
            loop = true;
            break;
        case CharAnimState::RUN:
            if (movingBackward) {
                animId = pickFirstAvailable({ANIM_BACKPEDAL}, ANIM_WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({ANIM_STRAFE_RUN_LEFT}, ANIM_RUN);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({ANIM_STRAFE_RUN_RIGHT}, ANIM_RUN);
            } else {
                animId = pickFirstAvailable({ANIM_RUN, ANIM_WALK}, ANIM_STAND);
            }
            loop = true;
            break;
        case CharAnimState::JUMP_START: animId = ANIM_JUMP_START; loop = false; break;
        case CharAnimState::JUMP_MID:   animId = ANIM_JUMP_MID;   loop = false; break;
        case CharAnimState::JUMP_END:   animId = ANIM_JUMP_END;   loop = false; break;
        case CharAnimState::SIT_DOWN:   animId = ANIM_SIT_DOWN;   loop = false; break;
        case CharAnimState::SITTING:    animId = ANIM_SITTING;    loop = true;  break;
        case CharAnimState::EMOTE:      animId = emoteAnimId_;    loop = emoteLoop_; break;
        case CharAnimState::SWIM_IDLE:  animId = ANIM_SWIM_IDLE;  loop = true;  break;
        case CharAnimState::SWIM:       animId = ANIM_SWIM;       loop = true;  break;
        case CharAnimState::MELEE_SWING:
            animId = resolveMeleeAnimId();
            if (animId == 0) {
                animId = ANIM_STAND;
            }
            loop = false;
            break;
        case CharAnimState::MOUNT:      animId = ANIM_MOUNT;      loop = true;  break;
        case CharAnimState::COMBAT_IDLE:
            animId = pickFirstAvailable(
                {ANIM_READY_1H, ANIM_READY_2H, ANIM_READY_2H_L, ANIM_READY_UNARMED},
                ANIM_STAND);
            loop = true;
            break;
        case CharAnimState::CHARGE:
            animId = ANIM_RUN;
            loop = true;
            break;
    }

    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f;
    float currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    const bool requestChanged = (lastPlayerAnimRequest_ != animId) || (lastPlayerAnimLoopRequest_ != loop);
    const bool shouldPlay = (haveState && currentAnimId != animId) || (!haveState && requestChanged);
    if (shouldPlay) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }
}

// ── Footstep event detection ─────────────────────────────────────────────────

bool AnimationController::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs) {
    if (animationDurationMs <= 1.0f) {
        footstepNormInitialized_ = false;
        return false;
    }

    float wrappedTime = animationTimeMs;
    while (wrappedTime >= animationDurationMs) {
        wrappedTime -= animationDurationMs;
    }
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    float norm = wrappedTime / animationDurationMs;

    if (animationId != footstepLastAnimationId_) {
        footstepLastAnimationId_ = animationId;
        footstepLastNormTime_ = norm;
        footstepNormInitialized_ = true;
        return false;
    }

    if (!footstepNormInitialized_) {
        footstepNormInitialized_ = true;
        footstepLastNormTime_ = norm;
        return false;
    }

    auto crossed = [&](float eventNorm) {
        if (footstepLastNormTime_ <= norm) {
            return footstepLastNormTime_ < eventNorm && eventNorm <= norm;
        }
        return footstepLastNormTime_ < eventNorm || eventNorm <= norm;
    };

    bool trigger = crossed(0.22f) || crossed(0.72f);
    footstepLastNormTime_ = norm;
    return trigger;
}

audio::FootstepSurface AnimationController::resolveFootstepSurface() const {
    auto* cameraController = renderer_->getCameraController();
    if (!cameraController || !cameraController->isThirdPerson()) {
        return audio::FootstepSurface::STONE;
    }

    const glm::vec3& p = renderer_->getCharacterPosition();

    float distSq = glm::dot(p - cachedFootstepPosition_, p - cachedFootstepPosition_);
    if (distSq < 2.25f && cachedFootstepUpdateTimer_ < 0.5f) {
        return cachedFootstepSurface_;
    }

    cachedFootstepPosition_ = p;
    cachedFootstepUpdateTimer_ = 0.0f;

    if (cameraController->isSwimming()) {
        cachedFootstepSurface_ = audio::FootstepSurface::WATER;
        return audio::FootstepSurface::WATER;
    }

    auto* waterRenderer = renderer_->getWaterRenderer();
    if (waterRenderer) {
        auto waterH = waterRenderer->getWaterHeightAt(p.x, p.y);
        if (waterH && p.z < (*waterH + 0.25f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::WATER;
            return audio::FootstepSurface::WATER;
        }
    }

    auto* wmoRenderer = renderer_->getWMORenderer();
    auto* terrainManager = renderer_->getTerrainManager();
    if (wmoRenderer) {
        auto wmoFloor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
        auto terrainFloor = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
        if (wmoFloor && (!terrainFloor || *wmoFloor >= *terrainFloor - 0.1f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::STONE;
            return audio::FootstepSurface::STONE;
        }
    }

    audio::FootstepSurface surface = audio::FootstepSurface::STONE;

    if (terrainManager) {
        auto texture = terrainManager->getDominantTextureAt(p.x, p.y);
        if (texture) {
            std::string t = *texture;
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t.find("snow") != std::string::npos || t.find("ice") != std::string::npos) surface = audio::FootstepSurface::SNOW;
            else if (t.find("grass") != std::string::npos || t.find("moss") != std::string::npos || t.find("leaf") != std::string::npos) surface = audio::FootstepSurface::GRASS;
            else if (t.find("sand") != std::string::npos || t.find("dirt") != std::string::npos || t.find("mud") != std::string::npos) surface = audio::FootstepSurface::DIRT;
            else if (t.find("wood") != std::string::npos || t.find("timber") != std::string::npos) surface = audio::FootstepSurface::WOOD;
            else if (t.find("metal") != std::string::npos || t.find("iron") != std::string::npos) surface = audio::FootstepSurface::METAL;
            else if (t.find("stone") != std::string::npos || t.find("rock") != std::string::npos || t.find("cobble") != std::string::npos || t.find("brick") != std::string::npos) surface = audio::FootstepSurface::STONE;
        }
    }

    cachedFootstepSurface_ = surface;
    return surface;
}

// ── Footstep update (called from Renderer::update) ──────────────────────────

void AnimationController::updateFootsteps(float deltaTime) {
    auto* footstepManager = renderer_->getAudioCoordinator()->getFootstepManager();
    if (!footstepManager) return;

    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();

    footstepManager->update(deltaTime);
    cachedFootstepUpdateTimer_ += deltaTime;

    bool canPlayFootsteps = characterRenderer && characterInstanceId > 0 &&
        cameraController && cameraController->isThirdPerson() &&
        cameraController->isGrounded() && !cameraController->isSwimming();

    if (canPlayFootsteps && isMounted() && mountInstanceId_ > 0 && !taxiFlight_) {
        // Mount footsteps: use mount's animation for timing
        uint32_t animId = 0;
        float animTimeMs = 0.0f, animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(mountInstanceId_, animId, animTimeMs, animDurationMs) &&
            animDurationMs > 1.0f && cameraController->isMoving()) {
            float wrappedTime = animTimeMs;
            while (wrappedTime >= animDurationMs) {
                wrappedTime -= animDurationMs;
            }
            if (wrappedTime < 0.0f) wrappedTime += animDurationMs;
            float norm = wrappedTime / animDurationMs;

            if (animId != mountFootstepLastAnimId_) {
                mountFootstepLastAnimId_ = animId;
                mountFootstepLastNormTime_ = norm;
                mountFootstepNormInitialized_ = true;
            } else if (!mountFootstepNormInitialized_) {
                mountFootstepNormInitialized_ = true;
                mountFootstepLastNormTime_ = norm;
            } else {
                auto crossed = [&](float eventNorm) {
                    if (mountFootstepLastNormTime_ <= norm) {
                        return mountFootstepLastNormTime_ < eventNorm && eventNorm <= norm;
                    }
                    return mountFootstepLastNormTime_ < eventNorm || eventNorm <= norm;
                };
                if (crossed(0.25f) || crossed(0.75f)) {
                    footstepManager->playFootstep(resolveFootstepSurface(), true);
                }
                mountFootstepLastNormTime_ = norm;
            }
        } else {
            mountFootstepNormInitialized_ = false;
        }
        footstepNormInitialized_ = false;
    } else if (canPlayFootsteps && isFootstepAnimationState()) {
        uint32_t animId = 0;
        float animTimeMs = 0.0f;
        float animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(characterInstanceId, animId, animTimeMs, animDurationMs) &&
            shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs)) {
            auto surface = resolveFootstepSurface();
            footstepManager->playFootstep(surface, cameraController->isSprinting());
            if (surface == audio::FootstepSurface::WATER) {
                if (renderer_->getAudioCoordinator()->getMovementSoundManager()) {
                    renderer_->getAudioCoordinator()->getMovementSoundManager()->playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
                }
                auto* swimEffects = renderer_->getSwimEffects();
                auto* waterRenderer = renderer_->getWaterRenderer();
                if (swimEffects && waterRenderer) {
                    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
                    auto wh = waterRenderer->getWaterHeightAt(characterPosition.x, characterPosition.y);
                    if (wh) {
                        swimEffects->spawnFootSplash(characterPosition, *wh);
                    }
                }
            }
        }
        mountFootstepNormInitialized_ = false;
    } else {
        footstepNormInitialized_ = false;
        mountFootstepNormInitialized_ = false;
    }
}

// ── Activity SFX state tracking ──────────────────────────────────────────────

void AnimationController::updateSfxState(float deltaTime) {
    auto* activitySoundManager = renderer_->getAudioCoordinator()->getActivitySoundManager();
    if (!activitySoundManager) return;

    auto* cameraController = renderer_->getCameraController();

    activitySoundManager->update(deltaTime);
    if (cameraController && cameraController->isThirdPerson()) {
        bool grounded = cameraController->isGrounded();
        bool jumping = cameraController->isJumping();
        bool falling = cameraController->isFalling();
        bool swimming = cameraController->isSwimming();
        bool moving = cameraController->isMoving();

        if (!sfxStateInitialized_) {
            sfxPrevGrounded_ = grounded;
            sfxPrevJumping_ = jumping;
            sfxPrevFalling_ = falling;
            sfxPrevSwimming_ = swimming;
            sfxStateInitialized_ = true;
        }

        if (jumping && !sfxPrevJumping_ && !swimming) {
            activitySoundManager->playJump();
        }

        if (grounded && !sfxPrevGrounded_) {
            bool hardLanding = sfxPrevFalling_;
            activitySoundManager->playLanding(resolveFootstepSurface(), hardLanding);
        }

        if (swimming && !sfxPrevSwimming_) {
            activitySoundManager->playWaterEnter();
        } else if (!swimming && sfxPrevSwimming_) {
            activitySoundManager->playWaterExit();
        }

        activitySoundManager->setSwimmingState(swimming, moving);

        if (renderer_->getAudioCoordinator()->getMusicManager()) {
            renderer_->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(swimming);
        }

        sfxPrevGrounded_ = grounded;
        sfxPrevJumping_ = jumping;
        sfxPrevFalling_ = falling;
        sfxPrevSwimming_ = swimming;
    } else {
        activitySoundManager->setSwimmingState(false, false);
        if (renderer_->getAudioCoordinator()->getMusicManager()) {
            renderer_->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(false);
        }
        sfxStateInitialized_ = false;
    }

    // Mount ambient sounds
    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->update(deltaTime);
        if (cameraController && isMounted()) {
            bool isMoving = cameraController->isMoving();
            bool flying = taxiFlight_ || !cameraController->isGrounded();
            renderer_->getAudioCoordinator()->getMountSoundManager()->setMoving(isMoving);
            renderer_->getAudioCoordinator()->getMountSoundManager()->setFlying(flying);
        }
    }
}

} // namespace rendering
} // namespace wowee
