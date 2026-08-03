#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <cstdint>

class LessonCinematicDisplayTransport {
public:
    struct Ops {
        void* context = nullptr;
        bool (*begin)(void* context) = nullptr;
        bool (*queue)(void* context, const std::uint16_t* pixels) = nullptr;
        bool (*wait)(void* context, std::uint32_t timeout_ms) = nullptr;
        void (*end)(void* context) = nullptr;
    };

    explicit LessonCinematicDisplayTransport(Ops ops) : ops_(ops) {}
    ~LessonCinematicDisplayTransport() { EndLessonCinematic(); }

    LessonCinematicDisplayTransport(const LessonCinematicDisplayTransport&) = delete;
    LessonCinematicDisplayTransport& operator=(const LessonCinematicDisplayTransport&) = delete;

    bool BeginLessonCinematic() {
        if (owned_ || cleanup_owed_ || ops_.begin == nullptr || ops_.end == nullptr) {
            return false;
        }
        cleanup_owed_ = true;
        if (!ops_.begin(ops_.context)) {
            EndLessonCinematic();
            return false;
        }
        owned_ = true;
        return true;
    }

    bool QueueLessonCinematicFrame(const std::uint16_t* pixels, std::uint16_t width,
                                   std::uint16_t height) {
        if (!owned_ || in_flight_ || pixels == nullptr || width != 320 || height != 480 ||
            ops_.queue == nullptr) {
            return false;
        }
        in_flight_ = true;
        if (!ops_.queue(ops_.context, pixels)) {
            EndLessonCinematic();
            return false;
        }
        return true;
    }

    bool WaitLessonCinematicFrame(std::uint32_t timeout_ms) {
        if (!owned_ || !in_flight_ || ops_.wait == nullptr) return false;
        if (!ops_.wait(ops_.context, timeout_ms)) {
            EndLessonCinematic();
            return false;
        }
        in_flight_ = false;
        return true;
    }

    void EndLessonCinematic() {
        if (!cleanup_owed_) return;
        ops_.end(ops_.context);
        in_flight_ = false;
        owned_ = false;
        cleanup_owed_ = false;
    }

private:
    Ops ops_;
    bool owned_ = false;
    bool in_flight_ = false;
    bool cleanup_owed_ = false;
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
