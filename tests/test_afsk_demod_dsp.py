"""Static source-shape regression locks over the AFSK demodulation DSP layer in
main/boards/common/afsk_demod.cc.

WHY STATIC, AND WHAT THIS DOES / DOES NOT PROVE
-----------------------------------------------
This repo has NO host-compiled C++ harness for afsk_demod.cc: the .cc pulls in
esp_log.h / display.h / ssid_manager.h / wifi_manager.h / application.h and the
FreeRTOS vTaskDelay/pdMS_TO_TICKS macros, none of which are available off-device.
So these tests CANNOT execute the Goertzel filter and assert a computed amplitude
for a known tone, nor run the state machine on a synthetic bit stream and assert a
decoded string. True DSP value-testing (e.g. feed a 1800 Hz sine, assert the mark
probability > 0.5) requires a host-compiled / mocked-ESP harness that does not
exist here.

Instead each test is a STRUCTURAL lock: it asserts the SHAPE of the algorithm in
source -- the presence and arrangement of the load-bearing expressions, the
ordering of branches, and the exact constants/markers -- so an accidental edit
that breaks the math (wrong coefficient, dropped branch, flipped comparison,
missing reset, wrong bit order) is caught as a source regression. These are locks
on STRUCTURE/BRANCHES, NOT on computed numeric values.

The scanner mirrors the convention in tests/test_afsk_credential_redaction.py and
tests/test_blufi_provisioning_stability.py: module-level ROOT, a read(path)
helper, comment stripping, and STATEMENT-oriented (not line-oriented) collapsing
so multi-line expressions are visible to substring/regex checks. test_harness_*
guard the scanner itself.

Coverage map (task round-2 gap fill):
  FrequencyDetector  -> Goertzel: filter_coefficient_ = 2*cos, state_buffer
                        rotation (S[-2]/S[-1]/S[0]), amplitude sqrt(re^2+im^2)
                        / (window/2).
  AudioSignalProcessor -> samples_per_bit_ = sample_rate/bit_rate, the
                        sample_rate % bit_rate ESP_LOGW warning branch, window
                        sliding + output_sample_count_ gating + per-window reset.
  AudioDataBuffer    -> state machine kInactive->kWaiting->kReceiving, start/end
                        pattern compare, overflow reset, checksum sum-of-bytes +
                        'Data too short' + mismatch reject, ConvertBitsToBytes
                        MSB-first + integer-truncation.
  ReceiveWifiCredentialsFromAudio -> not-in-config sleep gate, 2ch->mono
                        downmix, no-newline 'Invalid data format' branch.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

AFSK_SRC = "main/boards/common/afsk_demod.cc"
AFSK_HDR = "main/boards/common/afsk_demod.h"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# Scanner helpers (statement-oriented, robust to multi-line expressions).
# ---------------------------------------------------------------------------
def _strip_comments(source: str) -> str:
    """Remove C/C++ comments so commented-out code / words in prose never match."""
    no_block = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", no_block)


def _normalize(source: str) -> str:
    """Comment-stripped, whitespace-collapsed single-line view of the source.

    Lets a substring/regex match an expression that spans several physical lines
    (e.g. a multi-line return or ESP_LOGW) without the scan being blind to the
    continuation lines.
    """
    return re.sub(r"\s+", " ", _strip_comments(source)).strip()


def _statements(source: str):
    """Logical C++ statements/fragments, robust to multi-line expressions.

    Comments are stripped first (newlines still present so // works), string
    literal contents are blanked (so ; { } inside a literal can't mis-split, and
    format-string text never matches a code token), whitespace is collapsed, and
    the text is split on statement/block boundaries (; { }).
    """
    no_comments = _strip_comments(source)
    blanked = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', no_comments)
    collapsed = re.sub(r"\s+", " ", blanked)
    return [s.strip() for s in re.split(r"[;{}]", collapsed) if s.strip()]


def _function_body(source: str, signature_substr: str) -> str:
    """Return the brace-balanced body of the first function/method whose text
    contains `signature_substr`, on the comment-stripped source. Lets a test
    scope its assertions to one routine so a marker found in a *different*
    function can't falsely satisfy it.
    """
    src = _strip_comments(source)
    idx = src.find(signature_substr)
    assert idx != -1, f"signature not found: {signature_substr!r}"
    brace = src.find("{", idx)
    assert brace != -1, f"no opening brace after {signature_substr!r}"
    depth = 0
    for pos in range(brace, len(src)):
        ch = src[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace : pos + 1]
    raise AssertionError(f"unbalanced braces for {signature_substr!r}")


# ---------------------------------------------------------------------------
# Sanity: the source / header files exist and are non-trivial.
# ---------------------------------------------------------------------------
def test_source_and_header_present_and_nonempty():
    src = read(AFSK_SRC)
    hdr = read(AFSK_HDR)
    assert len(src) > 2000, "afsk_demod.cc unexpectedly small"
    assert "namespace audio_wifi_config" in src
    assert "namespace audio_wifi_config" in hdr


# ===========================================================================
# FrequencyDetector -- Goertzel single-tone detector.
# ===========================================================================
def test_freqdetector_filter_coefficient_is_two_cos():
    """Goertzel recurrence coefficient must be 2*cos(w), where cos comes from the
    angular frequency 2*pi*f. A wrong coefficient silently destroys detection."""
    ctor = _function_body(read(AFSK_SRC), "FrequencyDetector::FrequencyDetector")

    # angular_frequency_ = 2*pi*frequency_
    assert re.search(
        r"angular_frequency_\s*=\s*2\.0f\s*\*\s*M_PI\s*\*\s*frequency_", ctor
    ), "angular frequency must be 2*pi*f"
    # cos/sin computed from the angular frequency
    assert re.search(r"cos_coefficient_\s*=\s*std::cos\(\s*angular_frequency_\s*\)", ctor)
    assert re.search(r"sin_coefficient_\s*=\s*std::sin\(\s*angular_frequency_\s*\)", ctor)
    # the load-bearing Goertzel coefficient: 2 * cos(w)
    assert re.search(
        r"filter_coefficient_\s*=\s*2\.0f\s*\*\s*cos_coefficient_", ctor
    ), "filter_coefficient_ must be 2*cos(w)"


def test_freqdetector_ctor_seeds_two_zero_states():
    """State buffer holds S[-1], S[-2]; it must start with exactly two zeros."""
    ctor = _function_body(read(AFSK_SRC), "FrequencyDetector::FrequencyDetector")
    pushes = re.findall(r"state_buffer_\.push_back\(\s*0\.0f\s*\)", ctor)
    assert len(pushes) == 2, f"ctor must seed two zero states, found {len(pushes)}"


def test_freqdetector_reset_clears_then_reseeds_two_zeros():
    """Reset must clear and re-seed exactly two zero states (per-window reuse)."""
    reset = _function_body(read(AFSK_SRC), "FrequencyDetector::Reset")
    assert "state_buffer_.clear()" in reset
    pushes = re.findall(r"state_buffer_\.push_back\(\s*0\.0f\s*\)", reset)
    assert len(pushes) == 2, f"Reset must re-seed two zero states, found {len(pushes)}"


def test_freqdetector_process_sample_guards_underfull_state():
    """ProcessSample must early-return when fewer than 2 states are buffered,
    otherwise front()/pop_front() would underflow the deque."""
    proc = _function_body(read(AFSK_SRC), "FrequencyDetector::ProcessSample")
    assert re.search(r"if\s*\(\s*state_buffer_\.size\(\)\s*<\s*2\s*\)", proc)
    # the guard returns before touching the deque
    guard_then = proc[: proc.find("s_minus_2")]
    assert "return" in guard_then, "size<2 guard must return before deque access"


def test_freqdetector_process_sample_recurrence_and_rotation():
    """The Goertzel recurrence and state rotation must be exact:
        s_current = sample + filter_coefficient_*S[-1] - S[-2]
    with S[-2] popped first, S[-1] popped second, then S[-1] and S[0] pushed back.
    A flipped sign or wrong pop order breaks detection silently."""
    proc = _function_body(read(AFSK_SRC), "FrequencyDetector::ProcessSample")

    # S[-2] is the older front, popped first; S[-1] is popped second.
    assert re.search(r"s_minus_2\s*=\s*state_buffer_\.front\(\)", proc)
    assert re.search(r"s_minus_1\s*=\s*state_buffer_\.front\(\)", proc)
    # exactly two pop_front (consume S[-2] and S[-1])
    assert len(re.findall(r"state_buffer_\.pop_front\(\)", proc)) == 2

    # the recurrence: sample + filter_coefficient_ * s_minus_1 - s_minus_2
    assert re.search(
        r"s_current\s*=\s*sample\s*\+\s*filter_coefficient_\s*\*\s*s_minus_1\s*-\s*s_minus_2",
        proc,
    ), "Goertzel recurrence shape is wrong"

    # push order: S[-1] back first, then the new S[0]
    body = re.sub(r"\s+", " ", proc)
    push_minus1 = body.find("push_back(s_minus_1)")
    push_current = body.find("push_back(s_current)")
    assert push_minus1 != -1 and push_current != -1
    assert push_minus1 < push_current, "must push S[-1] before new S[0]"


def test_freqdetector_amplitude_is_complex_magnitude_over_half_window():
    """Amplitude = sqrt(real^2 + imag^2) / (window_size_/2), with
        real = cos*S[-1] - S[-2]
        imag = sin*S[-1]
    The /(window/2) normalization and the sqrt-of-sum-of-squares are the locks."""
    amp = _function_body(read(AFSK_SRC), "FrequencyDetector::GetAmplitude")

    # under-full state returns 0
    assert re.search(r"if\s*\(\s*state_buffer_\.size\(\)\s*<\s*2\s*\)", amp)
    assert re.search(r"return\s+0\.0f", amp)

    # real / imaginary parts
    assert re.search(
        r"real_part\s*=\s*cos_coefficient_\s*\*\s*s_minus_1\s*-\s*s_minus_2", amp
    )
    assert re.search(r"imaginary_part\s*=\s*sin_coefficient_\s*\*\s*s_minus_1", amp)

    # magnitude / (window_size_ / 2.0f), collapsed across the multi-line return
    flat = re.sub(r"\s+", " ", amp)
    assert re.search(
        r"return\s+std::sqrt\(\s*real_part\s*\*\s*real_part\s*\+\s*imaginary_part\s*\*\s*imaginary_part\s*\)\s*/\s*\(\s*static_cast<float>\(window_size_\)\s*/\s*2\.0f\s*\)",
        flat,
    ), "amplitude must be sqrt(re^2+im^2)/(window/2)"


def test_freqdetector_indices_distinguish_minus1_and_minus2():
    """GetAmplitude reads S[-1] from index 1 and S[-2] from index 0. Swapping
    these indices would compute the wrong phase."""
    amp = _function_body(read(AFSK_SRC), "FrequencyDetector::GetAmplitude")
    assert re.search(r"s_minus_1\s*=\s*state_buffer_\[\s*1\s*\]", amp)
    assert re.search(r"s_minus_2\s*=\s*state_buffer_\[\s*0\s*\]", amp)


# ===========================================================================
# AudioSignalProcessor -- Mark/Space pair, windowing, bit-rate gating.
# ===========================================================================
def test_signalprocessor_samples_per_bit_is_rate_over_bitrate():
    """samples_per_bit_ = sample_rate / bit_rate gates one probability emission
    per bit period. Wrong divisor would emit at the wrong cadence."""
    ctor = _function_body(read(AFSK_SRC), "AudioSignalProcessor::AudioSignalProcessor")
    assert re.search(r"samples_per_bit_\s*=\s*sample_rate\s*/\s*bit_rate", ctor)


def test_signalprocessor_warns_when_rate_not_divisible_by_bitrate():
    """If sample_rate % bit_rate != 0 the integer samples_per_bit_ is lossy; the
    ctor must emit a divisibility warning (it does not abort -- ESP keeps running)."""
    ctor = _function_body(read(AFSK_SRC), "AudioSignalProcessor::AudioSignalProcessor")
    assert re.search(r"if\s*\(\s*sample_rate\s*%\s*bit_rate\s*!=\s*0\s*\)", ctor), (
        "missing sample_rate % bit_rate guard"
    )
    # the warning is ESP_LOGW (warning level, visible) inside that branch
    flat = re.sub(r"\s+", " ", ctor)
    guard = re.search(r"if\s*\(\s*sample_rate\s*%\s*bit_rate\s*!=\s*0\s*\)\s*\{(.*?)\}", flat)
    assert guard, "divisibility guard body not found"
    assert "ESP_LOGW" in guard.group(1), "non-divisible case must ESP_LOGW"
    # and it is a warning, not a hard failure / return
    assert "return" not in guard.group(1), "warning branch must not abort"


def test_signalprocessor_normalizes_mark_and_space_frequencies():
    """Detectors are built with normalized freq = f / sample_rate (Goertzel
    expects a normalized frequency, not the raw Hz value)."""
    ctor = _function_body(read(AFSK_SRC), "AudioSignalProcessor::AudioSignalProcessor")
    assert re.search(
        r"normalized_mark_freq\s*=\s*static_cast<float>\(mark_frequency\)\s*/\s*static_cast<float>\(sample_rate\)",
        ctor,
    )
    assert re.search(
        r"normalized_space_freq\s*=\s*static_cast<float>\(space_frequency\)\s*/\s*static_cast<float>\(sample_rate\)",
        ctor,
    )
    assert "mark_detector_ = std::make_unique<FrequencyDetector>(normalized_mark_freq, window_size)" in re.sub(r"\s+", " ", ctor)
    assert "space_detector_ = std::make_unique<FrequencyDetector>(normalized_space_freq, window_size)" in re.sub(r"\s+", " ", ctor)


def test_signalprocessor_window_slides_when_full():
    """A full input buffer must pop the oldest and push the newest (sliding
    window), and increment the output counter; an under-full buffer just fills."""
    proc = _function_body(read(AFSK_SRC), "AudioSignalProcessor::ProcessAudioSamples")
    flat = re.sub(r"\s+", " ", proc)
    # under-full branch: only push, no pop
    assert re.search(r"if\s*\(\s*input_buffer_\.size\(\)\s*<\s*input_buffer_size_\s*\)", flat)
    # full branch: pop_front then push_back, count++
    assert "input_buffer_.pop_front()" in flat
    assert "input_buffer_.push_back(sample)" in flat
    assert "output_sample_count_++" in flat
    # The under-full branch ALSO push_back(sample) (without a pop), so the
    # ordering check must be scoped to the full (else) branch -- the slice from
    # the pop_front onward. Within that slice, pop precedes the matching push.
    full_branch = flat[flat.find("input_buffer_.pop_front()"):]
    assert full_branch.find("input_buffer_.pop_front()") < full_branch.find("input_buffer_.push_back(sample)"), (
        "in the full branch, pop_front (oldest out) must precede push_back (newest in)"
    )
    # the counter increment lives in the full branch (after the slide)
    assert "output_sample_count_++" in full_branch


def test_signalprocessor_emits_one_probability_per_bit_period():
    """Probability is emitted only when output_sample_count_ >= samples_per_bit_,
    and the counter resets to 0 afterward (one emission per bit period)."""
    proc = _function_body(read(AFSK_SRC), "AudioSignalProcessor::ProcessAudioSamples")
    flat = re.sub(r"\s+", " ", proc)
    assert re.search(r"if\s*\(\s*output_sample_count_\s*>=\s*samples_per_bit_\s*\)", flat)
    assert "result.push_back(mark_probability)" in flat
    assert re.search(r"output_sample_count_\s*=\s*0", flat), "counter must reset to 0"
    # reset of counter occurs after the probability push (inside the emit branch)
    assert flat.find("result.push_back(mark_probability)") < flat.rfind("output_sample_count_ = 0")


def test_signalprocessor_runs_goertzel_over_whole_window_and_resets_detectors():
    """On emit, every buffered sample is fed to BOTH detectors, amplitudes are
    read, and both detectors are Reset() before the next window."""
    proc = _function_body(read(AFSK_SRC), "AudioSignalProcessor::ProcessAudioSamples")
    flat = re.sub(r"\s+", " ", proc)
    assert "mark_detector_->ProcessSample(window_sample)" in flat
    assert "space_detector_->ProcessSample(window_sample)" in flat
    assert re.search(r"mark_amplitude\s*=\s*mark_detector_->GetAmplitude\(\)", flat)
    assert re.search(r"space_amplitude\s*=\s*space_detector_->GetAmplitude\(\)", flat)
    assert "mark_detector_->Reset()" in flat
    assert "space_detector_->Reset()" in flat


def test_signalprocessor_probability_normalized_with_epsilon():
    """mark_probability = mark / (space + mark + epsilon): the epsilon term is the
    division-by-zero guard for a silent window. Dropping it is a divide-by-zero."""
    proc = _function_body(read(AFSK_SRC), "AudioSignalProcessor::ProcessAudioSamples")
    flat = re.sub(r"\s+", " ", proc)
    assert re.search(
        r"mark_probability\s*=\s*mark_amplitude\s*/\s*\(\s*space_amplitude\s*\+\s*mark_amplitude\s*\+\s*std::numeric_limits<float>::epsilon\(\)\s*\)",
        flat,
    ), "probability normalization (with epsilon guard) is wrong"


# ===========================================================================
# AudioDataBuffer -- state machine, patterns, checksum, bit packing.
# ===========================================================================
def test_databuffer_state_enum_order_and_default_state():
    """State machine has exactly kInactive/kWaiting/kReceiving, and the buffer
    starts in kInactive."""
    hdr = _normalize(read(AFSK_HDR))
    assert re.search(r"enum\s+class\s+DataReceptionState\s*\{[^}]*kInactive[^}]*kWaiting[^}]*kReceiving[^}]*\}", hdr), (
        "state enum order kInactive->kWaiting->kReceiving missing"
    )
    # default ctor seeds current_state_ to kInactive
    default_ctor = read(AFSK_SRC)
    assert "current_state_(DataReceptionState::kInactive)" in default_ctor


def test_databuffer_default_start_end_patterns_exact_bits():
    """Default SOT \\x01\\x02 and EOT \\x03\\x04 patterns must be the exact 16-bit
    sequences; an off-by-one bit means start/end is never detected."""
    src = _normalize(read(AFSK_SRC))
    # \x01\x02 = 00000001 00000010
    assert re.search(
        r"kDefaultStartTransmissionPattern\s*=\s*\{\s*0,\s*0,\s*0,\s*0,\s*0,\s*0,\s*0,\s*1,\s*0,\s*0,\s*0,\s*0,\s*0,\s*0,\s*1,\s*0\s*\}",
        src,
    ), "start pattern bits wrong"
    # \x03\x04 = 00000011 00000100
    assert re.search(
        r"kDefaultEndTransmissionPattern\s*=\s*\{\s*0,\s*0,\s*0,\s*0,\s*0,\s*0,\s*1,\s*1,\s*0,\s*0,\s*0,\s*0,\s*0,\s*1,\s*0,\s*0\s*\}",
        src,
    ), "end pattern bits wrong"


def test_databuffer_default_max_bit_buffer_is_776():
    """Default bit buffer is 776 bits = (32+1+63+1)*8 (SSID+sep+pw+checksum)."""
    src = _normalize(read(AFSK_SRC))
    assert re.search(r"max_bit_buffer_size_\s*=\s*776", src)


def test_databuffer_custom_ctor_max_bits_is_bytes_times_8():
    """Custom ctor sizes the bit buffer as max_byte_size * 8."""
    src = _normalize(read(AFSK_SRC))
    assert re.search(r"max_bit_buffer_size_\s*=\s*max_byte_size\s*\*\s*8", src)


def test_databuffer_identifier_buffer_sized_to_longest_pattern():
    """Identifier window is max(start.size, end.size) so either pattern fits."""
    src = _normalize(read(AFSK_SRC))
    assert re.search(
        r"identifier_buffer_size_\s*=\s*std::max\(\s*start_of_transmission_\.size\(\)\s*,\s*end_of_transmission_\.size\(\)\s*\)",
        src,
    )


def test_databuffer_threshold_decides_bit():
    """Bit is 1 iff probability > threshold (strict greater-than)."""
    proc = _function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData")
    assert re.search(r"bit\s*=\s*\(\s*probability\s*>\s*threshold\s*\)\s*\?\s*1\s*:\s*0", _normalize(proc))


def test_databuffer_identifier_window_is_bounded_fifo():
    """The identifier deque is a bounded FIFO: pop_front when at capacity before
    pushing the new bit, so it always holds the most-recent identifier_size bits."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"if\s*\(\s*identifier_buffer_\.size\(\)\s*>=\s*identifier_buffer_size_\s*\)", proc)
    assert "identifier_buffer_.pop_front()" in proc
    assert "identifier_buffer_.push_back(bit)" in proc
    assert proc.find("identifier_buffer_.pop_front()") < proc.find("identifier_buffer_.push_back(bit)")


def test_databuffer_state_machine_transitions():
    """kInactive -> kWaiting once enough bits seen; kWaiting -> kReceiving only on
    an exact start-pattern match (and clears buffers); EOT match -> kInactive."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))

    # kInactive -> kWaiting
    assert "current_state_ = DataReceptionState::kWaiting" in proc
    # kWaiting confirms an exact start match before kReceiving
    assert re.search(r"identifier_snapshot\s*==\s*start_of_transmission_", proc)
    assert "current_state_ = DataReceptionState::kReceiving" in proc
    # entering receiving clears buffers (drop the matched start identifier bits)
    waiting_to_recv = proc[proc.find("== start_of_transmission_"):]
    assert "ClearBuffers()" in waiting_to_recv[: waiting_to_recv.find("kReceiving") + 40]
    # EOT match returns to inactive
    assert re.search(r"identifier_snapshot\s*==\s*end_of_transmission_", proc)
    assert "current_state_ = DataReceptionState::kInactive" in proc


def test_databuffer_receiving_appends_bits():
    """In kReceiving every incoming bit is appended to the bit buffer."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert "bit_buffer_.push_back(bit)" in proc


def test_databuffer_overflow_resets_when_no_end_pattern():
    """If the bit buffer fills without an EOT match, the machine resets to
    kInactive and clears buffers (the overflow-reset branch)."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"bit_buffer_\.size\(\)\s*>=\s*max_bit_buffer_size_", proc)
    # this branch is the else-if of the EOT-match check (only fires when NOT end)
    overflow = proc[proc.find(">= max_bit_buffer_size_"):]
    assert "ClearBuffers()" in overflow[:120]
    assert "DataReceptionState::kInactive" in overflow[:200]
    assert "Buffer overflow" in proc


def test_databuffer_checksum_is_unsigned_sum_of_bytes():
    """CalculateChecksum is an 8-bit wrapping sum of the bytes (uint8_t accumulator
    + cast each char to uint8_t)."""
    cs = _function_body(read(AFSK_SRC), "AudioDataBuffer::CalculateChecksum")
    flat = _normalize(cs)
    assert re.search(r"uint8_t\s+checksum\s*=\s*0", flat)
    assert re.search(r"checksum\s*\+=\s*static_cast<uint8_t>\(\s*character\s*\)", flat)
    assert re.search(r"return\s+checksum", flat)


def test_databuffer_too_short_payload_rejected():
    """A decoded payload shorter than the minimum length is rejected with a
    'Data too short' warning and returns false (clears buffer)."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"if\s*\(\s*bytes\.size\(\)\s*<\s*minimum_length\s*\)", proc)
    too_short = proc[proc.find("bytes.size() < minimum_length"):]
    assert "Data too short" in too_short[:200]
    assert "ClearBuffers()" in too_short[:200]
    assert re.search(r"return\s+false", too_short[:200])


def test_databuffer_minimum_length_depends_on_checksum_flag():
    """With checksum on, minimum_length reserves an extra byte for the checksum
    (1 + start_size/8); with it off, it is start_size/8. This is the boundary that
    decides 'too short'."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"minimum_length\s*=\s*1\s*\+\s*start_of_transmission_\.size\(\)\s*/\s*8", proc)
    assert re.search(r"minimum_length\s*=\s*start_of_transmission_\.size\(\)\s*/\s*8", proc)


def test_databuffer_checksum_mismatch_rejected_only_when_enabled():
    """Checksum validation runs only when enabled; a mismatch is logged and the
    payload is rejected (return false), never decoded."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"if\s*\(\s*enable_checksum_validation_\s*\)", proc)
    assert "CalculateChecksum(result)" in proc
    assert re.search(r"if\s*\(\s*calculated_checksum\s*!=\s*received_checksum\s*\)", proc)
    mismatch = proc[proc.find("calculated_checksum != received_checksum"):]
    assert "Checksum mismatch" in mismatch[:200]
    assert "ClearBuffers()" in mismatch[:200]
    assert re.search(r"return\s+false", mismatch[:200])


def test_databuffer_success_path_sets_decoded_text_and_returns_true():
    """On a valid frame, decoded_text is set to the parsed string and true is
    returned. This is the only success exit."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(r"decoded_text\s*=\s*result", proc)
    # success returns true after assigning decoded_text
    tail = proc[proc.find("decoded_text = result"):]
    assert re.search(r"return\s+true", tail[:80])
    # function falls through to a final `return false` when nothing decoded.
    # The last *statement* of the body (split on ';{}') must be `return false`.
    last_stmt = _statements(proc)[-1]
    assert last_stmt == "return false", f"expected fall-through 'return false', got {last_stmt!r}"


def test_databuffer_text_strips_trailing_identifier_and_checksum():
    """Decoded text is bytes[0 .. size-minimum_length): the trailing checksum/EOT
    bytes are stripped. An off-by-one here corrupts the payload."""
    proc = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ProcessProbabilityData"))
    assert re.search(
        r"text_bytes\(\s*bytes\.begin\(\)\s*,\s*bytes\.begin\(\)\s*\+\s*bytes\.size\(\)\s*-\s*minimum_length\s*\)",
        proc,
    )


def test_convertbitstobytes_msb_first_and_truncates_partial_byte():
    """Bit packing is MSB-first: byte |= bits[i*8+j] << (7-j); trailing bits that
    don't fill a byte are truncated (complete_bytes_count = size/8 integer div)."""
    fn = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ConvertBitsToBytes"))
    # integer-truncating count
    assert re.search(r"complete_bytes_count\s*=\s*bits\.size\(\)\s*/\s*8", fn)
    # MSB-first shift
    assert re.search(r"byte_value\s*\|=\s*bits\[\s*i\s*\*\s*8\s*\+\s*j\s*\]\s*<<\s*\(\s*7\s*-\s*j\s*\)", fn)
    # the inner loop runs exactly 8 bits per byte
    assert re.search(r"for\s*\(\s*size_t\s+j\s*=\s*0\s*;\s*j\s*<\s*8\s*;", fn)


def test_clearbuffers_clears_identifier_and_bit_buffers():
    fn = _normalize(_function_body(read(AFSK_SRC), "AudioDataBuffer::ClearBuffers"))
    assert "identifier_buffer_.clear()" in fn
    assert "bit_buffer_.clear()" in fn


# ===========================================================================
# ReceiveWifiCredentialsFromAudio -- top-level driver loop.
# ===========================================================================
def test_receive_gates_on_wifi_configuring_state_with_sleep():
    """When the device is NOT in kDeviceStateWifiConfiguring the loop sleeps 100ms
    and continues -- it must not process audio outside config mode."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"if\s*\(\s*app->GetDeviceState\(\)\s*!=\s*kDeviceStateWifiConfiguring\s*\)", fn)
    gate = fn[fn.find("!= kDeviceStateWifiConfiguring"):]
    assert "vTaskDelay(pdMS_TO_TICKS(100))" in gate[:160], "not-in-config branch must sleep 100ms"
    assert "continue" in gate[:200], "not-in-config branch must continue (skip processing)"


def test_receive_read_failure_retries_after_short_delay():
    """A failed audio read logs, delays 10ms, and retries -- it must not fall
    through to processing stale data."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"if\s*\(\s*!app->GetAudioService\(\)\.ReadAudioData\(", fn)
    fail = fn[fn.find("ReadAudioData("):]
    assert "vTaskDelay(pdMS_TO_TICKS(10))" in fail[:260]
    assert "continue" in fail[:320]


def test_receive_stereo_to_mono_downmix_takes_left_channel():
    """2-channel input is downmixed to mono by taking every other sample
    (j += 2), i.e. the left channel; size halves (audio_data.size()/2)."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"if\s*\(\s*input_channels\s*==\s*2\s*\)", fn), "missing 2-channel branch"
    assert re.search(r"std::vector<int16_t>\(\s*audio_data\.size\(\)\s*/\s*2\s*\)", fn), "mono size must halve"
    assert re.search(r"mono_data\[i\]\s*=\s*audio_data\[j\]", fn)
    assert re.search(r"\bj\s*\+=\s*2", fn), "stereo stride must be 2"
    assert "audio_data = std::move(mono_data)" in fn


def test_receive_downsample_step_and_branch():
    """kDownsampleStep = 16000 / kAudioSampleRate; the >1 branch decimates, the
    else branch copies 1:1. Both downsample paths must exist."""
    src = _normalize(read(AFSK_SRC))
    assert re.search(
        r"kDownsampleStep\s*=\s*static_cast<float>\(kInputSampleRate\)\s*/\s*static_cast<float>\(kAudioSampleRate\)",
        src,
    )
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"if\s*\(\s*kDownsampleStep\s*>\s*1\.0f\s*\)", fn)
    # decimation index derived from the step
    assert re.search(r"sample_index\s*=\s*static_cast<size_t>\(\s*i\s*/\s*kDownsampleStep\s*\)", fn)


def test_receive_no_newline_is_invalid_format_and_continues():
    """If the decoded payload has no '\\n' separator, it is an 'Invalid data
    format' error and the loop continues (does not save a malformed credential)."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    # newline search
    assert re.search(r"newline_position\s*=\s*data_buffer\.decoded_text->find\('\\n'\)", fn)
    # the no-newline branch
    assert re.search(r"if\s*\(\s*newline_position\s*!=\s*std::string::npos\s*\)", fn)
    no_nl = fn[fn.find("else"):] if "else" in fn else fn
    assert "Invalid data format" in fn
    invalid = fn[fn.find("Invalid data format"):]
    assert "continue" in invalid[:120], "invalid-format branch must continue, not save"
    # 'Invalid data format' is logged at error level
    assert re.search(r"ESP_LOGE[^;]*Invalid data format", fn)


def test_receive_splits_ssid_before_newline_and_password_after():
    """SSID is substr(0, newline) and password is substr(newline+1): a split-point
    off-by-one would swap/cut the fields."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"wifi_ssid\s*=\s*data_buffer\.decoded_text->substr\(\s*0\s*,\s*newline_position\s*\)", fn)
    assert re.search(r"wifi_password\s*=\s*data_buffer\.decoded_text->substr\(\s*newline_position\s*\+\s*1\s*\)", fn)


def test_receive_saves_then_stops_ap_then_returns():
    """On a valid credential the order is: AddSsid -> StopConfigAp -> reset
    decoded_text -> return. Stopping the AP before saving would lose creds."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    add = fn.find("ssid_manager.AddSsid(wifi_ssid, wifi_password)")
    stop = fn.find("wifi_manager->StopConfigAp()")
    reset = fn.find("data_buffer.decoded_text.reset()")
    assert add != -1 and stop != -1 and reset != -1
    assert add < stop < reset, "order must be AddSsid -> StopConfigAp -> reset"
    ret = fn[reset:]
    assert re.search(r"\breturn\b", ret[:60]), "must return after a successful save"


def test_receive_only_processes_when_decoded_text_present():
    """Credentials are only extracted after ProcessProbabilityData reports a
    complete frame AND decoded_text has a value."""
    fn = _normalize(_function_body(read(AFSK_SRC), "void ReceiveWifiCredentialsFromAudio"))
    assert re.search(r"if\s*\(\s*data_buffer\.ProcessProbabilityData\(\s*probabilities\s*,\s*0\.5f\s*\)\s*\)", fn)
    assert re.search(r"if\s*\(\s*data_buffer\.decoded_text\.has_value\(\)\s*\)", fn)


# ===========================================================================
# Module constants (header) -- the values plumbed into the DSP.
# ===========================================================================
def test_header_dsp_constants_exact_values():
    """Lock the AFSK constants that parameterize the whole pipeline. Note 6400 %
    100 == 0, so the default config does NOT hit the divisibility warning."""
    hdr = _normalize(read(AFSK_HDR))
    assert re.search(r"kAudioSampleRate\s*=\s*6400", hdr)
    assert re.search(r"kMarkFrequency\s*=\s*1800", hdr)
    assert re.search(r"kSpaceFrequency\s*=\s*1500", hdr)
    assert re.search(r"kBitRate\s*=\s*100", hdr)
    assert re.search(r"kWindowSize\s*=\s*64", hdr)
    # internal-consistency boundary: default rate IS divisible by default bit rate
    assert 6400 % 100 == 0


# ===========================================================================
# HARNESS SELF-TESTS -- prove the scanner does what the locks above assume.
# ===========================================================================
def test_harness_function_body_is_brace_balanced_and_scoped():
    """_function_body must return exactly one brace-balanced body, and a marker
    in a different function must NOT leak into it."""
    synthetic = (
        "void alpha() {\n"
        "    int a = 1;\n"
        "    if (a) { a = 2; }\n"
        "}\n"
        "void beta() {\n"
        "    int beta_only = 99;\n"
        "}\n"
    )
    alpha = _function_body(synthetic, "void alpha")
    assert alpha.count("{") == alpha.count("}")
    assert "a = 2" in alpha
    assert "beta_only" not in alpha, "scope leaked into the next function"


def test_harness_normalize_collapses_multiline_expression():
    """A multi-line return must be matchable as one collapsed expression."""
    synthetic = (
        "float g() {\n"
        "    return std::sqrt(re * re + im * im) /\n"
        "           (static_cast<float>(w) / 2.0f);\n"
        "}\n"
    )
    body = _normalize(_function_body(synthetic, "float g"))
    assert re.search(r"return std::sqrt\(re \* re \+ im \* im\) / \(static_cast<float>\(w\) / 2\.0f\)", body)


def test_harness_strip_comments_ignores_commented_code():
    """A leaked-looking marker inside a comment must be invisible to the scan."""
    synthetic = (
        "void h() {\n"
        "    // filter_coefficient_ = 3.0f * cos_coefficient_;  // OLD wrong value\n"
        "    /* samples_per_bit_ = 999; */\n"
        "    filter_coefficient_ = 2.0f * cos_coefficient_;\n"
        "}\n"
    )
    body = _normalize(_function_body(synthetic, "void h"))
    assert "3.0f" not in body, "comment content leaked through"
    assert "999" not in body
    assert "filter_coefficient_ = 2.0f * cos_coefficient_" in body


def test_harness_statements_blank_string_literals():
    """Statement splitting must blank literal contents so a ';' or token inside a
    format string can't mis-split or false-match."""
    synthetic = 'void k() { ESP_LOGW(tag, "rate; samples_per_bit_ = bogus"); int x = 1; }'
    stmts = _statements(synthetic)
    # the format-string body is blanked, so 'bogus' never appears as a token
    assert not any("bogus" in s for s in stmts)
    # the literal's embedded ';' did not split off a spurious 'samples_per_bit_' stmt
    assert not any(s.strip().startswith("samples_per_bit_") for s in stmts)
