// Real LVGL tree and RGB565 software flush; ESP platform services remain host adapters.
#include <lvgl.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
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
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define FONT_AWESOME_VOLUME_XMARK "mute"
#define FONT_AWESOME_BATTERY_BOLT "charge"
#define FONT_AWESOME_BATTERY_EMPTY "empty"
#define FONT_AWESOME_BATTERY_QUARTER "quarter"
#define FONT_AWESOME_BATTERY_HALF "half"
#define FONT_AWESOME_BATTERY_THREE_QUARTERS "threequarters"
#define FONT_AWESOME_BATTERY_FULL "full"
constexpr int kLessonCaptionBottomInsetDivisor=20;
std::recursive_mutex ui_mutex;
struct DisplayLockGuard {
    std::lock_guard<std::recursive_mutex> lock;
    template<class T> explicit DisplayLockGuard(T*) : lock(ui_mutex) {}
};
struct Timer { void (*callback)(void*); void* arg; };
struct esp_timer_create_args_t { void (*callback)(void*); void* arg; int dispatch_method; const char* name; bool skip_unhandled_events; };
int esp_timer_create(const esp_timer_create_args_t* args, Timer** timer) { *timer=new Timer{args->callback,args->arg}; return 0; }
int esp_timer_stop(Timer*) { return 0; }
int esp_timer_start_once(Timer*, int) { return 0; }
uint64_t esp_timer_get_time() { return 100; }
int esp_pm_lock_create(int, int, const char*, void**) { return 0; }
void esp_pm_lock_acquire(void*) {} void esp_pm_lock_release(void*) {}
enum DeviceState { kDeviceStateIdle, kDeviceStateStarting, kDeviceStateWifiConfiguring, kDeviceStateListening, kDeviceStateActivating };
namespace Lang { namespace Sounds { constexpr int OGG_LOW_BATTERY=0; } }
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    bool IsLessonRuntimeActive() { return false; }
    DeviceState GetDeviceState() { return kDeviceStateListening; }
    void Schedule(std::function<void()> fn) { fn(); }
    void PlaySound(int) {}
};
struct Codec { int output_volume() { return 0; } };
struct Board {
    Codec codec;
    static Board& GetInstance() { static Board board; return board; }
    Codec* GetAudioCodec() { return &codec; }
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
        level=70; charging=false; discharging=true; return true;
    }
    const char* GetNetworkStateIcon() { return "wifi"; }
};
struct LvglImage {
    lv_image_dsc_t data{};
    virtual ~LvglImage()=default;
    bool IsGif() { return false; }
    const lv_image_dsc_t* image_dsc() { return &data; }
};
struct LvglAllocatedImage : LvglImage {
    void* storage;
    LvglAllocatedImage(void* bytes, size_t size, int w, int h, int stride, lv_color_format_t format) : storage(bytes) {
        data.header.magic=LV_IMAGE_HEADER_MAGIC; data.header.cf=format;
        data.header.w=w; data.header.h=h; data.header.stride=stride;
        data.data_size=size; data.data=static_cast<const uint8_t*>(bytes);
    }
    ~LvglAllocatedImage() { std::free(storage); }
};
void* heap_caps_malloc(size_t bytes, int) { return std::malloc(bytes); }
int LessonImageCoverScale(int, int, int, int) { return 256; }
struct LvglGif {
    explicit LvglGif(const lv_image_dsc_t*, bool=false) {}
    void Stop() {} void Start() {} bool IsLoaded() { return false; }
    void SetFrameCallback(std::function<void()>) {}
    const lv_image_dsc_t* image_dsc() { return nullptr; }
};
struct EmojiCollection { LvglImage* GetEmojiImage(const char*) { return nullptr; } };
struct LvglTheme { EmojiCollection collection; EmojiCollection* emoji_collection() { return &collection; } };
const char* font_awesome_get_utf8(const char*) { return "FACE"; }
struct LvglDisplay {
    lv_obj_t *network_label_=nullptr,*status_label_=nullptr,*notification_label_=nullptr,
        *mute_label_=nullptr,*battery_label_=nullptr,*low_battery_popup_=nullptr;
    std::atomic<bool> lesson_mode_active_{false};
    bool setup_ui_called_=true,muted_=false;
    const char *battery_icon_=nullptr,*network_icon_=nullptr;
    Timer* notification_timer_=nullptr;
    void* pm_lock_=nullptr;
    std::chrono::system_clock::time_point last_status_update_time_;
    LvglDisplay(); ~LvglDisplay() { delete notification_timer_; }
    void SetStatus(const char*); void ShowNotification(const char*,int); void UpdateStatusBar(bool);
};
struct LcdDisplay : LvglDisplay {
    lv_display_t* display_=nullptr;
    lv_obj_t *top_bar_=nullptr,*status_bar_=nullptr,*bottom_bar_=nullptr,*emoji_box_=nullptr,
        *emoji_image_=nullptr,*emoji_label_=nullptr,*lesson_focus_cue_=nullptr,*preview_image_=nullptr,
        *chat_message_label_=nullptr,*lesson_caption_bar_=nullptr,*lesson_caption_label_=nullptr,
        *lesson_background_=nullptr,*container_=nullptr,*content_=nullptr;
    std::unique_ptr<LvglGif> gif_controller_;
    std::unique_ptr<LvglImage> preview_image_cached_,lesson_cinematic_framebuffer_;
    std::uint16_t* lesson_cinematic_pixels_=nullptr;
    Timer* preview_timer_=nullptr;
    std::uint8_t lesson_chat_visibility_=0;
    bool lesson_caption_active_=false;
    bool hide_subtitle_=false;
    int width_=320,height_=480;
    LvglTheme theme; LvglTheme* current_theme_=&theme;
    void SetLessonMode(bool); void SetEmotion(const char*); void SetChatMessage(const char*,const char*);
    void SetLessonCaption(const char*); void SetPreviewImage(std::unique_ptr<LvglImage>);
    void SetHideSubtitle(bool); void UpdateConversationFaceLayout() {}
    void ClearChatMessages();
    bool PresentLessonFramebuffer(const std::uint16_t*,std::uint16_t,std::uint16_t);
};
// PRODUCTION_METHODS
std::vector<uint16_t> captured(320*480), draw_buffer(320*480);
unsigned flushes=0;
lv_obj_t* RedBar(lv_obj_t* screen,int y,int h) {
    auto* bar=lv_obj_create(screen); lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar,0,y); lv_obj_set_size(bar,320,h);
    lv_obj_set_style_bg_color(bar,lv_color_hex(0xff0000),0);
    lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);
    return bar;
}
void Save(const char* name) {
    FILE* file=std::fopen(name,"wb"); assert(file);
    std::fprintf(file,"P6\n320 480\n255\n");
    for(auto pixel:captured) {
        const uint8_t rgb[]={static_cast<uint8_t>(((pixel>>11)&31)*255/31),
            static_cast<uint8_t>(((pixel>>5)&63)*255/63),static_cast<uint8_t>((pixel&31)*255/31)};
        assert(std::fwrite(rgb,1,3,file)==3);
    }
    assert(std::fclose(file)==0);
}
int main() {
    lv_init();
    LcdDisplay display;
    display.display_=lv_display_create(320,480);
    lv_display_set_color_format(display.display_,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display.display_,draw_buffer.data(),nullptr,draw_buffer.size()*2,LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display.display_,[](lv_display_t* owner,const lv_area_t* area,uint8_t* pixels) {
        assert(area->x1==0 && area->y1==0 && area->x2==319 && area->y2==479);
        std::memcpy(captured.data(),pixels,captured.size()*2); ++flushes; lv_display_flush_ready(owner);
    });
    auto* screen=lv_display_get_screen_active(display.display_);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x00ff00),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    display.lesson_background_=lv_image_create(screen);
    display.top_bar_=RedBar(screen,0,60); display.status_bar_=RedBar(screen,60,60);
    display.bottom_bar_=RedBar(screen,420,60);
    display.network_label_=lv_label_create(display.top_bar_);
    display.mute_label_=lv_label_create(display.top_bar_); display.battery_label_=lv_label_create(display.top_bar_);
    display.status_label_=lv_label_create(display.status_bar_);
    display.notification_label_=lv_label_create(display.status_bar_);
    display.chat_message_label_=lv_label_create(display.bottom_bar_);
    display.emoji_box_=RedBar(screen,120,100); display.emoji_label_=lv_label_create(display.emoji_box_);
    display.emoji_image_=lv_image_create(display.emoji_box_);
    display.preview_image_=lv_image_create(screen);
    display.low_battery_popup_=RedBar(screen,360,60);
    display.SetStatus("chat ready"); display.SetChatMessage("assistant","chat caption");
    lv_refr_now(display.display_); Save("before.ppm");
    assert(captured[30*320+300]==0xf800 && captured[450*320+300]==0xf800);
    std::vector<uint16_t> lesson(320*480,0x07e0);
    assert(display.PresentLessonFramebuffer(lesson.data(),320,480));
    display.SetStatus("WIFI CONNECTING"); display.ShowNotification("wifi lost",3000);
    display.UpdateStatusBar(true); display.SetChatMessage("assistant","late chat");
    display.SetEmotion("thinking"); display.SetPreviewImage(nullptr); display.SetHideSubtitle(false);
    display.notification_timer_->callback(display.notification_timer_->arg);
    lv_refr_now(display.display_); Save("during.ppm");
    assert(std::all_of(captured.begin(),captured.end(),[](uint16_t pixel){return pixel==0x07e0;}));
    display.SetLessonCaption("LESSON CAPTION");
    assert(!lv_obj_has_flag(display.bottom_bar_,LV_OBJ_FLAG_HIDDEN));
    display.ClearChatMessages();
    assert(std::strcmp(lv_label_get_text(display.chat_message_label_),"LESSON CAPTION")==0);
    display.SetHideSubtitle(true); assert(lv_obj_has_flag(display.bottom_bar_,LV_OBJ_FLAG_HIDDEN));
    display.SetHideSubtitle(false); assert(!lv_obj_has_flag(display.bottom_bar_,LV_OBJ_FLAG_HIDDEN));
    lv_refr_now(display.display_); Save("lesson-caption.ppm");
    assert(std::any_of(captured.begin(),captured.end(),[](uint16_t pixel){return pixel!=0x07e0;}));
    display.SetLessonCaption("");
    display.SetLessonMode(false); display.SetLessonMode(false);
    display.SetStatus("chat returned"); display.SetChatMessage("assistant","chat returned");
    lv_refr_now(display.display_); Save("after.ppm");
    assert(captured[30*320+300]==0xf800 && captured[450*320+300]==0xf800);
    std::printf("PASS real LVGL %d.%d.%d: before/during/caption/after, 153600 lesson pixels, %u flushes\n",
        LVGL_VERSION_MAJOR,LVGL_VERSION_MINOR,LVGL_VERSION_PATCH,flushes);
    lv_display_delete(display.display_); lv_deinit();
}
