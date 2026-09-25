"""At the board's 100 Hz tick, capture must let its cleanup worker run."""
import subprocess
from pathlib import Path

from test_protocol_work_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def test_processor_blocks_for_a_tick_between_fetches(tmp_path):
    source = (ROOT / "main/audio/processors/afe_audio_processor.cc").read_text()
    fixture = r'''
#include <atomic>
#include <cassert>
#define ESP_LOGI(...) ((void)0)
constexpr int PROCESSOR_RUNNING=1, PROCESSOR_EXITED=2;
constexpr int pdFALSE=0, pdTRUE=1, portMAX_DELAY=-1;
int pdMS_TO_TICKS(int ms) { return ms / 10; }
void xEventGroupWaitBits(int,int,int,int,int) {}
void xEventGroupSetBits(int,int) {}
unsigned cleanup_runs=0;
void vTaskDelay(int ticks) { if(ticks>0) ++cleanup_runs; }
struct AfeAudioProcessor {
    std::atomic<bool> shutdown_{false};
    int event_group_=0;void* afe_data_=nullptr;
    struct Sdk { int get_fetch_chunksize(void*){return 512;} int get_feed_chunksize(void*){return 512;} } sdk;
    Sdk* afe_iface_=&sdk;
    unsigned fetches=0;
    void FetchAudio() { if(++fetches==3) shutdown_=true; }
    void AudioProcessorTask();
};
''' + method(source, "void AfeAudioProcessor::AudioProcessorTask") + r'''
int main(){AfeAudioProcessor app;app.AudioProcessorTask();assert(cleanup_runs==3);}
'''
    generated = tmp_path / "scheduler.cc"
    generated.write_text(fixture)
    binary = tmp_path / "scheduler"
    subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
