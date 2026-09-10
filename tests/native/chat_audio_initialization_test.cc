#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <vector>

constexpr int AFE_TYPE_VC=0, AFE_MODE_HIGH_PERF=0, AEC_MODE_VOIP_HIGH_PERF=0;
constexpr int VAD_MODE_0=0, AFE_NS_MODE_NET=0, AFE_MEMORY_ALLOC_MORE_PSRAM=0;
constexpr int tskIDLE_PRIORITY=0, pdPASS=1, pdFALSE=0, pdTRUE=1, portMAX_DELAY=-1;
constexpr int PROCESSOR_RUNNING=1, PROCESSOR_EXITED=2;
const char* ESP_NSNET_PREFIX="ns";
const char* ESP_VADN_PREFIX="vad";
struct AudioCodec { int input_channels() { return 1; } bool input_reference() { return false; } };
struct srmodel_list_t {};
struct afe_config_t {
    int aec_mode=0, vad_mode=0, vad_min_noise_ms=0, afe_ns_mode=0, memory_alloc_mode=0;
    bool ns_init=false, agc_init=false, aec_init=false, vad_init=false;
    char* vad_model_name=nullptr;
    char* ns_model_name=nullptr;
};
static int fail_stage=0, task_attempts=0, created_data=0, destroyed_data=0, config_count=0;
srmodel_list_t* esp_srmodel_init(const char*) { static srmodel_list_t models; return &models; }
char* esp_srmodel_filter(srmodel_list_t*, const char*, void*) { return nullptr; }
afe_config_t* afe_config_init(const char*, void*, int, int) {
    if (fail_stage==2) return nullptr;
    ++config_count; return new afe_config_t;
}
void afe_config_free(afe_config_t* value) { --config_count; delete value; }
struct Sdk {
    void* create_from_config(afe_config_t*) { if (fail_stage==4) return nullptr; ++created_data; return this; }
    void destroy(void*) { ++destroyed_data; }
    void reset_buffer(void*) {}
};
Sdk* esp_afe_handle_from_config(afe_config_t*) { static Sdk sdk; return fail_stage==3 ? nullptr : &sdk; }
int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) {
    ++task_attempts; return fail_stage==5 ? 0 : 1;
}
void vTaskDelete(void*) {}
void xEventGroupSetBits(int* group, int bits) { assert(group); *group |= bits; }
void xEventGroupClearBits(int* group, int bits) { assert(group); *group &= ~bits; }
int xEventGroupGetBits(int* group) { assert(group); return *group; }
void xEventGroupWaitBits(int*, int, int, int, int) {}
void vEventGroupDelete(int*) {}
class AfeAudioProcessor {
public:
    int bits=0;
    int* event_group_ = &bits;
    Sdk* afe_iface_=nullptr;
    void* afe_data_=nullptr;
    AudioCodec* codec_=nullptr;
    int frame_samples_=0, codec_input_channels_=0, afe_feed_channels_=0;
    std::vector<int16_t> input_buffer_, output_buffer_;
    std::mutex callback_mutex_, input_buffer_mutex_, fetch_mutex_;
    std::atomic<bool> shutdown_{false};
    bool task_created_=false, initialization_attempted_=false;
    void Initialize(AudioCodec*, int, srmodel_list_t*);
    ~AfeAudioProcessor();
    void AudioProcessorTask() {}
    void Start();
    void Stop();
    bool IsRunning();
    // PRODUCTION_READINESS
};
// PRODUCTION_METHODS
int main() {
    AudioCodec codec;
    for (int stage=1; stage<=5; ++stage) {
        fail_stage=stage;
        const auto attempts=task_attempts;
        {
            AfeAudioProcessor processor;
            if (stage==1) processor.event_group_=nullptr;
            processor.Initialize(&codec, 60, nullptr);
            assert(!processor.IsCaptureReady());
            processor.Start(); assert(!processor.IsRunning());
            processor.Stop();
            fail_stage=0;
            processor.Initialize(&codec, 60, nullptr);
            assert(!processor.IsCaptureReady());
            assert(task_attempts==attempts+(stage==5 ? 1 : 0));
        }
        assert(config_count==0 && created_data==destroyed_data);
    }
    fail_stage=0;
    {
        AfeAudioProcessor ready;
        ready.Initialize(&codec, 60, nullptr);
        assert(ready.IsCaptureReady()); ready.Start(); assert(ready.IsRunning()); ready.Stop();
    }
    assert(created_data==destroyed_data && config_count==0);
}
