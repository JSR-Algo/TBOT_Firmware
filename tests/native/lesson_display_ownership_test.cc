#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "chat_runtime_timing.h"

#define CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P 1
#define CONFIG_USE_WECHAT_MESSAGE_STYLE 0
#define CONFIG_USE_MULTILINE_CHAT_MESSAGE 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(value) assert((value) == 0)
#define ESP_TIMER_TASK 0
#define ESP_PM_APB_FREQ_MAX 0
#define ESP_ERR_NOT_SUPPORTED -1
#define PREVIEW_IMAGE_DURATION_MS 5000
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ALIGN_BOTTOM_MID 0
#define LV_ALIGN_CENTER 0
#define LV_OPA_TRANSP 0
#define LV_COLOR_FORMAT_RGB565 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define FONT_AWESOME_VOLUME_XMARK "mute"
#define FONT_AWESOME_BATTERY_BOLT "charge"
#define FONT_AWESOME_BATTERY_EMPTY "empty"
#define FONT_AWESOME_BATTERY_QUARTER "quarter"
#define FONT_AWESOME_BATTERY_HALF "half"
#define FONT_AWESOME_BATTERY_THREE_QUARTERS "threequarters"
#define FONT_AWESOME_BATTERY_FULL "full"
constexpr int kLessonCaptionBottomInsetDivisor = 20;
int lock_depth = 0;
std::function<void()> before_lock;
struct DisplayLockGuard {
    template<class T> explicit DisplayLockGuard(T*) {
        if (before_lock) { auto callback = std::move(before_lock); before_lock = {}; callback(); }
        ++lock_depth;
    }
    ~DisplayLockGuard() { --lock_depth; }
};
struct lv_obj_t { bool hidden = false; unsigned writes = 0; std::string text = "old"; };
void lv_obj_add_flag(lv_obj_t* o, int) { assert(lock_depth && o); o->hidden = true; ++o->writes; }
void lv_obj_remove_flag(lv_obj_t* o, int) { assert(lock_depth && o); o->hidden = false; ++o->writes; }
bool lv_obj_has_flag(lv_obj_t* o, int) { return o->hidden; }
void lv_label_set_text(lv_obj_t* o, const char* text) { assert(lock_depth); o->text = text ? text : ""; ++o->writes; }
const char* lv_label_get_text(lv_obj_t* o) { return o->text.c_str(); }
void lv_obj_move_foreground(lv_obj_t* o) { assert(lock_depth); ++o->writes; }
void lv_obj_align(lv_obj_t*, int, int, int) { assert(lock_depth); }
void lv_obj_set_style_bg_opa(lv_obj_t*, int, int) { assert(lock_depth); }
void lv_obj_invalidate(lv_obj_t*) { assert(lock_depth); }
void lv_obj_clean(lv_obj_t*) { assert(lock_depth); }
void lv_refr_now(void*) { assert(lock_depth); }
int LessonImageCoverScale(int, int, int, int) { return 256; }
bool fail_allocation=false;
void* heap_caps_malloc(size_t size, int) { return fail_allocation ? nullptr : std::malloc(size); }
struct ImageDescription { struct { unsigned w = 320, h = 480; } header; };
using lv_img_dsc_t = ImageDescription;
void lv_image_set_src(lv_obj_t* o, const ImageDescription*) { assert(lock_depth); ++o->writes; }
void lv_image_set_scale(lv_obj_t*, unsigned) { assert(lock_depth); }
struct LvglImage { ImageDescription data; virtual ~LvglImage() = default; bool IsGif() { return false; } ImageDescription* image_dsc() { return &data; } };
struct LvglAllocatedImage : LvglImage {
    void* storage;
    LvglAllocatedImage(void* bytes, size_t, int, int, int, int) : storage(bytes) {}
    ~LvglAllocatedImage() { std::free(storage); }
};
struct LvglGif {
    unsigned stops = 0, starts = 0;
    LvglGif() = default;
    LvglGif(ImageDescription*, bool = false) {}
    void Stop() { ++stops; } void Start() { ++starts; }
    bool IsLoaded() { return true; }
    void SetFrameCallback(std::function<void()>) {}
    ImageDescription* image_dsc() { static ImageDescription image; return &image; }
};
bool use_glyph = false;
struct EmojiCollection { LvglImage image; LvglImage* GetEmojiImage(const char*) { return use_glyph ? nullptr : &image; } };
struct LvglTheme { EmojiCollection collection; EmojiCollection* emoji_collection() { return &collection; } };
const char* font_awesome_get_utf8(const char*) { return "face"; }
struct Timer { void (*callback)(void*) = nullptr; void* arg = nullptr; unsigned stops = 0; };
struct esp_timer_create_args_t { void (*callback)(void*); void* arg; int dispatch_method; const char* name; bool skip_unhandled_events; };
int esp_timer_create(const esp_timer_create_args_t* args, Timer** timer) { *timer = new Timer{args->callback,args->arg}; return 0; }
int esp_timer_stop(Timer* timer) { if (timer) ++timer->stops; return 0; }
int esp_timer_start_once(Timer*, int) { return 0; }
uint64_t esp_timer_get_time() { return 100; }
int esp_pm_lock_create(int, int, const char*, void**) { return 0; }
void esp_pm_lock_acquire(void*) {} void esp_pm_lock_release(void*) {}
enum DeviceState { kDeviceStateIdle, kDeviceStateStarting, kDeviceStateWifiConfiguring, kDeviceStateListening, kDeviceStateActivating };
namespace Lang { namespace Sounds { constexpr int OGG_LOW_BATTERY = 0; } }
struct Application {
    bool lesson = false; unsigned sounds = 0;
    static Application& GetInstance() { static Application app; return app; }
    bool IsLessonRuntimeActive() { return lesson; }
    DeviceState GetDeviceState() { return kDeviceStateListening; }
    void Schedule(std::function<void()> fn) { fn(); }
    void PlaySound(int) { ++sounds; }
};
struct Codec { int output_volume() { return 0; } };
struct Board {
    Codec codec; int battery = 70; bool battery_valid = true;
    static Board& GetInstance() { static Board board; return board; }
    Codec* GetAudioCodec() { return &codec; }
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
        assert(lock_depth == 0); level = battery; charging = false; discharging = true; return battery_valid;
    }
    const char* GetNetworkStateIcon() { assert(lock_depth == 0); return "wifi"; }
};
struct LvglDisplay {
    lv_obj_t objects[6];
    lv_obj_t *network_label_=&objects[0], *status_label_=&objects[1], *notification_label_=&objects[2],
        *mute_label_=&objects[3], *battery_label_=&objects[4], *low_battery_popup_=&objects[5];
    bool setup_ui_called_=true, muted_=false;
    std::atomic<bool> lesson_mode_active_{false};
    const char *battery_icon_=nullptr, *network_icon_=nullptr;
    Timer* notification_timer_=nullptr;
    void* pm_lock_=nullptr;
    std::chrono::system_clock::time_point last_status_update_time_;
    LvglDisplay();
    ~LvglDisplay() { delete notification_timer_; }
    void SetStatus(const char*);
    void ShowNotification(const char*, int);
    void UpdateStatusBar(bool);
};
struct LcdDisplay : LvglDisplay {
    void* display_=this;
    lv_obj_t surfaces[12];
    lv_obj_t *top_bar_=&surfaces[0], *status_bar_=&surfaces[1], *bottom_bar_=&surfaces[2],
        *emoji_box_=&surfaces[3], *emoji_image_=&surfaces[4], *emoji_label_=&surfaces[5],
        *lesson_focus_cue_=&surfaces[6], *preview_image_=&surfaces[7],
        *chat_message_label_=&surfaces[8], *lesson_caption_bar_=&surfaces[9],
        *lesson_caption_label_=&surfaces[10];
    lv_obj_t *lesson_background_=&surfaces[11], *container_=nullptr, *content_=nullptr;
    std::unique_ptr<LvglImage> lesson_cinematic_framebuffer_;
    std::uint16_t* lesson_cinematic_pixels_=nullptr;
    std::unique_ptr<LvglGif> gif_controller_=std::make_unique<LvglGif>();
    std::unique_ptr<LvglImage> preview_image_cached_;
    Timer preview_timer_storage;
    Timer* preview_timer_=&preview_timer_storage;
    bool hide_subtitle_=false;
    std::uint8_t lesson_chat_visibility_=0;
    bool lesson_caption_active_=false;
    int width_=320, height_=480;
    LvglTheme theme; LvglTheme* current_theme_=&theme;
    void SetLessonMode(bool); void SetEmotion(const char*); void SetChatMessage(const char*,const char*);
    void SetLessonCaption(const char*); void SetPreviewImage(std::unique_ptr<LvglImage>);
    void SetHideSubtitle(bool);
    void ClearChatMessages();
    void UpdateConversationFaceLayout() {}
    bool PresentLessonFramebuffer(const std::uint16_t*, std::uint16_t, std::uint16_t);
};
struct WechatLcdDisplay : LcdDisplay { void ClearChatMessages(); };
// PRODUCTION_METHODS

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "wechat_clear") {
        WechatLcdDisplay display;
        display.content_=&display.surfaces[11];
        display.SetLessonMode(true);
        display.ClearChatMessages();
        assert(display.emoji_label_->hidden);
        return 0;
    }
    LcdDisplay display;
    if (scenario == "subtitle_restore") {
        display.SetHideSubtitle(true);
        display.SetLessonMode(true);
        display.SetHideSubtitle(false);
        assert(display.bottom_bar_->hidden);
        display.SetLessonMode(false);
        assert(!display.bottom_bar_->hidden);
        return 0;
    }
    if (scenario == "status_race" || scenario == "notification_race" ||
        scenario == "chat_race" || scenario == "preview_race") {
        before_lock = [&] { display.SetLessonMode(true); };
        if (scenario == "status_race") display.SetStatus("late status");
        if (scenario == "notification_race") display.ShowNotification("late notification", 3000);
        if (scenario == "chat_race") display.SetChatMessage("assistant", "late caption");
        if (scenario == "preview_race") display.SetPreviewImage(std::make_unique<LvglImage>());
        assert(display.top_bar_->hidden && display.status_bar_->hidden && display.bottom_bar_->hidden);
        assert(display.status_label_->text == "old" && display.chat_message_label_->text == "old");
        assert(display.notification_label_->hidden && display.preview_image_->hidden);
        return 0;
    }
    if (scenario.rfind("present_", 0) == 0) {
        std::vector<std::uint16_t> pixels(320 * 480, 0x07e0);
        if (scenario == "present_invalid") {
            assert(!display.PresentLessonFramebuffer(pixels.data(), 319, 480));
            assert(!display.lesson_mode_active_ && !display.top_bar_->hidden);
        } else if (scenario == "present_allocation_failure") {
            fail_allocation=true;
            assert(!display.PresentLessonFramebuffer(pixels.data(), 320, 480));
            assert(!display.lesson_mode_active_ && !display.top_bar_->hidden);
        } else {
            assert(display.PresentLessonFramebuffer(pixels.data(), 320, 480));
            assert(display.lesson_mode_active_ && display.top_bar_->hidden && display.status_bar_->hidden);
            assert(!display.lesson_background_->hidden && display.lesson_cinematic_pixels_[0] == 0x07e0);
            const auto stops=display.gif_controller_->stops;
            const auto* storage=display.lesson_cinematic_pixels_;
            display.SetStatus("chat cannot claim between preparation and start");
            display.SetLessonMode(true);
            pixels[0]=0xf800;
            assert(display.PresentLessonFramebuffer(pixels.data(), 320, 480));
            assert(display.lesson_cinematic_pixels_ == storage && storage[0] == 0xf800);
            assert(display.gif_controller_->stops == stops && display.status_label_->text == "old");
        }
        return 0;
    }
    if (scenario == "emotion_race" || scenario == "glyph_race") {
        use_glyph = scenario == "glyph_race";
        before_lock = [&] { display.SetLessonMode(true); };
        display.SetEmotion("thinking");
        assert(display.emoji_box_->hidden && display.emoji_image_->hidden && display.emoji_label_->hidden);
        return 0;
    }
    display.SetLessonMode(true);
    if (scenario == "entry") {
        assert(display.top_bar_->hidden && display.status_bar_->hidden && display.bottom_bar_->hidden);
        assert(display.preview_image_->hidden && display.low_battery_popup_->hidden);
        assert(display.notification_timer_->stops && display.preview_timer_->stops);
    } else if (scenario == "chat") {
        display.bottom_bar_->hidden = true;
        display.SetChatMessage("assistant", "late chat");
        assert(display.bottom_bar_->hidden && display.chat_message_label_->text == "old");
    } else if (scenario == "status") {
        const auto writes = display.status_label_->writes;
        display.SetStatus("connecting");
        assert(display.status_label_->writes == writes);
    } else if (scenario == "notification") {
        display.notification_label_->hidden = true;
        display.ShowNotification("wifi", 3000);
        assert(display.notification_label_->hidden);
    } else if (scenario == "notification_timer") {
        display.status_label_->hidden = true;
        display.notification_timer_->callback(display.notification_timer_->arg);
        assert(display.status_label_->hidden);
    } else if (scenario == "network") {
        const auto writes = display.network_label_->writes;
        display.UpdateStatusBar(true);
        assert(display.network_label_->writes == writes);
        assert(display.mute_label_->text == "old");
    } else if (scenario == "preview" || scenario == "preview_expiry") {
        display.preview_image_->hidden = true;
        if (scenario == "preview") display.SetPreviewImage(std::make_unique<LvglImage>());
        else display.SetPreviewImage(nullptr);
        assert(display.preview_image_->hidden && display.emoji_box_->hidden);
    } else if (scenario == "subtitle") {
        display.bottom_bar_->hidden = true;
        display.SetHideSubtitle(false);
        assert(display.bottom_bar_->hidden);
    } else if (scenario == "clear_caption_fallback") {
        display.lesson_caption_bar_=nullptr;
        display.lesson_caption_label_=nullptr;
        display.SetLessonCaption("lesson caption");
        display.ClearChatMessages();
        assert(!display.bottom_bar_->hidden && display.chat_message_label_->text == "lesson caption");
    } else if (scenario == "hide_caption_fallback" || scenario == "hide_caption_dedicated") {
        auto* bar=display.lesson_caption_bar_;
        if (scenario == "hide_caption_fallback") {
            display.lesson_caption_bar_=nullptr;
            display.lesson_caption_label_=nullptr;
            bar=display.bottom_bar_;
        }
        display.SetLessonCaption("lesson caption");
        assert(!bar->hidden);
        display.SetHideSubtitle(true);
        assert(bar->hidden);
        display.SetHideSubtitle(false);
        assert(!bar->hidden);
    } else if (scenario == "idempotent") {
        const auto stops = display.gif_controller_->stops;
        display.SetLessonMode(true);
        assert(display.gif_controller_->stops == stops);
        display.SetLessonMode(false);
        const auto writes = display.emoji_box_->writes;
        display.SetLessonMode(false);
        assert(display.emoji_box_->writes == writes);
    } else if (scenario == "lesson_caption") {
        display.SetLessonCaption("lesson words");
        assert(!display.lesson_caption_bar_->hidden && display.lesson_caption_label_->text == "lesson words");
    } else if (scenario == "battery_policy") {
        Board::GetInstance().battery = 0;
        display.UpdateStatusBar(false);
        assert(display.low_battery_popup_->hidden && Application::GetInstance().sounds == 0);
        display.SetLessonMode(false);
        display.UpdateStatusBar(false);
        assert(!display.low_battery_popup_->hidden && Application::GetInstance().sounds == 1);
        display.UpdateStatusBar(false);
        assert(Application::GetInstance().sounds == 1);
    } else if (scenario == "chat_restored") {
        display.SetLessonMode(false);
        display.SetStatus("listening"); display.SetChatMessage("assistant", "chat");
        assert(!display.top_bar_->hidden && !display.status_bar_->hidden && !display.bottom_bar_->hidden);
        assert(!display.emoji_box_->hidden && display.status_label_->text == "listening");
    } else assert(false);
}
