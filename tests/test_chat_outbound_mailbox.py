import os
import json
import re
import shlex
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_target_generation_operations_are_bounded(tmp_path):
    commands = ROOT / "build/compile_commands.json"
    if not commands.exists():
        pytest.skip("Target compilation database required")
    entry = next(item for item in json.loads(commands.read_text())
                 if item["file"].endswith("/audio/audio_service.cc"))
    args = shlex.split(entry["command"])
    compiler = Path(args[0])
    if not compiler.exists() or "xtensa" not in compiler.name:
        pytest.skip("Configured Xtensa toolchain required")
    source = tmp_path / "mailbox_probe.cc"
    source.write_text('#include "chat_outbound_mailbox.h"\n'
                      'extern "C" uint32_t Advance(ChatOutboundMailbox* p) { return p->AdvanceGeneration(); }\n'
                      'extern "C" bool Current(const ChatOutboundMailbox* p, uint32_t g) { return p->IsCurrent(g); }\n')
    obj = tmp_path / "mailbox_probe.o"
    args[args.index("-o") + 1] = str(obj)
    args[args.index("-c") + 1] = str(source)
    args.extend(["-I", str(ROOT / "main")])
    subprocess.run(args, cwd=entry["directory"], check=True, timeout=60)
    objdump = str(compiler).replace("g++", "objdump")
    for symbol in ("Advance", "Current"):
        assembly = subprocess.check_output(
            [objdump, "-d", "--disassemble=" + symbol, str(obj)], text=True)
        instructions = re.findall(
            r"^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]+[ \t]+(\S+)[ \t]*([^\n]*)",
            assembly, re.MULTILINE)
        assert instructions, assembly
        assert not any(op.startswith(("call", "loop", "s32c1i"))
                       for _, op, _ in instructions), assembly
        edges = {}
        for index, (address, op, operands) in enumerate(instructions):
            successors = []
            if op.startswith(("b", "j")):
                target = re.search(r"(?:^|[,\s])([0-9a-f]+)\s*<", operands)
                assert target, assembly
                successors.append(int(target[1], 16))
            if not op.startswith(("ret", "j")) and index + 1 < len(instructions):
                successors.append(int(instructions[index + 1][0], 16))
            edges[int(address, 16)] = successors

        def check_acyclic(address, path):
            assert address not in path, assembly
            for successor in edges[address]:
                check_acyclic(successor, path | {address})

        check_acyclic(int(instructions[0][0], 16), set())
        assert any(op.startswith("l32i") for _, op, _ in instructions), assembly
        assert any(op == "memw" for _, op, _ in instructions), assembly
        if symbol == "Advance":
            assert any(op.startswith("s32i") for _, op, _ in instructions), assembly


@pytest.mark.parametrize("sanitizer", ["address,undefined", "thread"])
@pytest.mark.parametrize("instrumented", [False, True])
def test_chat_outbound_mailbox(tmp_path, sanitizer, instrumented):
    header = ROOT / "main/chat_outbound_mailbox.h"
    assert header.exists(), "Chat outbound mailbox implementation is missing"
    if sanitizer == "thread" and os.environ.get("CHAT_MAILBOX_TSAN") != "1":
        pytest.skip("Run separately with CHAT_MAILBOX_TSAN=1")
    include = ROOT / "main"
    flags = []
    if instrumented:
        # Expose internals only in a temporary copy for exhaustion and contention.
        source = header.read_text()
        assert source.count("private:") == 1
        (tmp_path / header.name).write_text(source.replace("private:", "public:"))
        include = tmp_path
        flags = ["-DMAILBOX_TEST_INSTRUMENTED"]
    binary = tmp_path / "chat_outbound_mailbox"
    subprocess.run([
        "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=" + sanitizer, "-fno-omit-frame-pointer", *flags, "-I", str(include),
        "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/chat_outbound_mailbox_test.cc"), "-o", str(binary),
    ], check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=30)
