#pragma once

#include <cstdint>

namespace lesson_tvideo {

enum class Phase : uint8_t {
    kHidden,
    kFlyIn,
    kLandFar,
    kSettle,
    kWalkToward,
    kArriveNear,
    kGreetIdle,
    kRevealTeachingContent,
};

enum class DegradedReason : uint8_t {
    kNone,
    kMissingAtlas,
    kMissingOverlay,
    kPhaseTimeout,
    kReducedMotion,
    kUnsupportedContract,
};

struct Rect {
    int16_t left;
    int16_t top;
    int16_t width;
    int16_t height;
};

struct LayoutGeometry {
    Rect robot;
    Rect teaching_object;
    Rect word_pill;
    Rect prompt;
    Rect progress;
};

struct Config {
    const char* template_id;
    uint8_t template_version;
    const char* layout_preset;
    uint8_t geometry_version;
    bool arrived_pose_available;
    bool atlas_available;
    bool reduced_motion;
};

bool IsSupported(const char* template_id, uint8_t template_version,
                 const char* layout_preset, uint8_t geometry_version);
uint8_t PhaseCount();
const char* PhaseName(uint8_t index);
uint32_t PhaseDurationMs(uint8_t index);
bool ExactVersion(double value, uint8_t* out);
bool Overlaps(const Rect& a, const Rect& b);
const LayoutGeometry* ArrivedGeometry(const char* layout_preset, uint8_t geometry_version);
const char* DegradedReasonName(DegradedReason reason);

class StateMachine {
public:
    explicit StateMachine(const Config& config);

    void Advance(uint32_t elapsed_ms);
    void Timeout();

    Phase phase() const { return phase_; }
    DegradedReason degraded_reason() const { return degraded_reason_; }
    bool content_visible() const { return content_visible_; }
    bool bypass() const { return bypass_; }
    const LayoutGeometry& geometry() const { return geometry_; }

private:
    void Reveal(DegradedReason reason);
    void ApplyPhaseGeometry();

    Phase phase_ = Phase::kRevealTeachingContent;
    DegradedReason degraded_reason_ = DegradedReason::kNone;
    uint32_t phase_elapsed_ms_ = 0;
    bool content_visible_ = true;
    bool bypass_ = false;
    const char* layout_preset_ = nullptr;
    LayoutGeometry geometry_{};
};

}  // namespace lesson_tvideo
