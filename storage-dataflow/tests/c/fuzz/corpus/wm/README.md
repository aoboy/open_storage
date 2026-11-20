# WM profile-update replay corpus (W6.11)

`fuzz_wm_profile FILE...` replays checked-in directed update vectors. Each
vector pins the exact return code and post-update `b[]`, so an accepted update
that changes redistribution or a rejected update that mutates state fails
immediately.

## Format

Little-endian:

```text
8s magic "WMPROF1\0"
u32 n, u32 m
u32 b[n]
u32 e[n]
i32 mode
i32 expected_rc
u32 expected_b[n]
```

Replay rejects malformed headers, `n` outside `[2, WM_MAX_PATHS]`, `m == 0`,
`sum(b) != m`, truncated fields, and trailing bytes. On success it also checks
`sum(b) == m` and that the fleet is not all zero.

## Cases

- `mode1-single-hot.bin` — one hot path redistributed over the fleet.
- `mode2-multi.bin` — multi-path removal.
- `mode3-kbar.bin` — K/K-bar donor/recipient split.
- `mode4-proportional.bin` — proportional renormalization.
- `last-path-zeroed.bin` — a zero-valued path is removed while another remains
  healthy; the fleet stays non-empty.
- `reject-oversized-e.bin` — `e[i] > b[i]` refused without mutation.
- `reject-unknown-mode.bin` — unknown mode refused without mutation.

Regenerate with:

```sh
python3 storage-dataflow/tests/c/fuzz/corpus/wm/generate.py
```

Run through CTest (`wm_profile_replay_corpus`) or directly:

```sh
storage-dataflow/build/tests/c/fuzz_wm_profile \
  storage-dataflow/tests/c/fuzz/corpus/wm/*.bin
```
