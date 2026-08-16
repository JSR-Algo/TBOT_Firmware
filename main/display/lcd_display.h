#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <cstdint>
#include <atomic>
#include <mutex>

class LessonCinematicPanelCompletionGate {
public:
    void ArmNextCompletion() {
        armed_generation_.store(
            completed_generation_.load(std::memory_order_acquire) + 1,
            std::memory_order_release);
    }

    void Disarm() { armed_generation_.store(0, std::memory_order_release); }

    bool OnPanelCompletion() {
        const std::uint32_t completed =
            completed_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::uint32_t armed = armed_generation_.load(std::memory_order_acquire);
        return armed != 0 && completed == armed &&
               armed_generation_.compare_exchange_strong(
                   armed, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    std::atomic<std::uint32_t> completed_generation_{0};
    std::atomic<std::uint32_t> armed_generation_{0};
};

class LessonCinematicDisplayTransport {
public:
    struct Ops {
        void* context = nullptr;
        bool (*begin)(void* context) = nullptr;
        bool (*queue)(void* context, const std::uint16_t* pixels) = nullptr;
        bool (*wait)(void* context, std::uint32_t timeout_ms) = nullptr;
        bool (*end)(void* context) = nullptr;
    };

    explicit LessonCinematicDisplayTransport(Ops ops) : ops_(ops) {}
    ~LessonCinematicDisplayTransport() { EndLessonCinematic(); }

    LessonCinematicDisplayTransport(const LessonCinematicDisplayTransport&) = delete;
    LessonCinematicDisplayTransport& operator=(const LessonCinematicDisplayTransport&) = delete;

    bool BeginLessonCinematic() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::kIdle || ops_.begin == nullptr || ops_.end == nullptr) {
                return false;
            }
            state_ = State::kBeginning;
            end_requested_ = false;
        }

        const bool began = ops_.begin(ops_.context);
        bool finish_end = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!began || end_requested_) {
                state_ = State::kEnding;
                finish_end = true;
            } else {
                state_ = State::kOwned;
            }
        }
        if (finish_end) FinishEnd(State::kIdle);
        if (!began) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == State::kOwned;
    }

    bool QueueLessonCinematicFrame(const std::uint16_t* pixels, std::uint16_t width,
                                   std::uint16_t height) {
        if (pixels == nullptr || width != 320 || height != 480 || ops_.queue == nullptr) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::kOwned) return false;
            state_ = State::kQueueing;
        }

        const bool queued = ops_.queue(ops_.context, pixels);
        bool finish_end = false;
        State fallback = State::kOwned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fallback = queued ? State::kInFlight : State::kOwned;
            if (!queued || end_requested_) {
                state_ = State::kEnding;
                finish_end = true;
            } else {
                state_ = State::kInFlight;
            }
        }
        if (finish_end) FinishEnd(fallback);
        if (!queued) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == State::kInFlight;
    }

    bool WaitLessonCinematicFrame(std::uint32_t timeout_ms) {
        if (ops_.wait == nullptr) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::kInFlight) return false;
            state_ = State::kWaiting;
        }

        const bool completed = ops_.wait(ops_.context, timeout_ms);
        bool finish_end = false;
        State fallback = completed ? State::kOwned : State::kInFlight;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (end_requested_) {
                state_ = State::kEnding;
                finish_end = true;
            } else {
                state_ = fallback;
            }
        }
        if (finish_end) FinishEnd(fallback);
        return completed;
    }

    void EndLessonCinematic() {
        State fallback = State::kIdle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            switch (state_) {
                case State::kIdle:
                case State::kEnding:
                    return;
                case State::kBeginning:
                case State::kQueueing:
                case State::kWaiting:
                    end_requested_ = true;
                    return;
                case State::kOwned:
                    fallback = State::kOwned;
                    break;
                case State::kInFlight:
                    fallback = State::kInFlight;
                    break;
            }
            state_ = State::kEnding;
        }
        FinishEnd(fallback);
    }

private:
    enum class State {
        kIdle,
        kBeginning,
        kOwned,
        kQueueing,
        kInFlight,
        kWaiting,
        kEnding,
    };

    void FinishEnd(State fallback) {
        const bool released = ops_.end(ops_.context);
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = released ? State::kIdle : fallback;
        end_requested_ = false;
    }

    Ops ops_;
    std::mutex mutex_;
    State state_ = State::kIdle;
    bool end_requested_ = false;
};

#ifndef TBOT_LESSON_CINEMATIC_TRANSPORT_ONLY
#include "lvgl_display.h"
#include "gif/lvgl_gif.h"
#include "lesson_renderer_memory_probe.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* lesson_background_ = nullptr;  // US-006: full-screen, persistent lesson poster
    std::unique_ptr<LvglImage> lesson_background_cached_ = nullptr;
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    std::unique_ptr<LvglImage> lesson_cinematic_framebuffer_ = nullptr;
    std::uint16_t* lesson_cinematic_pixels_ = nullptr;
#endif
    lv_obj_t* lesson_object_ = nullptr;  // US-006: foreground teaching object layer
    std::unique_ptr<LvglImage> lesson_object_cached_ = nullptr;
    lv_obj_t* lesson_robot_overlay_ = nullptr;  // US-006: robot overlay image layer
    std::unique_ptr<LvglImage> lesson_robot_overlay_cached_ = nullptr;
    bool lesson_robot_overlay_bounds_set_ = false;
    int lesson_robot_overlay_left_ = 0;
    int lesson_robot_overlay_top_ = 0;
    int lesson_robot_overlay_width_ = 0;
    int lesson_robot_overlay_height_ = 0;
    void* lesson_robot_animation_context_ = nullptr;
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonRendererMemoryProbe lesson_renderer_memory_probe_;
#endif
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
    lv_timer_t* lesson_renderer_settled_timer_ = nullptr;
    uint32_t lesson_renderer_settled_generation_ = 0;
    uint32_t lesson_renderer_settled_armed_generation_ = 0;
    LessonRendererMemoryPhase lesson_renderer_settled_phase_ =
        LessonRendererMemoryPhase::kComplete;
#endif
    int lesson_robot_arrived_left_ = 0;
    int lesson_robot_arrived_top_ = 0;
    int lesson_robot_arrived_width_ = 0;
    int lesson_robot_arrived_height_ = 0;
    lv_obj_t* lesson_caption_bar_ = nullptr;  // Lesson-only bottom caption overlay
    lv_obj_t* lesson_caption_label_ = nullptr;
    lv_obj_t* lesson_word_pill_ = nullptr;
    lv_obj_t* lesson_word_label_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    std::atomic<bool> lesson_mode_active_{false};

    void InitializeLcdThemes();
    void CancelLessonRobotEntranceLocked();
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
    void CancelLessonRendererSettledObservationLocked();
    void ScheduleLessonRendererSettledObservationLocked(
        LessonRendererMemoryPhase phase);
#endif
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetLessonCaption(const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetLessonBackground(std::unique_ptr<LvglImage> image) override;
    virtual void SetLessonObject(std::unique_ptr<LvglImage> image) override;
    virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) override;
    virtual void SetLessonRobotOverlayBounds(int left, int top, int width, int height) override;
    virtual void SetLessonTeachingWord(const char* text) override;
    virtual bool BeginLessonCinematic();
    virtual bool QueueLessonCinematicFrame(const std::uint16_t* pixels, std::uint16_t width,
                                           std::uint16_t height);
    virtual bool WaitLessonCinematicFrame(std::uint32_t timeout_ms);
    virtual void EndLessonCinematic();
    bool PresentLessonFramebuffer(const std::uint16_t* pixels, std::uint16_t width,
                                  std::uint16_t height);
    virtual bool StartLessonRobotEntrance(
        const LessonRobotEntrancePlan& plan, LessonVisualCompletion completion) override;
    virtual void CancelLessonRobotEntrance() override;
    virtual bool ApplyLessonVisualState(
        const LessonVisualState& state, LessonVisualCompletion completion) override;
    virtual void SetLessonMode(bool active) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};
#endif  // TBOT_LESSON_CINEMATIC_TRANSPORT_ONLY

#endif // LCD_DISPLAY_H
