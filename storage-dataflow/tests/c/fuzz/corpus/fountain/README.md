# Fountain replay corpus (W6.11)

`fuzz_fountain FILE...` replay mode consumes these raw streams instead of
generating random ones. Replay keeps corpus ESIs exactly as written and checks
`fd_ready()` after every push, so a false-readiness regression is caught at the
operation that caused it.

Each file starts with one codec byte (`0` LT, `1` nanorq), little-endian
`k - 1`, little-endian `sym_len - 1`, then records of `u32 esi` plus `sym_len`
payload bytes. A partial trailing record is ignored.

Files:

- `nanorq-k512-t1-repair-boundary.bin` — repair ESI 512 arrives first, then all
  512 sources. With the historical 512-entry `have[]` bitmap, the repair aliased
  decoder state and reported readiness at rank 1; the current 1,024-entry bitmap
  passes. This file was verified to fail when the old bitmap size is restored.
- `nanorq-k257-t1-source-set.bin` — the supported K=257 boundary.
- `nanorq-k1-t1024-header-only.bin` — no records: not ready, decode refused.
- `lt-k64-t1-source-set.bin` — maximum LT K with one-byte symbols.
- `lt-k1-t256-source-one.bin` — minimum K with the maximum LT symbol size.
- `lt-k8-t8-duplicates-and-repair.bin` — duplicate accounting plus a repair.
- `lt-k4-t4-truncated-record.bin` — valid sources followed by a partial record.

Regenerate with:

```sh
python3 storage-dataflow/tests/c/fuzz/corpus/fountain/generate.py
```

Run through CTest (`fountain_replay_corpus`) or directly:

```sh
storage-dataflow/build/tests/c/fuzz_fountain \
  storage-dataflow/tests/c/fuzz/corpus/fountain/*.bin
```
