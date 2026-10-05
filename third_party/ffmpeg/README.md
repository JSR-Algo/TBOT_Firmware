# Original-source decoder dependency checkpoint

Pin: FFmpeg db69d06eeeab4f46da15030a80d539efb4503ca8. This directory contains a
narrow source patch, not a separately qualified firmware capability. Existing
H264/VP9/PNG semantics, original media bytes and codec configuration are retained.

`0001-vp9-guard-unallocated-tile-cleanup.patch` guards the tile-array owner during
teardown when the first tile allocation fails. `active_tile_cols` has already
been set in the header parser, but `td` can still be NULL. Existing cleanup of
successfully allocated buffers remains unchanged.

Build against the pinned source and its complete native build:

```sh
python3 scripts/build_original_source_vp9_patch.py \
  --source /absolute/path/FFmpeg-db69d06eeeab4f46da15030a80d539efb4503ca8 \
  --libraries /absolute/path/pinned-build \
  --output /absolute/path/new-owned-output
```

The builder verifies the original VP9 source hash, applies the patch only to an
owned copy, preserves the supplied config's compiler/flags, replaces one object
in an owned archive copy and writes `identity.json`. It does not mutate the
source/config/archive inputs. The host session runner uses this builder with
ASan/UBSan libraries; use all fault shards, and sum checked allocation counts
across all seven originals. `ORIGINAL_SOURCE_CANCEL_ONLY` is diagnostic-only and
never establishes exhaustive allocation proof.

## Allocation routing

`main/lesson_original_source_allocator.{h,cc}` is the single serialized owner of
the `tbot_original_*` hooks. On ESP_PLATFORM it allocates with
`heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and never
falls back to internal RAM; native callers supply an explicit backend.

Build pinned `libavutil/mem.c` with `-DMALLOC_PREFIX=tbot_original_
-DTBOT_FFMPEG_ALLOC_FAILURE` after applying
`0002-avutil-notify-early-allocation-refusal.patch`. Without the patch, size
limit, multiplication overflow, fast-allocation and dynarray refusals return
NULL/ENOMEM without reaching the hooks, so the session's sticky failure state is
never set. `av_size_mult` and legal zero-size operations do not notify.

Build pinned zlib with `-DMY_ZCALLOC -Dzcalloc=tbot_original_zcalloc
-Dzcfree=tbot_original_zcfree`. PNG already uses FFmpeg's allocation wrapper,
but Matroska zlib track compression (`inflateInit`) and MOV `cmov`
(`uncompress`) keep zlib's default allocator, which otherwise calls libc malloc.

Native proofs: `run_host_native_lesson_original_source_allocator_test.sh`,
`..._ffmpeg_allocator_test.sh` (host posix_memalign and target memalign
branches) and `..._zlib_allocator_test.sh`; set `ORIGINAL_ALLOCATOR_UNPATCHED=1`
to reproduce the unrouted failures. Stdio file input (`fopen`/`fdopen`) remains
outside the allocator until the leased SD input replaces it.

The session is a serialized portable decoder checkpoint. It does not yet bind
LessonAssetReadLease, route allocations to ESP-IDF heap capabilities, register a
scene runtime or advertise READY. Target footprint, RAM coexistence, timeline,
DMA and physical performance require their separate integration gates. Never
relabel an existing renderer identity or change partitions to hide a fit failure.
