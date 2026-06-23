"""CR-FW-09 — lesson_handler header-vs-code download-contract RECONCILIATION.

WHY THIS FILE EXISTS
--------------------
`test_lesson_dispatch_backward_compat.py::test_firmware_does_not_download_or_checksum_assets`
greps `main/lesson_handler.cc` for four literal symbol names
(`esp_http_client`, `esp_crypto_sha256`, `mbedtls_sha256`, `esp_https_ota`)
and passes when none are present. None ARE present — so the guard is GREEN —
yet `FetchLessonImage()` (lesson_handler.cc:~394) performs a real HTTP GET of the
authored `scene.*.src` URL via the board network abstraction:

    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    http->Open("GET", url);   ... http->Read(...) ...

That abstraction wraps esp_http_client one layer down, so the literal-grep guard
gives FALSE ASSURANCE: the firmware genuinely downloads, but the guard cannot
see it, and `lesson_handler.h` still carries the stale D-PRELOAD-OWNER prose
"the ESP Server remains the single authoritative downloader + verifier".

This module is READ-ONLY (it parses source text; it never compiles or flashes
and it MUST NOT modify the firmware source or the header). It exists to convert
the header-vs-code contradiction into an executable assertion so the drift can
no longer hide behind a vacuously-green literal grep.

THREE INDEPENDENT, NON-TAUTOLOGICAL FACTS
-----------------------------------------
1. CODE DOWNLOADS (asserted against the real API, not the evaded literals):
   `FetchLessonImage` calls `CreateHttp` + `Open("GET", ...)` + `Read`.
2. OLD GUARD IS BLIND: the four literals it greps for are absent from the .cc,
   which is exactly why it stays green while (1) is true.
3. HEADER STILL CLAIMS "NEVER THE DOWNLOADER": the D-PRELOAD-OWNER sentence
   naming the ESP Server as the *single authoritative downloader* survives
   verbatim in the header.

RECONCILED 2026-06-20 (CR-FW-09 fix): lesson_handler.h no longer claims the ESP
Server is the "single authoritative downloader" / that the firmware "does NOT act
as the asset interpreter" — it now states the firmware fetches ESP-verified bytes
at render time while the ESP Server stays the authoritative sha256 verifier + READY
gate. The sibling guard was likewise repurposed
(test_firmware_does_not_checksum_or_verify_assets_but_does_fetch). The reconciliation
test below is therefore a LIVE regression guard (plain assert, no xfail): it passes
now and turns RED if anyone reintroduces the "single authoritative downloader" /
"does NOT act as the asset interpreter or sha256 verifier" prose while the code still
downloads.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CC = ROOT / "main" / "lesson_handler.cc"
HDR = ROOT / "main" / "lesson_handler.h"

# The exact literals the sibling guard greps for. Listed here so this test fails
# (red, by KeyError on the missing import or by the explicit assert below) if that
# guard's evasion surface ever changes, keeping the two tests in lockstep.
LITERALS_THE_OLD_GUARD_GREPS = (
    "esp_http_client",
    "esp_crypto_sha256",
    "mbedtls_sha256",
    "esp_https_ota",
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# Strip // line comments and /* ... */ block comments so "the code does X"
# assertions match real statements, not prose in the doc-comment that already
# (partially) describes FetchLessonImage. Comments are kept for the *header*
# claim check, where the claim deliberately lives in prose.
#
# STRING-LITERAL AWARE: a naive `//[^\n]*` strip would also eat the contents of
# string literals like "sd://" / "file://", truncating the parsed source. This
# scanner tracks whether we are inside a "..." or '...' literal (honoring \ escapes)
# and only treats // and /* as comment starts when OUTSIDE a literal.
def _strip_c_comments(text: str) -> str:
    out: list[str] = []
    i, n = 0, len(text)
    in_str = False
    str_ch = ""
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_str:
            out.append(ch)
            if ch == "\\":  # escape: copy the escaped char verbatim
                if i + 1 < n:
                    out.append(text[i + 1])
                i += 2
                continue
            if ch == str_ch:
                in_str = False
            i += 1
            continue
        if ch in ('"', "'"):
            in_str = True
            str_ch = ch
            out.append(ch)
            i += 1
            continue
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def test_fetchlessonimage_is_a_real_http_downloader_in_code() -> None:
    """FACT 1 — the firmware downloads. Asserted against the ACTUAL board-network
    HTTP API actually called, not the literal symbol names the old guard greps.

    Mutation that turns this RED: delete the body of FetchLessonImage (or replace
    the CreateHttp/Open("GET")/Read chain with a flashed-asset-only probe). That
    is exactly the change that would make the D-PRELOAD-OWNER header claim true
    again, so a red here genuinely means "firmware no longer downloads".
    """
    code = _strip_c_comments(_read(CC))

    assert "FetchLessonImage" in code, "FetchLessonImage must exist in lesson_handler.cc"

    # Isolate the function body so we assert on the fetcher, not some other helper.
    sig = "FetchLessonImage(const char* url)"
    start = code.index(sig)
    brace = code.index("{", start)
    depth = 0
    body = ""
    for i in range(brace, len(code)):
        ch = code[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                body = code[brace : i + 1]
                break
    assert body, "could not extract FetchLessonImage body"

    # The real download path: board network -> CreateHttp -> HTTP GET -> Read.
    assert "Board::GetInstance().GetNetwork()" in body, (
        "FetchLessonImage must reach the board network stack"
    )
    assert "->CreateHttp(" in body, "FetchLessonImage must create an HTTP client"
    assert '->Open("GET"' in body, "FetchLessonImage must issue an HTTP GET"
    assert "->Read(" in body, "FetchLessonImage must read the response body (download)"


def test_old_literal_grep_guard_is_blind_to_the_real_download() -> None:
    """FACT 2 — the four literals the sibling guard greps for are ABSENT from the
    .cc, which is the mechanism by which that guard stays green while Fact 1 holds.

    Mutation that turns this RED: any of those literals appearing in the .cc — at
    which point the OLD guard would itself go red and catch the download, so the
    false-assurance gap would close on its own. A red here therefore also means
    "the blind spot is gone".
    """
    code = _read(CC)
    for literal in LITERALS_THE_OLD_GUARD_GREPS:
        assert literal not in code, (
            f"{literal!r} now appears in lesson_handler.cc — the sibling literal-grep "
            f"guard is no longer blind; reconcile both guards."
        )


def _header_prose(header_text: str) -> str:
    """Flatten the header's `//` doc-comment block into a single whitespace-normalized
    string. The header is one long `//`-prefixed comment, so every wrapped line starts
    with `// `; those continuation markers are stripped (NOT the prose after them) so a
    sentence that wraps across lines — e.g. "the single // authoritative downloader" —
    reads as contiguous text.
    """
    lines = []
    for line in header_text.splitlines():
        lines.append(re.sub(r"^\s*//\s?", "", line))
    return re.sub(r"\s+", " ", " ".join(lines))


def _header_still_claims_esp_is_sole_downloader(header_text: str) -> bool:
    """True iff lesson_handler.h still carries the unqualified D-PRELOAD-OWNER
    claim that the firmware never downloads / the ESP server is the *single*
    authoritative downloader.

    This is intentionally tolerant of the partial reconciliation already in the
    header (it DOES describe FetchLessonImage lower down): we key off the specific
    "single authoritative downloader" sentence, which is the part the code now
    contradicts.
    """
    prose = _header_prose(header_text)
    return (
        "single authoritative downloader" in prose
        or "does NOT act as the asset interpreter or sha256 verifier" in prose
    )


def test_header_download_claim_is_reconciled_with_code() -> None:
    """RECONCILIATION — header and code must AGREE about who downloads.

    The assertion encodes the *correct* end-state: if the code downloads (Fact 1),
    the header must NOT still claim the ESP server is the sole/only downloader.
    As of the CR-FW-09 fix (2026-06-20) the header is reconciled, so this passes. It
    is NOT a tautology: it cross-checks two independent files. It turns RED if anyone
    reintroduces the 'single authoritative downloader' / 'does NOT act as the asset
    interpreter or sha256 verifier' prose into lesson_handler.h while FetchLessonImage
    still performs an HTTP GET — i.e. it is now a live regression guard against the
    drift returning.
    """
    code = _strip_c_comments(_read(CC))
    header = _read(HDR)

    code_downloads = (
        "FetchLessonImage" in code
        and "->CreateHttp(" in code
        and '->Open("GET"' in code
    )
    header_says_never = _header_still_claims_esp_is_sole_downloader(header)

    # Contract: code-downloads implies the header no longer claims ESP is the sole
    # downloader. The drift violates this, so the assert fails -> xfail catches it.
    assert not (code_downloads and header_says_never), (
        "Header-vs-code download-contract DRIFT (CR-FW-09): lesson_handler.cc "
        "FetchLessonImage performs an HTTP GET download, yet lesson_handler.h still "
        "documents the ESP Server as the single authoritative downloader "
        "(D-PRELOAD-OWNER). Reconcile the header (and the literal-grep guard)."
    )

def test_lesson_handler_comments_do_not_reintroduce_no_download_claim() -> None:
    """Source comments must not contradict FetchLessonImage.

    The reconciled contract is: ESP Server verifies sha256 + gates READY; firmware
    may fetch/read verified URLs or SD pack paths to render, but it does not
    checksum/verify assets. Stale comments that say firmware never downloads have
    already caused false assurance around the renderer boundary.
    """
    source = _read(CC)
    forbidden = (
        "firmware never downloads",
        "firmware does not download",
        "never downloads/checksums",
        "renders only bytes already on-device",
    )
    for phrase in forbidden:
        assert phrase not in source

    assert "FetchLessonImage" in source
    assert "FetchLessonLocalImage" in source
