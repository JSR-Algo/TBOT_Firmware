"""Regression locks for MCP tools/list pagination on low-SRAM TLS links.

The LCDWiki ESP32-S3 can have only a few KB of free internal SRAM while audio is
active. A single ~8 KB MCP tools/list response can make mbedTLS fail allocation
inside the encrypted websocket send path, which drops the connection and shows
"server unavailable" on the robot. Keep each page small and observable.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/mcp_server.cc"


def read_source() -> str:
    return SOURCE.read_text(encoding="utf-8")


def test_mcp_tools_list_pages_are_small_enough_for_tls_under_low_sram():
    source = read_source()

    match = re.search(r"kMcpToolsListMaxPayloadBytes\s*=\s*(\d+)", source)

    assert match, "tools/list must use a named byte limit, not an inline large value"
    assert int(match.group(1)) <= 3000
    assert "const int max_payload_size = 8000" not in source


def test_mcp_tools_list_logs_page_size_before_replying():
    source = read_source()

    body = source[source.index("void McpServer::GetToolsList") : source.index("void McpServer::DoToolCall")]

    assert "tools/list page bytes=" in body
    assert "next_cursor=" in body
