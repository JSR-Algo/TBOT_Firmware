#ifndef LESSON_MOTION_PRESETS_H_
#define LESSON_MOTION_PRESETS_H_

class RobotUart;

enum class LessonMotionResult {
    kApplied,
    kDegraded,
};

// Dispatches one of the fixed lesson-safe names. All motion is best-effort: an
// unavailable UART or timer degrades the step instead of failing the lesson.
LessonMotionResult DispatchLessonMotionPreset(RobotUart& robot_uart, const char* preset);

#endif  // LESSON_MOTION_PRESETS_H_
