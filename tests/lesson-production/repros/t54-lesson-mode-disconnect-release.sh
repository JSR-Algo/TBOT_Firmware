#!/usr/bin/env bash
set -euo pipefail
# repo: robot/TBOT-Firmware

python3 -m pytest -q \
  tests/test_lesson_disconnect_release_contract.py \
  tests/test_goal2_canonical_port_contract.py::test_transport_teardown_abandons_only_the_current_lesson_owner \
  tests/test_lesson_passive_websocket_contract.py::test_passive_liveness_failure_has_one_reconnect_owner_even_if_disconnect_arrives

bash scripts/run_host_native_lesson_handler_test.sh
