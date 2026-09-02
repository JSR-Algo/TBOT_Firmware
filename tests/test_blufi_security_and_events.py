"""Source-level regression tests locking the US-005 BLUFI SECURITY layer + helpers.

Round-2 gap fill: the sibling suite tests/test_blufi_provisioning_stability.py
(FW1..FW28) locks the provisioning / event-ordering invariants. This file adds a
dedicated static source-scrape suite over the BLUFI *security* primitives and the
device-name helpers that were not yet covered:

  - _dh_negotiate_data_handler  (DH key exchange error/success surface)
  - _aes_encrypt / _aes_decrypt (invalid-arg guards + mbedtls CFB128 call)
  - _crc_checksum               (esp_crc16_be wiring)
  - _security_init / _security_deinit (alloc guard, memset, dhm/aes init/free)
  - SanitizedSerial             (allowed-char filter + stop-at-NUL)
  - GetBlufiDeviceName          (case-insensitive prefix detection / TBOT- prepend
                                 / empty-serial -> MAC fallback)

Same convention as the sibling suite: module-level ROOT, a read(path) helper, and
test_* functions asserting on substrings / regex / relative ordering of real
markers in the firmware source text (NO device, NO compiler). Each test is tagged
with a SEC* id so a failure is traceable back to the audited security invariant.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BLUFI_CPP = "main/boards/common/blufi.cpp"
BLUFI_H = "main/boards/common/blufi.h"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# Body-slicing helpers (anchored on real function signatures so every test
# scopes its markers to the function under test, never the whole file).
# ---------------------------------------------------------------------------
def _slice(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    end = text.index(end_marker, start + len(start_marker))
    return text[start:end]


def _sanitized_serial_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(
        cpp,
        "static std::string SanitizedSerial(",
        "static std::string GetBlufiDeviceName()",
    )


def _device_name_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(
        cpp,
        "static std::string GetBlufiDeviceName()",
        "static wifi_mode_t GetWifiModeWithFallback",
    )


def _security_init_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "void Blufi::_security_init()", "void Blufi::_security_deinit()")


def _security_deinit_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(
        cpp,
        "void Blufi::_security_deinit()",
        "void Blufi::_dh_negotiate_data_handler(",
    )


def _dh_handler_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(
        cpp,
        "void Blufi::_dh_negotiate_data_handler(",
        "int Blufi::_aes_encrypt(",
    )


def _aes_encrypt_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "int Blufi::_aes_encrypt(", "int Blufi::_aes_decrypt(")


def _aes_decrypt_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "int Blufi::_aes_decrypt(", "uint16_t Blufi::_crc_checksum(")


def _crc_checksum_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "uint16_t Blufi::_crc_checksum(", "int Blufi::_get_softap_conn_num()")

def _credential_guard_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "bool Blufi::_require_secure_session_for_credentials()", "int Blufi::_get_softap_conn_num()")

def _handle_event_body() -> str:
    cpp = read(BLUFI_CPP)
    return _slice(cpp, "void Blufi::_handle_event(", "void Blufi::_ble_setup_timeout_cb")

def _event_case_body(case_name: str) -> str:
    body = _handle_event_body()
    start = body.index(f"case {case_name}")
    next_case = body.find("\n        case ", start + 1)
    default_case = body.find("\n        default:", start + 1)
    candidates = [idx for idx in (next_case, default_case) if idx != -1]
    end = min(candidates) if candidates else len(body)
    return body[start:end]


# ===========================================================================
# SanitizedSerial — allowed-char filter [0-9A-Za-z-], stop-at-NUL.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC1: SanitizedSerial bounds its scan by BOTH max_len AND a NUL terminator,
#       so a fixed-size eFuse buffer with a short serial does not over-read into
#       the zero-padding (and a buffer with no NUL is still bounded by max_len).
# ---------------------------------------------------------------------------
def test_sec1_sanitized_serial_stops_at_max_len_and_nul():
    body = _sanitized_serial_body()

    # The loop condition must combine the length bound with the NUL stop.
    assert re.search(
        r"for\s*\(\s*size_t\s+i\s*=\s*0\s*;\s*i\s*<\s*max_len\s*&&\s*bytes\[i\]\s*!=\s*0\s*;",
        body,
    ), "SanitizedSerial must bound on `i < max_len && bytes[i] != 0`"


# ---------------------------------------------------------------------------
# SEC2: SanitizedSerial admits ONLY [0-9], [A-Z], [a-z] and '-'. Every other
#       byte (spaces, control chars, punctuation, high bytes) is dropped. This
#       is the allowlist that keeps a garbage eFuse blob from producing an
#       un-advertisable BLE name.
# ---------------------------------------------------------------------------
def test_sec2_sanitized_serial_allows_only_alnum_and_dash():
    body = _sanitized_serial_body()

    # Each of the four allowed ranges/chars must be present in the guard.
    assert "ch >= '0' && ch <= '9'" in body
    assert "ch >= 'A' && ch <= 'Z'" in body
    assert "ch >= 'a' && ch <= 'z'" in body
    assert "ch == '-'" in body

    # The four conditions are OR-ed into a single accept guard.
    assert re.search(
        r"if\s*\(\(ch\s*>=\s*'0'\s*&&\s*ch\s*<=\s*'9'\)\s*\|\|\s*"
        r"\(ch\s*>=\s*'A'\s*&&\s*ch\s*<=\s*'Z'\)\s*\|\|\s*"
        r"\(ch\s*>=\s*'a'\s*&&\s*ch\s*<=\s*'z'\)\s*\|\|\s*ch\s*==\s*'-'\)",
        body,
        re.DOTALL,
    ), "the allowlist must be a single OR of the four allowed classes"

    # Only an accepted char is pushed; there is exactly one push_back (the
    # filter), so a dropped byte produces nothing — no else/passthrough branch.
    assert body.count("serial.push_back(") == 1
    assert "push_back(ch)" in body


# ---------------------------------------------------------------------------
# SEC3: SanitizedSerial returns the accumulated std::string (it does not, e.g.,
#       return a pointer into the caller's transient buffer). Empty input yields
#       an empty string, which is the signal GetBlufiDeviceName uses to fall back
#       to the MAC name.
# ---------------------------------------------------------------------------
def test_sec3_sanitized_serial_returns_accumulated_string():
    body = _sanitized_serial_body()

    assert "std::string serial;" in body
    assert "return serial;" in body
    # The cast to char is explicit (bytes are uint8_t), avoiding sign-extension
    # surprises in the range comparisons.
    assert "const char ch = static_cast<char>(bytes[i]);" in body


# ===========================================================================
# GetBlufiDeviceName — case-insensitive already-prefixed detection vs TBOT-
#                      prepend; empty-serial -> MAC fallback.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC4: the eFuse-serial branch is gated on a non-empty sanitized serial, and a
#       successful eFuse read. An empty serial must NOT short-circuit the MAC
#       fallback (it must fall through to the MAC name below the #endif).
# ---------------------------------------------------------------------------
def test_sec4_device_name_serial_branch_requires_nonempty_serial():
    body = _device_name_body()

    # Sanity: this is the serial branch (it sanitizes the eFuse blob).
    assert "const std::string serial = SanitizedSerial(serial_number, 32);" in body
    # The serial path only runs when the eFuse read succeeded.
    assert "esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK" in body
    # And only when the sanitized serial is non-empty.
    assert "if (!serial.empty())" in body

    # The empty-serial case must reach the MAC fallback (no early return inside
    # the !serial.empty() block leaks past it to skip the MAC path). The MAC
    # fallback must sit AFTER the #endif so it is the unconditional default.
    endif_idx = body.index("#endif")
    mac_idx = body.index("esp_read_mac(mac, ESP_MAC_WIFI_STA);")
    assert endif_idx < mac_idx, "the MAC fallback must follow the #ifdef serial branch"


# ---------------------------------------------------------------------------
# SEC5: the already-prefixed detection is CASE-INSENSITIVE on each of T,B,O,T
#       and requires a literal '-' at index 4 and length >= 5. A serial that
#       already reads "tbot-..." / "TBOT-..." (any case mix) must NOT be double-
#       prefixed; anything else gets a fresh "TBOT-" prepend.
# ---------------------------------------------------------------------------
def test_sec5_device_name_prefix_detection_is_case_insensitive():
    body = _device_name_body()

    # Length precondition: at least "TBOT-" (5 chars) before reading index 4.
    assert "serial.size() >= 5" in body

    # Each of the four brand letters is matched case-insensitively.
    assert "(serial[0] == 'T' || serial[0] == 't')" in body
    assert "(serial[1] == 'B' || serial[1] == 'b')" in body
    assert "(serial[2] == 'O' || serial[2] == 'o')" in body
    assert "(serial[3] == 'T' || serial[3] == 't')" in body

    # The separator at index 4 must be a literal dash (NOT case-folded).
    assert "serial[4] == '-'" in body

    # The decision: already-prefixed -> return as-is; else prepend "TBOT-".
    assert 'already_prefixed ? serial : (std::string("TBOT-") + serial)' in body


# ---------------------------------------------------------------------------
# SEC6: the length guard `serial.size() >= 5` must be AND-ed with the brand-char
#       checks BEFORE any indexed read of serial[4]. A 4-char serial like "TBOT"
#       (no dash, size 4) must be classified not-prefixed WITHOUT reading
#       serial[4] out of bounds — the size check has to come first in the &&.
# ---------------------------------------------------------------------------
def test_sec6_device_name_size_check_precedes_index_read():
    body = _device_name_body()

    cond_start = body.index("const bool already_prefixed =")
    cond_end = body.index(";", cond_start)
    cond = body[cond_start:cond_end]

    size_idx = cond.index("serial.size() >= 5")
    index4_idx = cond.index("serial[4]")
    assert size_idx < index4_idx, (
        "serial.size() >= 5 must be evaluated before serial[4] is read, so a "
        "short serial cannot index past the end (short-circuit &&)"
    )

    # The size guard must be the FIRST top-level term of the predicate value
    # (immediately after the `=`), joined to the rest by `&&` — so on a short
    # serial the `&&` short-circuits before any indexed read. (The per-letter
    # case-folding uses `||` *inside* parenthesized terms, which is fine; what
    # matters is the size check leads the && chain and nothing reads serial[...]
    # before it.)
    rhs = cond.split("=", 1)[1].strip()
    assert rhs.startswith("serial.size() >= 5"), (
        "serial.size() >= 5 must be the leading term of the predicate value"
    )
    assert rhs[len("serial.size() >= 5"):].lstrip().startswith("&&"), (
        "the size guard must be &&-joined to the rest so it short-circuits the "
        "indexed reads on a short serial"
    )
    # No indexed read of serial may appear before the size guard.
    assert "serial[" not in cond[:size_idx], (
        "no serial[...] index may be read before the size>=5 guard"
    )


# ---------------------------------------------------------------------------
# SEC7: the MAC fallback advertises "TBOT-<12 hex>" from the Wi-Fi STA MAC,
#       with a buffer large enough for the formatted name. "TBOT-" (5) +
#       12 hex + NUL = 18 bytes; the name buffer is 24, so snprintf cannot
#       truncate the name. Wi-Fi STA is the backend/device identity; BT MAC can
#       differ by +2 on ESP32-S3 and breaks mobile claim/assignment matching.
# ---------------------------------------------------------------------------
def test_sec7_device_name_mac_fallback_format_and_buffer():
    body = _device_name_body()

    assert "uint8_t mac[6] = {0};" in body
    assert "esp_read_mac(mac, ESP_MAC_WIFI_STA);" in body
    assert "esp_read_mac(mac, ESP_MAC_BT);" not in body

    # The MAC name buffer must be >= 18 (we assert the concrete declared size and
    # that the format string is the documented TBOT- + 6 hex bytes).
    m = re.search(r"char\s+name\[(\d+)\]\s*=\s*\{0\};", body)
    assert m is not None, "MAC fallback must declare a fixed-size name buffer"
    assert int(m.group(1)) >= 18, "name buffer must hold 'TBOT-' + 12 hex + NUL"

    assert 'snprintf(name, sizeof(name), "TBOT-%02X%02X%02X%02X%02X%02X"' in body
    # All six MAC octets feed the format.
    for i in range(6):
        assert f"mac[{i}]" in body
    assert "return std::string(name);" in body


# ===========================================================================
# _security_init / _security_deinit — alloc guard, memset, dhm/aes init/free.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC8: _security_init allocates the context, GUARDS the (new) allocation
#       against null and bails before any member access, then zero-fills the
#       whole struct before constructing the dhm/aes sub-contexts.
# ---------------------------------------------------------------------------
def test_sec8_security_init_null_guards_and_memsets():
    body = _security_init_body()

    alloc_idx = body.index("m_sec = new BlufiSecurity();")
    guard_idx = body.index("if (m_sec == nullptr)")
    memset_idx = body.index("memset(m_sec, 0, sizeof(BlufiSecurity));")

    # The null guard sits between the alloc and the first member write (memset).
    assert alloc_idx < guard_idx < memset_idx, (
        "_security_init must null-check the allocation before touching m_sec"
    )

    # The null path logs and returns WITHOUT touching m_sec members.
    guard_return = body.index("return;", guard_idx)
    assert guard_return < memset_idx, (
        "the null-alloc path must return before the memset / member init"
    )
    null_block = body[guard_idx:guard_return]
    assert 'ESP_LOGE(BLUFI_TAG, "Failed to allocate security context");' in null_block
    # The struct is zeroed BEFORE the sub-contexts are created (so dh_param / iv
    # / share_len start at a known 0 state).
    assert memset_idx < body.index("m_sec->dhm = new mbedtls_dhm_context();")


# ---------------------------------------------------------------------------
# SEC9: _security_init constructs and INITIALIZES both mbedtls sub-contexts and
#       zeroes the IV. Order: allocate dhm+aes, then mbedtls_*_init each, then
#       memset the IV to 0 (CFB128 starts from a known IV per session).
# ---------------------------------------------------------------------------
def test_sec9_security_init_creates_and_inits_dhm_aes_and_zero_iv():
    body = _security_init_body()

    dhm_new = body.index("m_sec->dhm = new mbedtls_dhm_context();")
    aes_new = body.index("m_sec->aes = new mbedtls_aes_context();")
    dhm_init = body.index("mbedtls_dhm_init(m_sec->dhm);")
    aes_init = body.index("mbedtls_aes_init(m_sec->aes);")
    iv_zero = body.index("memset(m_sec->iv, 0x0, sizeof(m_sec->iv));")

    # Both sub-contexts are allocated before they are initialized, and the IV is
    # zeroed last.
    assert dhm_new < dhm_init
    assert aes_new < aes_init
    assert dhm_init < iv_zero and aes_init < iv_zero, (
        "the IV must be zeroed after the mbedtls contexts are initialized"
    )


# ---------------------------------------------------------------------------
# SEC10: _security_deinit is null-safe and frees EVERYTHING in the right order:
#        free the heap dh_param (only if non-null), mbedtls_*_free both contexts,
#        delete the dhm/aes heap objects, delete the struct, then null m_sec.
#        Nulling m_sec last is what makes a subsequent deinit a safe no-op.
# ---------------------------------------------------------------------------
def test_sec10_security_deinit_is_null_safe_and_frees_in_order():
    body = _security_deinit_body()

    # Null-safe entry.
    assert "if (m_sec == nullptr)" in body
    early_return = body.index("return;")
    assert early_return < body.index("mbedtls_dhm_free"), (
        "_security_deinit must early-return on null m_sec before any free"
    )

    # dh_param is freed only when allocated (no free(nullptr) churn / it is the
    # malloc'd DH buffer, distinct from the new'd sub-contexts).
    assert "if (m_sec->dh_param) {" in body
    assert "free(m_sec->dh_param);" in body

    dhm_free = body.index("mbedtls_dhm_free(m_sec->dhm);")
    aes_free = body.index("mbedtls_aes_free(m_sec->aes);")
    dhm_delete = body.index("delete m_sec->dhm;")
    aes_delete = body.index("delete m_sec->aes;")
    struct_delete = body.index("delete m_sec;")
    null_idx = body.index("m_sec = nullptr;")

    # mbedtls free must precede delete of the sub-context heap object, the struct
    # is deleted after its members, and m_sec is nulled dead last.
    assert dhm_free < dhm_delete < struct_delete
    assert aes_free < aes_delete < struct_delete
    assert struct_delete < null_idx, "m_sec must be nulled only after delete"


# ---------------------------------------------------------------------------
# SEC11: the BLE session lifecycle wires security to connect/disconnect:
#        ESP_BLUFI_EVENT_BLE_CONNECT calls _security_init() and
#        ESP_BLUFI_EVENT_BLE_DISCONNECT calls _security_deinit(), so the DH/AES
#        context is created fresh per session and torn down on disconnect (no
#        key material survives across pairings). The destructor also deinits.
# ---------------------------------------------------------------------------
def test_sec11_security_lifecycle_tied_to_ble_session():
    cpp = read(BLUFI_CPP)

    connect = _slice(
        cpp,
        "case ESP_BLUFI_EVENT_BLE_CONNECT:",
        "case ESP_BLUFI_EVENT_BLE_DISCONNECT:",
    )
    assert "_security_init();" in connect, (
        "a new BLE connection must initialize a fresh security context"
    )

    disconnect = _slice(
        cpp,
        "case ESP_BLUFI_EVENT_BLE_DISCONNECT:",
        "case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:",
    )
    assert "_security_deinit();" in disconnect, (
        "a BLE disconnect must tear down the security context"
    )

    # The destructor must also guard + deinit so a dangling context is freed.
    dtor = _slice(cpp, "Blufi::~Blufi()", "esp_err_t Blufi::init()")
    assert "if (m_sec) {" in dtor
    assert "_security_deinit();" in dtor


# ===========================================================================
# _dh_negotiate_data_handler — error/success surface.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC12: a DH handler call with no security context reports INIT_SECURITY_ERROR
#        and returns before touching any member (the m_sec==nullptr guard is the
#        very first thing). This is the "negotiate before connect" defense.
# ---------------------------------------------------------------------------
def test_sec12_dh_handler_null_sec_reports_init_security_error():
    body = _dh_handler_body()

    guard_idx = body.index("if (m_sec == nullptr)")
    err_idx = body.index("btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);")
    return_idx = body.index("return;", guard_idx)

    # Guard is first; it reports the init-security error and returns before the
    # length check / any member deref.
    len_check = body.index("if (len < 1)")
    assert guard_idx < err_idx < return_idx < len_check, (
        "the m_sec==nullptr guard must report INIT_SECURITY_ERROR and return "
        "BEFORE the length check or any member access"
    )
    assert 'ESP_LOGE(BLUFI_TAG, "Security not initialized in DH handler");' in body


# ---------------------------------------------------------------------------
# SEC13: a too-short frame (len < 1, i.e. no type byte) reports
#        DATA_FORMAT_ERROR and returns before data[0] is read. Reading the type
#        byte from a zero-length buffer would be an over-read.
# ---------------------------------------------------------------------------
def test_sec13_dh_handler_len_lt_1_reports_data_format_error():
    body = _dh_handler_body()

    len_idx = body.index("if (len < 1)")
    err_idx = body.index("btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);", len_idx)
    type_read = body.index("uint8_t type = data[0];")

    assert len_idx < err_idx < type_read, (
        "the len<1 guard must report DATA_FORMAT_ERROR and return before reading "
        "the type byte data[0]"
    )
    assert 'ESP_LOGE(BLUFI_TAG, "DH handler: data too short");' in body
    # The error is followed by an early return (does not fall through to data[0]).
    assert body.index("return;", err_idx) < type_read


# ---------------------------------------------------------------------------
# SEC14: the DH_PARAM_LEN packet (type 0x00) must carry the 2-byte length, so a
#        frame shorter than 3 bytes is rejected with DATA_FORMAT_ERROR before
#        data[1]/data[2] are read to compute dh_param_len.
# ---------------------------------------------------------------------------
def test_sec14_dh_handler_type00_param_len_truncation_guard():
    body = _dh_handler_body()

    case0 = _slice(body, "case 0x00:", "case 0x01:")
    short_guard = case0.index("if (len < 3)")
    err_idx = case0.index("btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);", short_guard)
    len_compute = case0.index("m_sec->dh_param_len = (data[1] << 8) | data[2];")

    assert short_guard < err_idx < len_compute, (
        "type 0x00 must reject len<3 (no room for the 2-byte length) before "
        "reading data[1]/data[2]"
    )
    assert 'ESP_LOGE(BLUFI_TAG, "DH_PARAM_LEN packet too short");' in case0
    # The truncation guard returns; it does not fall through to the malloc.
    assert case0.index("return;", err_idx) < len_compute


# ---------------------------------------------------------------------------
# SEC15: type 0x00 reallocates dh_param safely: a previously-held dh_param is
#        freed AND nulled before the new malloc (no double-free / no leak on a
#        repeated 0x00), and a failed malloc reports DH_MALLOC_ERROR.
# ---------------------------------------------------------------------------
def test_sec15_dh_handler_type00_realloc_is_leak_and_double_free_safe():
    body = _dh_handler_body()
    case0 = _slice(body, "case 0x00:", "case 0x01:")

    # Free-then-null the old buffer before the new malloc.
    free_old = case0.index("free(m_sec->dh_param);")
    null_old = case0.index("m_sec->dh_param = nullptr;", free_old)
    malloc_new = case0.index("m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);")
    assert free_old < null_old < malloc_new, (
        "an existing dh_param must be freed and nulled before the new malloc"
    )
    # The old free is guarded so we never free a null on the first 0x00.
    assert "if (m_sec->dh_param) {" in case0

    # A failed malloc reports DH_MALLOC_ERROR.
    assert "if (m_sec->dh_param == nullptr) {" in case0
    assert "btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);" in case0


def test_sec15b_repeated_dh_session_releases_previous_crypto_heap_first():
    body = _dh_handler_body()
    case0 = _slice(body, "case 0x00:", "case 0x01:")

    reset_dhm = case0.index("mbedtls_dhm_free(m_sec->dhm);")
    init_dhm = case0.index("mbedtls_dhm_init(m_sec->dhm);", reset_dhm)
    reset_aes = case0.index("mbedtls_aes_free(m_sec->aes);")
    init_aes = case0.index("mbedtls_aes_init(m_sec->aes);", reset_aes)
    malloc_new = case0.index("m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);")

    assert reset_dhm < init_dhm < malloc_new
    assert reset_aes < init_aes < malloc_new
    assert case0.index("m_blufi_security_negotiated = false;") < malloc_new
    assert case0.index("memset(m_sec->share_key, 0, sizeof(m_sec->share_key));") < malloc_new
    assert case0.index("memset(m_sec->psk, 0, sizeof(m_sec->psk));") < malloc_new


def test_sec15c_dh_param_length_is_bounded_and_malloc_failure_returns():
    body = _dh_handler_body()
    case0 = _slice(body, "case 0x00:", "case 0x01:")

    assert "m_sec->dh_param_len <= 0 || m_sec->dh_param_len > 1024" in case0
    assert "m_sec->dh_param_len = 0;" in case0
    malloc_error = case0.index("btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);")
    assert case0.index("return;", malloc_error) > malloc_error


# ---------------------------------------------------------------------------
# SEC16: type 0x01 (DH params) must reject a missing dh_param (a 0x01 that
#        arrives without a preceding successful 0x00 malloc) with DH_PARAM_ERROR
#        before it memcpy's into a null buffer.
# ---------------------------------------------------------------------------
def test_sec16_dh_handler_type01_requires_allocated_param():
    body = _dh_handler_body()
    case1 = _slice(body, "case 0x01:", "default:")

    guard = case1.index("if (m_sec->dh_param == nullptr) {")
    err_idx = case1.index("btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);", guard)
    memcpy_idx = case1.index("memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);")

    assert guard < err_idx < memcpy_idx, (
        "type 0x01 must reject a null dh_param (no prior 0x00) before the memcpy"
    )
    assert 'ESP_LOGE(BLUFI_TAG, "DH param not allocated");' in case1
    assert case1.index("return;", err_idx) < memcpy_idx


def test_sec16b_dh_handler_type01_rejects_truncated_param_data():
    body = _dh_handler_body()
    case1 = _slice(body, "case 0x01:", "default:")

    guard = case1.index("if (len < m_sec->dh_param_len + 1)")
    memcpy_idx = case1.index("memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);")
    assert guard < case1.index("return;", guard) < memcpy_idx


# ---------------------------------------------------------------------------
# SEC17: the type-0x01 success path performs the full DH key-agreement chain in
#        order — read_params -> make_public -> calc_secret -> md5(psk) ->
#        aes_setkey_enc — and each step has a typed btc error report on failure.
#        The PSK is the MD5 of the DH shared secret; the AES key is set from it.
# ---------------------------------------------------------------------------
def test_sec17_dh_handler_type01_success_chain_order_and_typed_errors():
    body = _dh_handler_body()
    case1 = _slice(body, "case 0x01:", "default:")

    read_params = case1.index("mbedtls_dhm_read_params(m_sec->dhm")
    make_public = case1.index("mbedtls_dhm_make_public(m_sec->dhm")
    calc_secret = case1.index("mbedtls_dhm_calc_secret(m_sec->dhm")
    md5_idx = case1.index("mbedtls_md5(m_sec->share_key, m_sec->share_len, m_sec->psk)")
    setkey_idx = case1.index("mbedtls_aes_setkey_enc(m_sec->aes, m_sec->psk, PSK_LEN * 8)")

    assert read_params < make_public < calc_secret < md5_idx < setkey_idx, (
        "DH chain order must be read_params -> make_public -> calc_secret -> "
        "md5(psk) -> aes_setkey_enc"
    )

    # Each step maps to its own typed error report.
    assert "btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);" in case1
    assert "btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);" in case1
    assert "btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);" in case1
    # calc_secret and setkey both surface ENCRYPT_ERROR.
    assert case1.count("btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);") >= 2

    # The PSK is exactly the MD5 of the shared key; the AES key length is PSK_LEN
    # bytes expressed in BITS (* 8) for mbedtls.
    assert "PSK_LEN * 8" in case1


def test_sec17b_dh_public_key_output_is_bounded_by_fixed_buffer():
    body = _dh_handler_body()
    case1 = _slice(body, "case 0x01:", "default:")

    length = case1.index("const int dhm_len = mbedtls_dhm_get_len(m_sec->dhm);")
    guard = case1.index("if (dhm_len <= 0 || dhm_len > DH_SELF_PUB_KEY_LEN)")
    make_public = case1.index("mbedtls_dhm_make_public(m_sec->dhm")
    assert length < guard < case1.index("return;", guard) < make_public
    assert "m_sec->self_public_key, DH_SELF_PUB_KEY_LEN," in " ".join(case1.split())


# ---------------------------------------------------------------------------
# SEC18: on a successful 0x01, the handler hands its own self_public_key back to
#        the BLUFI stack with need_free=false (the buffer is owned by m_sec, the
#        stack must NOT free it) and the output length is the dhm length. It then
#        frees+nulls+zeroes dh_param so the transient DH input does not linger.
# ---------------------------------------------------------------------------
def test_sec18_dh_handler_type01_output_wiring_and_param_cleanup():
    body = _dh_handler_body()
    case1 = _slice(body, "case 0x01:", "default:")

    assert "*output_data = m_sec->self_public_key;" in case1
    assert "*output_len = dhm_len;" in case1
    assert "*need_free = false;" in case1, (
        "self_public_key is owned by m_sec; the stack must not free it"
    )
    assert 'ESP_LOGI(BLUFI_TAG, "DH negotiation completed successfully");' in case1

    # dh_param is released after the output is wired (transient input cleanup).
    out_idx = case1.index("*output_data = m_sec->self_public_key;")
    free_idx = case1.index("free(m_sec->dh_param);", out_idx)
    null_idx = case1.index("m_sec->dh_param = nullptr;", free_idx)
    len0_idx = case1.index("m_sec->dh_param_len = 0;", null_idx)
    assert out_idx < free_idx < null_idx < len0_idx, (
        "after wiring the output, dh_param must be freed, nulled, and its length "
        "reset to 0"
    )


# ---------------------------------------------------------------------------
# SEC19: an unknown DH type falls into default: and only LOGS (no member writes,
#        no false success, no error-report that the phone would treat as a fatal
#        protocol violation). It must not write *output_data / *output_len.
# ---------------------------------------------------------------------------
def test_sec19_dh_handler_unknown_type_is_logged_and_inert():
    body = _dh_handler_body()
    default_idx = body.index("default:")
    default_block = body[default_idx:]

    assert 'ESP_LOGE(BLUFI_TAG, "DH handler unknown type: %d", type);' in default_block
    # The default arm does not fabricate an output or claim success.
    assert "*output_data" not in default_block
    assert "*output_len" not in default_block
    assert "*need_free" not in default_block


# ===========================================================================
# _aes_encrypt / _aes_decrypt — invalid-arg guard + mbedtls_aes_crypt_cfb128.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC20: _aes_encrypt rejects a null context / null key / null buffer / non-
#        positive length with -ESP_ERR_INVALID_ARG and never reaches the cipher.
#        The guard must short-circuit on m_sec before reading m_sec->aes.
# ---------------------------------------------------------------------------
def test_sec20_aes_encrypt_invalid_args_guard():
    body = _aes_encrypt_body()

    # The guard checks m_sec first (short-circuit), then aes, buffer, len > 0.
    assert "if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {" in body

    guard_idx = body.index("if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {")
    ret_invalid = body.index("return -ESP_ERR_INVALID_ARG;", guard_idx)
    cipher_idx = body.index("mbedtls_aes_crypt_cfb128(")
    assert guard_idx < ret_invalid < cipher_idx, (
        "invalid args must return -ESP_ERR_INVALID_ARG before the cipher call"
    )
    # encrypt rejects crypt_len <= 0 (a zero-length encrypt is a no-op error).
    assert "crypt_len <= 0" in body


# ---------------------------------------------------------------------------
# SEC21: _aes_encrypt runs CFB128 in ENCRYPT mode in-place (input buffer ==
#        output buffer), starting from a per-call IV whose byte 0 is the BLUFI
#        sequence/iv8, and returns crypt_len on success or the mbedtls error
#        code on failure.
# ---------------------------------------------------------------------------
def test_sec21_aes_encrypt_cfb128_mode_iv_and_return():
    body = _aes_encrypt_body()

    # The IV is a 16-byte local copy of m_sec->iv with byte 0 overwritten by iv8.
    assert "uint8_t iv0[16];" in body
    assert "memcpy(iv0, m_sec->iv, 16);" in body
    assert "iv0[0] = iv8;" in body
    assert "size_t iv_offset = 0;" in body

    # CFB128 ENCRYPT, in-place (crypt_data is both src and dst).
    assert (
        "mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0,\n"
        "                                       crypt_data, crypt_data)" in body
    ), "encrypt must be MBEDTLS_AES_ENCRYPT CFB128 in-place"

    # Return contract: crypt_len on success, the raw ret on failure.
    assert "if (ret == 0) {" in body
    assert "return crypt_len;" in body
    assert "return ret;" in body


# ---------------------------------------------------------------------------
# SEC22: _aes_decrypt's invalid-arg guard accepts crypt_len == 0 (it only
#        rejects crypt_len < 0) but otherwise mirrors encrypt. CRITICALLY: the
#        guard reads m_sec FIRST, so the error path must NOT dereference
#        m_sec->aes (which is null exactly when the guard fires on !m_sec) — that
#        would be a NULL deref. The error log must be a bare string.
# ---------------------------------------------------------------------------
def test_sec22_aes_decrypt_invalid_args_guard_no_null_deref_in_log():
    body = _aes_decrypt_body()

    assert "if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {" in body
    # decrypt tolerates a zero-length buffer (rejects only negative).
    assert "crypt_len <= 0" not in body, (
        "decrypt must reject only crypt_len < 0, not <= 0 (0 is a valid no-op)"
    )

    guard_idx = body.index("if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {")
    ret_invalid = body.index("return -ESP_ERR_INVALID_ARG;", guard_idx)

    # Isolate the ESP_LOGE statement inside the invalid-arg block (it may span
    # multiple physical lines, up to its closing ");"). The guard line itself
    # legitimately reads `!m_sec->aes`, so we must scope to the LOG call only.
    err_block = body[guard_idx:ret_invalid]
    log_match = re.search(r"ESP_LOGE\((?:[^;]*?)\);", err_block, re.DOTALL)
    assert log_match is not None, "the invalid-arg path must log an error"
    log_stmt = log_match.group(0)

    # REGRESSION LOCK: the invalid-arg error log must NOT dereference m_sec->aes.
    # When the guard fires because m_sec is null, `m_sec->aes` would crash. The
    # log must be a bare string (matching the encrypt path), never format
    # m_sec->aes as a %p argument. (Source previously logged `... %p ..., m_sec->aes,
    # crypt_data, crypt_len` here — a NULL deref on the !m_sec branch.)
    assert "m_sec->aes" not in log_stmt, (
        "the decrypt invalid-arg log must not dereference m_sec->aes — it is "
        "reached when m_sec itself is nullptr, which would NULL-deref"
    )
    assert 'ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES decryption' in log_stmt

    # Guard returns invalid before the cipher.
    cipher_idx = body.index("mbedtls_aes_crypt_cfb128(")
    assert ret_invalid < cipher_idx


# ---------------------------------------------------------------------------
# SEC23: _aes_decrypt runs CFB128 in DECRYPT mode in-place from the same iv8-
#        seeded IV, returns crypt_len on success and the mbedtls error on
#        failure. Mode must be DECRYPT (not ENCRYPT) so CFB direction is right.
# ---------------------------------------------------------------------------
def test_sec23_aes_decrypt_cfb128_decrypt_mode_iv_and_return():
    body = _aes_decrypt_body()

    assert "uint8_t iv0[16];" in body
    assert "memcpy(iv0, m_sec->iv, 16);" in body
    assert "iv0[0] = iv8;" in body

    assert (
        "mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0,\n"
        "                                       crypt_data, crypt_data)" in body
    ), "decrypt must be MBEDTLS_AES_DECRYPT CFB128 in-place"

    # Decrypt must NOT accidentally use ENCRYPT mode.
    assert "MBEDTLS_AES_ENCRYPT" not in body, (
        "the decrypt path must not call the cipher in ENCRYPT mode"
    )

    # Return contract: error -> ret, success -> crypt_len.
    assert "if (ret != 0) {" in body
    assert "return crypt_len;" in body


# ---------------------------------------------------------------------------
# SEC24: encrypt and decrypt are SYMMETRIC in IV handling — both copy the
#        session IV, both overwrite byte 0 with iv8, both pass the SAME buffer as
#        src and dst (in-place). A divergence here would break round-tripping.
# ---------------------------------------------------------------------------
def test_sec24_aes_encrypt_decrypt_iv_handling_is_symmetric():
    enc = _aes_encrypt_body()
    dec = _aes_decrypt_body()

    for body in (enc, dec):
        assert "memcpy(iv0, m_sec->iv, 16);" in body
        assert "iv0[0] = iv8;" in body
        assert "size_t iv_offset = 0;" in body

    # Exactly one cipher call each, both in-place (crypt_data, crypt_data).
    assert enc.count("mbedtls_aes_crypt_cfb128(") == 1
    assert dec.count("mbedtls_aes_crypt_cfb128(") == 1
    assert enc.count("crypt_data, crypt_data)") == 1
    assert dec.count("crypt_data, crypt_data)") == 1


# ===========================================================================
# _crc_checksum — esp_crc16_be wiring.
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC25: _crc_checksum computes a CRC16 big-endian over the supplied buffer with
#        a zero seed, returning the 16-bit checksum. The iv8 argument is part of
#        the BLUFI callback ABI but does not seed this CRC (seed is the literal
#        0). This is the integrity check the BLUFI stack uses on framed packets.
# ---------------------------------------------------------------------------
def test_sec25_crc_checksum_uses_esp_crc16_be_zero_seed():
    body = _crc_checksum_body()

    assert "return esp_crc16_be(0, data, len);" in body, (
        "_crc_checksum must delegate to esp_crc16_be(0, data, len)"
    )
    # Sanity: the function returns a uint16_t (the CRC16 width).
    assert "uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len)" in read(BLUFI_CPP)


# ===========================================================================
# Callback wiring — the security primitives are actually registered with the
# BLUFI stack via trampolines (otherwise the whole security layer is dead code).
# ===========================================================================

# ---------------------------------------------------------------------------
# SEC26: the four security primitives are exposed to the BLUFI stack through
#        static trampolines that forward to the singleton instance methods, and
#        those trampolines are registered in the esp_blufi_callbacks_t table.
#        Without this wiring the DH/AES/CRC handlers would never be invoked.
# ---------------------------------------------------------------------------
def test_sec26_security_primitives_are_registered_via_trampolines():
    cpp = read(BLUFI_CPP)

    # Trampolines forward to the singleton's instance methods.
    assert "GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);" in cpp
    assert "return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);" in cpp
    assert "return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);" in cpp
    assert "return _crc_checksum(iv8, data, len);" in cpp

    # The callback table wires each trampoline to its BLUFI slot.
    assert ".negotiate_data_handler = &_negotiate_data_handler_trampoline," in cpp
    assert ".encrypt_func = &_encrypt_func_trampoline," in cpp
    assert ".decrypt_func = &_decrypt_func_trampoline," in cpp
    assert ".checksum_func = &_checksum_func_trampoline," in cpp
    assert "esp_blufi_register_callbacks(&blufi_callbacks);" in cpp


# ---------------------------------------------------------------------------
# SEC27: NO security primitive logs raw key material. The DH handler, the two
#        AES paths and the CRC must never format the PSK, the shared key, the
#        self public key, or the IV into a log line. (Length / error codes are
#        fine; the secret bytes are not.)
# ---------------------------------------------------------------------------
def test_sec27_security_primitives_never_log_key_material():
    secret_buffers = (
        "m_sec->psk",
        "m_sec->share_key",
        "m_sec->self_public_key",
        "m_sec->iv",
        "iv0",
    )
    for body in (
        _dh_handler_body(),
        _aes_encrypt_body(),
        _aes_decrypt_body(),
        _crc_checksum_body(),
        _security_init_body(),
        _security_deinit_body(),
    ):
        for line in body.splitlines():
            if "ESP_LOG" not in line:
                continue
            for buf in secret_buffers:
                assert buf not in line, (
                    f"a log line must not reference secret buffer {buf!r}: {line.strip()!r}"
                )


# ---------------------------------------------------------------------------
# SEC28: the BlufiSecurity struct (blufi.h) declares the key material with the
#        widths the .cpp relies on: a 16-byte PSK (PSK_LEN), a 16-byte IV, a
#        128-byte share key (SHARE_KEY_LEN), and a 128-byte self public key
#        (DH_SELF_PUB_KEY_LEN). A width drift here would silently corrupt the
#        md5/setkey/CFB math in _dh_negotiate_data_handler.
# ---------------------------------------------------------------------------
def test_sec28_blufi_security_struct_widths_match_cpp_expectations():
    h = read(BLUFI_H)
    struct = _slice(h, "struct BlufiSecurity {", "BlufiSecurity *m_sec;")

    assert "#define DH_SELF_PUB_KEY_LEN 128" in struct
    assert "uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];" in struct
    assert "#define SHARE_KEY_LEN 128" in struct
    assert "uint8_t share_key[SHARE_KEY_LEN];" in struct
    assert "#define PSK_LEN 16" in struct
    assert "uint8_t psk[PSK_LEN];" in struct
    assert "uint8_t iv[16];" in struct
    # The DH input buffer is a heap pointer with a paired length.
    assert "uint8_t *dh_param;" in struct
    assert "int dh_param_len;" in struct
    # The mbedtls sub-contexts are heap pointers (constructed in _security_init).
    # AES must use the mbedTLS API-compatible context so software-AES builds keep
    # the same BluFi CFB128 behavior without re-enabling HW AES SRAM pressure.
    assert "mbedtls_dhm_context *dhm;" in struct
    assert "mbedtls_aes_context *aes;" in struct

# ---------------------------------------------------------------------------
# SEC29: Wi-Fi credentials must only be accepted after BluFi DH/AES completed.
#        The SSID and password handlers must reject first, before copying bytes
#        into m_sta_config, so plaintext BLE credential frames are dropped.
# ---------------------------------------------------------------------------
def test_sec29_sta_credentials_are_rejected_before_copy_without_secure_session():
    assert "esp_blufi_send_error_info(" in _credential_guard_body()
    for case_name, destination in (
        ("ESP_BLUFI_EVENT_RECV_STA_SSID", "m_sta_config.sta.ssid"),
        ("ESP_BLUFI_EVENT_RECV_STA_PASSWD", "staged_wifi_credentials_.UpdatePassword"),
    ):
        case_body = _event_case_body(case_name)
        guard_idx = case_body.index("_require_secure_session_for_credentials()")
        write_idx = case_body.index(destination)
        assert guard_idx < write_idx, f"{case_name} must check secure session before staging"
        assert "break;" in case_body[guard_idx:write_idx]

# ---------------------------------------------------------------------------
# SEC31: BluFi CUSTOM_DATA carries bootstrap_token / provisioning_code /
#        claim_device_id and must be rejected before TLV parsing unless DH/AES
#        has completed. This closes the token/custom-data plaintext path.
# ---------------------------------------------------------------------------
def test_sec31_custom_data_is_rejected_before_tlv_parse_without_secure_session():
    case_body = _event_case_body("ESP_BLUFI_EVENT_RECV_CUSTOM_DATA")

    guard_idx = case_body.index("_require_secure_session_for_credentials()")
    data_idx = case_body.index("const uint8_t* data = param->custom_data.data;")
    snapshot_idx = case_body.index("BlufiCustomDataSnapshot snapshot;")
    token_persist_idx = case_body.index('"bootstrap_token", self->bootstrap_token_')
    device_id_persist_idx = case_body.index('"claim_device_id"')

    assert guard_idx < data_idx
    assert guard_idx < snapshot_idx
    assert guard_idx < device_id_persist_idx
    assert guard_idx < token_persist_idx
    assert "break;" in case_body[guard_idx:data_idx]

# ---------------------------------------------------------------------------
# SEC30: The secure-session predicate must prove both DH completion and an AES
#        context, not just allocation of the BlufiSecurity struct on BLE connect.
# ---------------------------------------------------------------------------
def test_sec30_secure_session_flag_set_only_after_dh_aes_success():
    cpp = read(BLUFI_CPP)
    h = read(BLUFI_H)
    dh_body = _dh_handler_body()

    assert "bool m_blufi_security_negotiated;" in h
    assert "m_blufi_security_negotiated = false;" in _security_init_body()
    assert "m_blufi_security_negotiated = false;" in _security_deinit_body()
    setkey_idx = dh_body.index("mbedtls_aes_setkey_enc")
    flag_idx = dh_body.index("m_blufi_security_negotiated = true;")
    assert setkey_idx < flag_idx
    assert "bool Blufi::_require_secure_session_for_credentials()" in cpp


# ---------------------------------------------------------------------------
# SEC32: DH negotiation records sanitized allocator evidence at the two points
#        needed to diagnose controller/mbedTLS INTERNAL|DMA pressure. The
#        snapshot contains numeric heap data only and never key/session data.
# ---------------------------------------------------------------------------
def test_sec32_dh_public_key_path_logs_numeric_heap_snapshots_at_both_boundaries():
    cpp = read(BLUFI_CPP)
    dh_body = _dh_handler_body()
    helper = _slice(cpp, "static void LogBlufiHeapSnapshot", "static std::string GetBlufiDeviceName")

    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in helper
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in helper
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)" in helper
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)" in helper
    for forbidden in ("ssid", "password", "token", "serial", "mac", "key"):
        assert forbidden not in helper.lower()

    make_public_idx = dh_body.index("mbedtls_dhm_make_public(m_sec->dhm")
    before_make_idx = dh_body.index('LogBlufiHeapSnapshot("dh_before_make_public")')
    output_idx = dh_body.index("*output_data = m_sec->self_public_key;")
    before_return_idx = dh_body.index('LogBlufiHeapSnapshot("dh_before_public_key_return")')

    assert before_make_idx < make_public_idx
    assert re.search(
        r'LogBlufiHeapSnapshot\("dh_before_make_public"\);\s*ret\s*=\s*'
        r'mbedtls_dhm_make_public',
        dh_body,
    )
    assert before_return_idx < output_idx
    assert dh_body[before_return_idx:output_idx].strip().endswith(
        'LogBlufiHeapSnapshot("dh_before_public_key_return");'
    )
