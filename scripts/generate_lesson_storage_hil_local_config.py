#!/usr/bin/env python3
"""Generate the uncommitted LAN-only URL overlay for lesson-storage HIL."""

import argparse
import ipaddress
import os
import tempfile
from pathlib import Path
from urllib.parse import urlsplit


PRIVATE_NETWORKS = (
    ipaddress.ip_network("10.0.0.0/8"),
    ipaddress.ip_network("172.16.0.0/12"),
    ipaddress.ip_network("192.168.0.0/16"),
    ipaddress.ip_network("fc00::/7"),
)


class ConfigError(ValueError):
    pass


def _validate_url(value: str, *, label: str, schemes: tuple[str, ...]) -> str:
    if not value or any(not 0x21 <= ord(character) <= 0x7E for character in value):
        raise ConfigError(f"invalid {label} URL")
    if "\\" in value or not value.startswith(tuple(f"{scheme}://" for scheme in schemes)):
        raise ConfigError(f"invalid {label} URL")

    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise ConfigError(f"invalid {label} URL") from error

    if parsed.scheme not in schemes or not parsed.netloc or parsed.query or parsed.fragment:
        raise ConfigError(f"invalid {label} URL")
    if parsed.username is not None or parsed.password is not None or parsed.hostname is None:
        raise ConfigError(f"invalid {label} URL")
    if port is None and parsed.netloc.endswith(":"):
        raise ConfigError(f"invalid {label} URL")
    if port is not None and port < 1:
        raise ConfigError(f"invalid {label} URL")

    try:
        host = ipaddress.ip_address(parsed.hostname)
    except ValueError as error:
        raise ConfigError(f"{label} URL host must be a private LAN address") from error
    if not any(host in network for network in PRIVATE_NETWORKS):
        raise ConfigError(f"{label} URL host must be a private LAN address")

    return value


def _kconfig_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _write_atomic(output: Path, contents: str) -> None:
    output = output.expanduser()
    output.parent.mkdir(parents=False, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        directory_fd = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ota-url", required=True)
    parser.add_argument("--websocket-url", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    arguments = parser.parse_args()

    try:
        ota_url = _validate_url(arguments.ota_url, label="OTA", schemes=("http",))
        websocket_url = _validate_url(
            arguments.websocket_url,
            label="WebSocket",
            schemes=("ws",),
        )
        if arguments.validate_only:
            return 0
        if arguments.output is None:
            raise ConfigError("output is required unless --validate-only is used")
        contents = (
            "CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT=y\n"
            f'CONFIG_OTA_URL="{_kconfig_string(ota_url)}"\n'
            f'CONFIG_WEBSOCKET_URL="{_kconfig_string(websocket_url)}"\n'
        )
        _write_atomic(arguments.output, contents)
    except (ConfigError, OSError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
