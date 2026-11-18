# Vendored dependencies

| Directory | Purpose |
|---|---|
| `unity/` | C unit test framework (placeholder) |
| `libfuzzer/` | System clang libFuzzer integration (placeholder) |
| `nanorq/` | RaptorQ codec (RFC 6330), vendored from the wack_a_mole-validated tree @ a0f0422 (import map §7): source copied, no prebuilt artifacts committed (`*.o`/`*.a` are gitignored; `make -C third_party/nanorq libnanorq.a` builds from source). The upstream reference previously cited here (`ursoid-tech/nanorq`) was corrected per the import map — the codec A/B must not introduce a second, untested upstream. |

Vendored trees are added by the build owner. Nothing is fetched during a
normal build.
