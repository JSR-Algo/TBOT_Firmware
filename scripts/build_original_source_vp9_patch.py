#!/usr/bin/env python3
"""Rebuild only the patched VP9 object against a pinned FFmpeg build.

The supplied source and library build are read-only. The output belongs to the
caller; configure flags are preserved, and the complete input identity is saved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

PIN = 'db69d06eeeab4f46da15030a80d539efb4503ca8'
VP9_SHA = 'dec7ed44be2259383da4402c22dd6ee0fc2c90dd56b3a39fa5dd26e6e3b175a2'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--libraries', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    src, lib, out = (p.resolve() for p in (args.source, args.libraries, args.output))
    if out.exists():
        raise SystemExit('Output must be new; refusing to overwrite retained evidence')
    if src.name != 'FFmpeg-' + PIN or digest(src/'libavcodec/vp9.c') != VP9_SHA:
        raise SystemExit('Unexpected pinned FFmpeg VP9 input')
    config = lib/'ffbuild/config.mak'
    values = dict(line.split('=', 1) for line in config.read_text().splitlines()
                  if '=' in line and not line.startswith(('#', '\t', ' ')))
    flags = []
    for key in ('CPPFLAGS', 'CFLAGS'):
        value = values[key].replace('$(SRC_PATH)', str(src))
        if '$' in value:
            raise SystemExit('Unresolved build flag in ' + key)
        flags += shlex.split(value)
    out.mkdir(parents=True)
    patched = out/'vp9.c'
    shutil.copy2(src/'libavcodec/vp9.c', patched)
    patch = Path(__file__).resolve().parents[1]/'third_party/ffmpeg/patches/0001-vp9-guard-unallocated-tile-cleanup.patch'
    subprocess.run(['patch', '--batch', str(patched), str(patch)], check=True)
    command = [values['CC'], '-I'+str(lib), '-I'+str(src),
               '-I'+str(src/'libavcodec'), '-DHAVE_AV_CONFIG_H', '-DBUILDING_avcodec',
               *flags, '-c', str(patched), '-o', str(out/'vp9.o')]
    subprocess.run(command, check=True, cwd=out)
    shutil.copy2(lib/'libavcodec/libavcodec.a', out/'libavcodec.a')
    subprocess.run([values['AR'], 'r', str(out/'libavcodec.a'), str(out/'vp9.o')], check=True)
    inputs = [src/'libavcodec/vp9.c', config, lib/'config.h', lib/'config_components.h',
              lib/'libavcodec/libavcodec.a', lib/'libavformat/libavformat.a', lib/'libavutil/libavutil.a', patch]
    identity = {'ffmpeg_commit': PIN, 'source': str(src), 'libraries': str(lib),
                'command': command, 'compiler': subprocess.check_output([values['CC'], '--version'], text=True),
                'inputs': {str(p): digest(p) for p in inputs},
                'outputs': {p.name: digest(p) for p in (patched, out/'vp9.o', out/'libavcodec.a')}}
    (out/'identity.json').write_text(json.dumps(identity, indent=2)+'\n')
    print('PASS pinned VP9 patch build; immutable dependency inputs preserved')

if __name__ == '__main__':
    main()
