# ARC replay corpus (W6.11)

`replay_arc_stream` reads the files here as exact operation scripts. Every
submit records its payload identity, expected return code, producer timestamp
(`S_n`), and receiver timestamp (`A_n`), so replay never samples the wall clock.
Every poll records the receiver time explicitly.

## Files

- `qg-park-632.bin` minimizes the W5 QG park/stranding failure from fuzz
  iteration 632 (`rng_state = 632 * 0x9E3779B97F4A7C15 + 1`). Six entries
  include multiple absurd-future stamps. The pre-fix drain used `break` after a
  re-queue and stranded entries; this corpus fails that behavior and passes the
  current `continue` drain.
- `saturation-retry.bin` pins the W6.9 saturation path at pacer level. Two
  identities fill both slab slots, a third is refused with `EAGAIN`, the first
  two release, and the third is then accepted and released. Unique payload
  bytes prove exactly-once identity and no substitution.

## Binary format

All integers are little-endian. The 96-byte header is:

```text
8s magic "ARCSTRM1"
u32 capacity, entry_max, batch_max, mode
u64 min_smooth_bytes, U, delta, gamma, G, T_idle_ns, batch_deadline_ns, seed
u32 op_count, expected_release_count
```

Operations follow:

```text
0x01 submit: u32 identity, u32 length, i32 expected_rc, u64 S_n, u64 A_n, bytes[length]
0x02 poll:   u64 now_ns
```

Replay rejects malformed counts, over-limit geometry, truncated payloads,
unknown tags, trailing bytes, duplicate identities with different payloads,
unexpected return codes, non-unique accepted payloads, duplicate releases, QG
reordering, stranded entries, non-finite estimator state, or a release count
different from the recorded expectation. The seed is provenance only; replay
does not regenerate operations from it.

Regenerate both files with:

```sh
python3 storage-dataflow/tests/c/fuzz/corpus/arc/generate.py
```

Run them through CTest (`arc_replay_park_632`) or directly:

```sh
storage-dataflow/build/tests/c/replay_arc_stream \
  storage-dataflow/tests/c/fuzz/corpus/arc/qg-park-632.bin \
  storage-dataflow/tests/c/fuzz/corpus/arc/saturation-retry.bin
```
