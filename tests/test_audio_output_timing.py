from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_output_timing_native(tmp_path):
    compiler = shutil.which('clang++') or shutil.which('c++')
    assert compiler
    executable = tmp_path / 'output-timing'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    str(ROOT / 'tests/native/audio_output_timing_test.cc'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_output_timing_records_outside_queue_lock():
    source = (ROOT / 'main/audio/audio_service.cc').read_text()
    body = source.split('void AudioService::AudioOutputTask()', 1)[1]
    body = body.split('void AudioService::OpusCodecTask()', 1)[0]
    assert body.index('codec_->OutputData(task->pcm);') < body.index('output_timing.Record(')
    assert body.index('output_timing.Record(') < body.index('lock.lock();')
    assert 'output_timing.frames >= 100' in body
