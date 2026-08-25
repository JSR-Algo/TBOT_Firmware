"""Regression locks for stereo-codec boards feeding ESP-SR AFE.

LCDWiki ES8311 exposes stereo I2S frames even though the physical microphone is
single-channel. ESP-SR AFE_TYPE_VC warns that multi-channel input selects the
first channel; if the mic lands on the other slot, wake word and voice input see
silence. These source-level tests lock the firmware to collapse non-reference
stereo input to one dominant mono channel before feeding AFE.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_wake_word_afe_downmixes_stereo_codec_to_dominant_mono_channel():
    header = read("main/audio/wake_words/afe_wake_word.h")
    source = read("main/audio/wake_words/afe_wake_word.cc")
    initialize = function_body(source, "bool AfeWakeWord::Initialize")
    feed = function_body(source, "void AfeWakeWord::Feed")

    assert "int afe_feed_channels_ = 1;" in header
    assert "int codec_input_channels_ = 1;" in header
    assert 'input_format = "M";' in initialize
    assert "codec_->input_reference()" in initialize
    assert "codec_input_channels_ = std::max(1, codec_->input_channels())" in initialize
    assert "SelectDominantMonoChannel(data, codec_input_channels_)" in feed
    assert "afe_iface_->get_feed_chunksize(afe_data_) * afe_feed_channels_" in feed

    observation = "telemetry_.ObserveFeedChunk(input_buffer_.data(), chunk_size,"
    submission = "afe_iface_->feed(afe_data_, input_buffer_.data())"
    assert observation in feed
    assert submission in feed
    assert feed.index("SelectDominantMonoChannel") < feed.index(submission)
    assert feed.index("input_buffer_.insert") < feed.index(submission)
    assert feed.index(submission) < feed.index(observation)
    assert feed.index(observation) < feed.index("input_buffer_.erase")
    assert "break;" not in feed


def test_voice_processor_afe_downmixes_stereo_codec_to_dominant_mono_channel():
    header = read("main/audio/processors/afe_audio_processor.h")
    source = read("main/audio/processors/afe_audio_processor.cc")
    initialize = function_body(source, "void AfeAudioProcessor::Initialize")
    feed = function_body(source, "void AfeAudioProcessor::Feed")

    assert "int afe_feed_channels_ = 1;" in header
    assert "int codec_input_channels_ = 1;" in header
    assert 'input_format = "M";' in initialize
    assert "codec_->input_reference()" in initialize
    assert "codec_input_channels_ = std::max(1, codec_->input_channels())" in initialize
    assert "SelectDominantMonoChannel(data, codec_input_channels_)" in feed
    assert "afe_iface_->get_feed_chunksize(afe_data_) * afe_feed_channels_" in feed
