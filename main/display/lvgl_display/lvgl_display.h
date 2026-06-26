#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    // US-006 lesson image render: a full-screen, PERSISTENT background image draw,
    // distinct from SetPreviewImage (which is centered, half-screen, and auto-hides
    // after PREVIEW_IMAGE_DURATION_MS). The lesson background must stay up for the
    // whole step, so it has no auto-hide timer. Default no-op (only LcdDisplay, the
    // class that owns the LVGL object tree, implements it); pass nullptr to clear it
    // and restore the realtime emoji face. Reuses the same LvglImage/decoder path.
    virtual void SetLessonBackground(std::unique_ptr<LvglImage> image) {}
    // Foreground teaching-object layer for lesson steps. It stacks above the lesson
    // background and below the persistent system/status bars. Default no-op for
    // non-LVGL displays; pass nullptr to clear stale step objects.
    virtual void SetLessonObject(std::unique_ptr<LvglImage> image) {}
    // Robot overlay image layer for lesson steps. It stacks above the teaching
    // object and below system/status bars; pass nullptr to clear stale overlays.
    virtual void SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) {}
    // Lesson display mode: when active, hide the idle realtime emoji face so ONLY the
    // lesson's three image layers (background/object/overlay) show. Toggled true on
    // lesson_start and false on lesson_stop/lesson_error so the smiley does not bleed
    // through the lesson scene (or show on a caption-only/asset-fetch-failed step).
    // Default no-op (only LcdDisplay owns the emoji LVGL object tree).
    virtual void SetLessonMode(bool active) {}
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


#endif
