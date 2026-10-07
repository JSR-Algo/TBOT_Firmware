#!/usr/bin/env python3
"""Configuration audit for the distinct, attended M1 staging admission profile."""
from pathlib import Path
import sys

from assert_lcdwiki_prod_config import configuration_failures


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("sdkconfig")
    try:
        failures = configuration_failures(path.read_text(), profile="m1-staging")
    except OSError:
        print("Missing staging sdkconfig", file=sys.stderr)
        return 2
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("LCDWiki M1 staging configuration OK; not production admission")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
