import copy
import hashlib
import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "hil_storage_identity_contract.py"


def load_contract():
    spec = importlib.util.spec_from_file_location("hil_storage_identity_contract", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def contract():
    return load_contract()


@pytest.fixture
def available_identity():
    return {
        "status": "available",
        "kind": "sdmmc-fat",
        "cidFingerprint": "1b-534d-3030303030-10-4a5f7d3d-17b",
        "cid": {
            "manufacturerId": 0x1B,
            "oemId": 0x534D,
            "productName": "00000",
            "revision": 0x10,
            "serial": 0x4A5F7D3D,
            "manufacturingDate": 0x17B,
        },
        "capacitySectors": 62333952,
        "sectorSizeBytes": 512,
        "capacityBytes": 31914983424,
        "mountGeneration": 1,
        "volumeSerial": "7a31f09c",
        "volumeLabel": "TBOT_HIL",
    }


def legacy_status():
    return {
        "status": "idle",
        "cacheKey": "",
        "armed": False,
        "reached": False,
        "consumed": False,
        "operation": "evict",
        "checkpoint": "before_first_unlink",
        "action": "fail",
        "threshold": 0,
        "declaredAssetBytes": 0,
        "pauseSeconds": 0,
        "armSequence": 0,
        "reachedSequence": 0,
        "consumedSequence": 0,
    }


def legacy_inspect():
    return {
        "cacheKey": "hil-task14/v1-" + "a" * 64,
        "siblingCacheKey": "hil-task14/v2-" + "b" * 64,
        "status": "inspected",
        "truncated": False,
        "entries": [],
    }


def test_v1_status_and_inspect_are_unchanged(contract):
    assert contract.validate_status_response(legacy_status(), schema_version=1) == legacy_status()
    assert contract.validate_inspect_response(legacy_inspect(), schema_version=1) == legacy_inspect()


def test_v2_status_and_inspect_accept_exact_available_identity(contract, available_identity):
    status = {**legacy_status(), "schemaVersion": 2, "storageIdentity": available_identity}
    inspect = {**legacy_inspect(), "schemaVersion": 2, "storageIdentity": available_identity}
    assert contract.validate_status_response(status, schema_version=2) == status
    assert contract.validate_inspect_response(inspect, schema_version=2) == inspect


@pytest.mark.parametrize("state", ["unavailable", "invalid", "card_swapped"])
def test_v2_accepts_exact_failure_state_without_partial_identity(contract, state):
    value = {"status": state, "kind": "sdmmc-fat"}
    response = {**legacy_status(), "schemaVersion": 2, "storageIdentity": value}
    assert contract.validate_status_response(response, schema_version=2) == response


@pytest.mark.parametrize(
    "field,bad",
    [
        ("cidFingerprint", "1B-534D-3030303030-10-4A5F7D3D-17B"),
        ("cidFingerprint", "1b534d"),
        ("volumeSerial", "7A31F09C"),
        ("volumeSerial", "00000000"),
        ("capacitySectors", True),
        ("sectorSizeBytes", 512.0),
        ("capacityBytes", 1),
        ("mountGeneration", 0),
        ("volumeLabel", "TBOT\nHIL"),
    ],
)
def test_v2_rejects_malformed_identity(contract, available_identity, field, bad):
    identity = copy.deepcopy(available_identity)
    identity[field] = bad
    response = {**legacy_status(), "schemaVersion": 2, "storageIdentity": identity}
    with pytest.raises(ValueError):
        contract.validate_status_response(response, schema_version=2)


def test_v2_rejects_missing_unknown_and_partial_fields(contract, available_identity):
    for identity in (
        {key: value for key, value in available_identity.items() if key != "cid"},
        {**available_identity, "unexpected": 1},
        {"status": "invalid", "kind": "sdmmc-fat", "volumeSerial": "7a31f09c"},
    ):
        response = {**legacy_status(), "schemaVersion": 2, "storageIdentity": identity}
        with pytest.raises(ValueError):
            contract.validate_status_response(response, schema_version=2)


def test_v2_rejects_malformed_cid_fields(contract, available_identity):
    mutations = [
        ("manufacturerId", 0),
        ("oemId", True),
        ("productName", ""),
        ("productName", "BAD\x01"),
        ("revision", 256),
        ("serial", 0),
        ("manufacturingDate", 0),
    ]
    for field, bad in mutations:
        identity = copy.deepcopy(available_identity)
        identity["cid"][field] = bad
        response = {**legacy_status(), "schemaVersion": 2, "storageIdentity": identity}
        with pytest.raises(ValueError):
            contract.validate_status_response(response, schema_version=2)


@pytest.mark.parametrize("product_name", ["FOUR", "SIX123"])
def test_v2_rejects_non_fixed_width_product_name(
    contract, available_identity, product_name
):
    identity = copy.deepcopy(available_identity)
    identity["cid"]["productName"] = product_name
    identity["cidFingerprint"] = (
        "1b-534d-"
        + product_name.encode("ascii").hex()
        + "-10-4a5f7d3d-17b"
    )
    response = {**legacy_status(), "schemaVersion": 2, "storageIdentity": identity}
    with pytest.raises(ValueError):
        contract.validate_status_response(response, schema_version=2)


def test_schema_versions_and_exact_top_level_fields_are_enforced(contract, available_identity):
    v2 = {**legacy_status(), "schemaVersion": 2, "storageIdentity": available_identity}
    for value, expected in ((v2, 1), (legacy_status(), 2)):
        with pytest.raises(ValueError):
            contract.validate_status_response(value, schema_version=expected)
    with pytest.raises(ValueError):
        contract.validate_status_response({**v2, "unknown": 1}, schema_version=2)
    with pytest.raises(ValueError):
        contract.validate_status_response(v2, schema_version=True)


def test_identity_comparison_detects_card_swap(contract, available_identity):
    after = copy.deepcopy(available_identity)
    after["cid"]["serial"] += 1
    after["cidFingerprint"] = "1b-534d-3030303030-10-4a5f7d3e-17b"
    with pytest.raises(ValueError, match="physical SD identity changed"):
        contract.require_same_storage_identity(available_identity, after)
    assert contract.require_same_storage_identity(available_identity, copy.deepcopy(available_identity))


def test_integrated_helper_is_exact_frozen_candidate():
    assert hashlib.sha256(MODULE_PATH.read_bytes()).hexdigest() == (
        "291b849cfd7aaf0347df6e63dcf35c3863931c4556a93ed9abaa064a0c880772"
    )
