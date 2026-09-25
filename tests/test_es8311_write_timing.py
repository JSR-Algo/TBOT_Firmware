from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_es8311_write_timing_native(tmp_path):
    compiler = shutil.which('clang++') or shutil.which('c++')
    assert compiler
    executable = tmp_path / 'es8311-write-timing'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    str(ROOT / 'tests/native/es8311_write_timing_test.cc'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_es8311_write_timing_stage_boundaries():
    source = (ROOT / 'main/audio/codecs/es8311_audio_codec.cc').read_text()
    body = source.split('int Es8311AudioCodec::Write(', 1)[1]
    markers = [
        'const int64_t prepare_start_us = esp_timer_get_time();',
        'std::vector<int16_t> stereo_data;',
        'const int64_t driver_start_us = esp_timer_get_time();',
        'esp_codec_dev_write(',
        'const int64_t driver_end_us = esp_timer_get_time();',
        'ESP_LOGI(TAG, "es8311_write count=',
        'ESP_ERROR_CHECK_WITHOUT_ABORT(ret);',
        'const int64_t log_end_us = esp_timer_get_time();',
        'write_timing_.Record(',
        'if (write_timing_.WindowReady())',
        'ESP_LOGI(TAG, "es8311_write_timing ',
        'write_timing_.ClearWindow();',
    ]
    positions = [body.index(marker) for marker in markers]
    assert positions == sorted(positions)
    assert 'driver_start_us - prepare_start_us' in body
    assert 'driver_end_us - driver_start_us' in body
    assert 'log_end_us - driver_end_us' in body
    assert 'write_count_ <= 5 || (write_count_ % 20) == 0 || ret != ESP_OK' in body
    assert '%lld' not in body and '%llu' not in body
