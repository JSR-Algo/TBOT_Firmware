#include "lesson_tvideo_template.h"

#include <cstring>
#include <cmath>

namespace lesson_tvideo {
namespace {

constexpr uint32_t kPhaseDurationsMs[] = {100, 1200, 700, 350, 1800, 250, 650, 100};

struct PresetGeometry {
    const char* id;
    Rect entry;
    Rect land;
    LayoutGeometry arrived;
};

constexpr PresetGeometry kPresets[] = {
    {"centerRoad", {400, 22, 96, 84}, {284, 116, 96, 84},
     {{184, 184, 112, 56}, {40, 104, 124, 124}, {150, 244, 180, 42}, {26, 18, 428, 48}, {390, 286, 64, 16}}},
    {"leftApproach", {24, 22, 96, 84}, {104, 116, 96, 84},
     {{42, 148, 108, 92}, {240, 104, 124, 124}, {150, 244, 180, 42}, {26, 18, 428, 48}, {390, 286, 64, 16}}},
    {"rightApproach", {410, 22, 96, 84}, {326, 116, 96, 84},
     {{330, 148, 108, 92}, {116, 104, 124, 124}, {150, 244, 180, 42}, {26, 18, 428, 48}, {390, 286, 64, 16}}},
};

const PresetGeometry* FindPreset(const char* id) {
    if (id == nullptr) return nullptr;
    for (const auto& preset : kPresets) {
        if (std::strcmp(id, preset.id) == 0) return &preset;
    }
    return nullptr;
}

uint8_t PhaseIndex(Phase phase) {
    return static_cast<uint8_t>(phase);
}

}  // namespace

bool IsSupported(const char* template_id, uint8_t template_version,
                 const char* layout_preset, uint8_t geometry_version) {
    return template_id != nullptr && std::strcmp(template_id, "tvideoFlyWalk") == 0 &&
           template_version == 1 && geometry_version == 1 && FindPreset(layout_preset) != nullptr;
}

bool ExactVersion(double value, uint8_t* out) {
    if (out == nullptr || !std::isfinite(value) || value < 0 || value > 255 || std::floor(value) != value) return false;
    *out = static_cast<uint8_t>(value);
    return true;
}

bool Overlaps(const Rect& a, const Rect& b) {
    return a.left < b.left + b.width && a.left + a.width > b.left &&
           a.top < b.top + b.height && a.top + a.height > b.top;
}

const LayoutGeometry* ArrivedGeometry(const char* layout_preset, uint8_t geometry_version) {
    if (geometry_version != 1) return nullptr;
    const auto* preset = FindPreset(layout_preset);
    return preset == nullptr ? nullptr : &preset->arrived;
}

const char* DegradedReasonName(DegradedReason reason) {
    switch (reason) {
        case DegradedReason::kNone: return "none";
        case DegradedReason::kMissingAtlas: return "missingAtlas";
        case DegradedReason::kMissingOverlay: return "missingOverlay";
        case DegradedReason::kPhaseTimeout: return "phaseTimeout";
        case DegradedReason::kReducedMotion: return "reducedMotion";
        case DegradedReason::kUnsupportedContract: return "unsupportedContract";
    }
    return "unsupportedContract";
}

StateMachine::StateMachine(const Config& config) : layout_preset_(config.layout_preset) {
    if (config.template_id == nullptr) {
        bypass_ = true;
        return;
    }
    if (!IsSupported(config.template_id, config.template_version, config.layout_preset, config.geometry_version)) {
        Reveal(DegradedReason::kUnsupportedContract);
        return;
    }
    const auto* preset = FindPreset(layout_preset_);
    geometry_ = preset->arrived;
    if (!config.arrived_pose_available) {
        Reveal(DegradedReason::kMissingOverlay);
    } else if (config.reduced_motion) {
        Reveal(DegradedReason::kReducedMotion);
    } else if (!config.atlas_available) {
        Reveal(DegradedReason::kMissingAtlas);
    } else {
        phase_ = Phase::kHidden;
        content_visible_ = false;
        ApplyPhaseGeometry();
    }
}

void StateMachine::Advance(uint32_t elapsed_ms) {
    if (bypass_ || phase_ == Phase::kRevealTeachingContent) return;
    phase_elapsed_ms_ += elapsed_ms;
    while (phase_ != Phase::kRevealTeachingContent) {
        const uint32_t duration = kPhaseDurationsMs[PhaseIndex(phase_)];
        if (phase_elapsed_ms_ < duration) break;
        phase_elapsed_ms_ -= duration;
        phase_ = static_cast<Phase>(PhaseIndex(phase_) + 1);
        if (phase_ == Phase::kRevealTeachingContent) {
            content_visible_ = true;
            phase_elapsed_ms_ = 0;
        }
        ApplyPhaseGeometry();
    }
}

void StateMachine::Timeout() {
    if (!bypass_ && phase_ != Phase::kRevealTeachingContent) Reveal(DegradedReason::kPhaseTimeout);
}

void StateMachine::Reveal(DegradedReason reason) {
    phase_ = Phase::kRevealTeachingContent;
    degraded_reason_ = reason;
    content_visible_ = true;
    phase_elapsed_ms_ = 0;
    ApplyPhaseGeometry();
}

void StateMachine::ApplyPhaseGeometry() {
    const auto* preset = FindPreset(layout_preset_);
    if (preset == nullptr) return;
    geometry_ = preset->arrived;
    if (phase_ == Phase::kHidden || phase_ == Phase::kFlyIn) geometry_.robot = preset->entry;
    if (phase_ == Phase::kLandFar || phase_ == Phase::kSettle) geometry_.robot = preset->land;
}

}  // namespace lesson_tvideo
