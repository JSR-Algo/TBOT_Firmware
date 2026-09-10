from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def test_chat_connection_fault_publishers_have_private_reason_markers():
    source = (ROOT / "main/application.cc").read_text()
    sites = list(re.finditer(r"(?:signals|signals_|chat_protocol_signals_)->PublishConnectionFault\(", source))
    assert sites
    for site in sites:
        before = source[max(0, site.start() - 300):site.start()]
        assert re.search(r'ESP_LOGW\(TAG, "chat_source_fault reason=[a-z_]+', before), before
    for line in source.splitlines():
        if 'ESP_LOGW(TAG, "chat_source_fault' in line:
            assert "%s" not in line, "Do not expose messages, transcripts, or credentials"
