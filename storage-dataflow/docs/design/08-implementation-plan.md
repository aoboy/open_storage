# 08 — Implementation Plan (import-led, waves W0–W5)

**Document type:** executable backlog — self-contained work packages
("prompts") suitable for assignment to an engineer or an autonomous coding
agent, in the wack_a_mole plan style.
**Sources:** [WAM] `research/Whack-a-Mole.pdf`, [FLUID]
`research/FLUID_Slack-based-Low-latency-Delivery.pdf`, [ARC]
`research/arc_adaptive_release_control.pdf`; implementation source of
record: the wack_a_mole repository (see
[07-wam-import-map](07-wam-import-map.md)).
**Related:** [00-overview](00-overview.md) (design baseline),
[09-origin-driver-seam](09-origin-driver-seam.md),
ADRs [0001–0009](../adr/).

---

## IMPLEMENTATION STATUS (informative — judgement; as of the first green hosted run at 0d6e60a; earlier rows cite their landing commits)

  W0.1 repo + discipline     : done — delivered with this plan (git init,
                                ignore rules, CONTRIBUTING.md, this backlog)
  W0.2 common primitives     : done — ring/seqlock/pool/timer/shm/log
                                (src: f53c872, tests: cf361ab); ring soak +
                                seqlock contention legs TSan-clean
  W0.3 strict TOML loader     : done — K.2 parser core imported; sdf key
                                schema enforced at load; 9 shipped
                                examples, 32 rejection cases (2a17946,
                                c0dd25f, d40f066)
  W0.4 golden-parity harness  : done — vectors extracted from the WAM
                                inline core; test_parity validates 152
                                comparisons, 2 seed groups pending W2.1;
                                arc/fountain schemas staged (d20f5a8,
                                d106332)
  W0.5 metrics + gates        : done — metric primitives with the
                                bucket map pinned by test_metrics (4-
                                thread exact totals); bench_wm records
                                2.4-2.8 cyc/decision (gate activates
                                W2.6; bench_arc lands W4.2); Makefile
                                ci/asan/tsan gates, `make ci` green from
                                a clean checkout — 10/10 x {default,
                                ASan+UBSan, TSan} (394c597, d9b3c6e,
                                ad0cf6e, 99fdc31)

  WAVE 0 COMPLETE — milestone M0 reached: ci green from clean checkout,
  parity harness validating 152 goldens, strict TOML examples loading.

  W1.1 origin.h ABI          : done — vtable + stats contract with
                                layout pins in test_abi (include:
                                ce5e63e)
  W1.2 loopback driver       : done — deadline scheduler, zero-copy
                                mmap completions, live knobs, drain,
                                EAGAIN, close flush (7ecb66c, 220f58b)
  W1.3 HTTP driver           : done — per-origin workers, persistent
                                conns, in-process server test found
                                keep-alive starvation live (b6de4aa,
                                1428f37)
  W1.4 loadgen + profiles    : done — steady ok==assigned exact, dies/
                                flapping enolink halves, slow throttle
                                in the EWMA (5411ff2, ffc46fd)

  WAVE 1 COMPLETE — M1 reached (both drivers + the harness).

  W2.1 spray methods+seeds    : done — ADR-0007 on the ABI (methods,
                                seeds, path_of_key LUT, rotation;
                                version 0.1): parity gate went live —
                                280/280 golden comparisons, ZERO
                                pending groups; discrepancy sweeps <= l
                                / <= 2l; bench 4.7-4.9 cyc/dec
                                (4915b27, cf7e6f5, 6a90d13)
  W2.2 updates + controller   : done — wm_profile_update embodiments
                                1-4 with the residual cursor and the
                                last-healthy guard; severity
                                controller on sdf_origin_stats_t with
                                theta bands, auto embodiment, t_min
                                rate limit (d2c5da1, e2ffa36)
  W2.3 SHM publication       : done — ADR-0002 double-buffer +
                                seqlock flip, telemetry ring, down_mask
                                deference; TSan caught plain-word
                                races -> atomic-access discipline
                                (908d2ab, d537efc)
  W2.4 Python control loop    : done — mmap mirror of the segment,
                                publish/acquire roundtrip, telemetry
                                drain, no-opinion + down_mask
                                deference, heartbeat; cross-language
                                ABI proven live both directions
                                (d4b7b94, 750f2b2, c1b0bf7); CFFI
                                header-graph fix + the origin utility
                                its full link exposed
  W2.5 cache-fill proxy       : done — job-split units, per-job
                                seeded spray (one compile per job), A/B
                                via xxhash64 buckets, controller in
                                the dispatch loop, kill switch;
                                integration test pins the acceptance
                                (f398a09, 35a73e7)
  W2.6 P1 e2e + gates         : done — e2e_cache_fill over all four
                                degradation profiles + bytes-neutral
                                arms, green 4x consecutively; bench
                                gate active (seeded spray over 30 cyc
                                fails the bench leg) (def1e48,
                                70bb9ed, b613f69); found + fixed en
                                route: drain is controller-visible
                                (set_enable drives hard_down) — the
                                mid-run whack had flaked on warm-up
                                latency noise, now deterministic 5/5

  WAVE 2 COMPLETE — milestone M2 reached: the P1 prototype is whole —
  seeded dispatch with golden-proven parity, graded shedding, the
  C/Python control split over SHM, the runnable proxy, and e2e gates
  green under every degradation profile.

  W3.1 fountain ABI v2        : done — size helpers + in-place init +
                                explicit ESI semantics + fd_rank;
                                the toy codec proves the surface and
                                its goldens (314 comparisons at the
                                time); SDF_VERSION 0.2 (ec3c8ad)
  W3.2 nanorq vendored + arm  : done — vendored from the wack_a_mole-
                                validated tree (pin corrected per the
                                import map); block-level adapter over
                                the public ABI; lifecycle + arm
                                selector added to the ABI when the
                                adaptation exposed them; live finds:
                                complete-block IGN semantics and the
                                max_esi cap; 335 comparisons zero
                                pending (26b133f, 39e8280)
  W3.3 LT reference arm        : done — systematic LT, robust-soliton
                                degrees with the tiny-K fallback,
                                deterministic per-ESI neighbor sets,
                                peeling decoder; 356 comparisons
                                zero pending; the probe lesson:
                                unsequenced printf args faked a decode
                                mismatch (b262e8c, bc12e69)
  W3.4 FLUID both halves       : done — budget N with the snap +
                                efficiency/cost bounds + count feedback
                                (monotone-max); the gate as the
                                telescoped quotient of the section-4.3
                                chain (the multiplicative fold wedges at
                                zero — the identity test caught it);
                                slack certification + stale-reject
                                (cad9aa7, 49653a1)
  W3.5 repair coordinator      : done — the design-02 loop over the
                                origin seam with per-ESI completion
                                events through an SPSC ring, stream =
                                exactly the budget N, a round = the
                                whole budget in flight, mandatory ARQ
                                fallback through the origins (6e9d83c,
                                0aaa36e)
  W3.6 three-arm A/B + ladder : done — fluid-repair shell + e2e over
                                3 arms x loss ladder {0..50%} + the
                                eps sweep, byte-exact via exit-code
                                gates, Pareto recorded; overhead grows
                                past the slack, control retry cost
                                visible at 50% loss (33806b3). The
                                repair e2e now renders
                                `out/bench/repair_pareto.csv` and `.svg`
                                from the recorded `REPAIR` lines through
                                `sdf.bench.plots`, with no matplotlib
                                dependency (recorded wall time vs
                                overhead, not a synthesized p99). The
                                Python `fluid_coord` now mirrors the
                                sender budget and cumulative-loss gate
                                (C-parity tested), assigns control or
                                class-primary arms by xxhash64 bucket,
                                and tracks the Pareto frontier;
                                `sdf.common.ab` carries a verified
                                pure-Python XXH64 fallback so the
                                control plane does not require the
                                optional wheel at import time.

  WAVE 3 COMPLETE — milestone M3 reached: the P2 repair prototype is
  whole — both codec arms golden-pinned behind one ABI, the FLUID
  budget + gate on the paper's identity, the coordinator with the
  never-ship-incomplete guarantee, and the three-arm ladder recorded.

  W4.1+W4.2 arc.h v2 + core : done — modes (BASE/Q/QG/IMMEDIATE),
                                the full paper parameter set with
                                presence-style defaults (the
                                unconditional-defaults draft silently
                                overwrote caller params — the golden
                                leg caught it), size helper + in-place
                                init, submit with S_n (A_n on the
                                receiver clock, no synchronization),
                                QG drain with park/timeout/wake; the
                                goldens drive the REAL clock the way
                                the sidecar runs: the linear cycle
                                lands on the paper's analytic fixed
                                point exactly (D alternates
                                {94.7934, 93.8016} ms), the exponent
                                example holds the paper band; replay
                                paced at the true 60 fps cadence:
                                QG releases all frames in submit order
                                (the guard reorders the valleys —
                                guard_waits is the mechanism working),
                                zero timeouts, p99 added 65 ms
                                (9932dbe, e2db1b7)
  W4.3 sidecar + SPSC       : done — entries over the W0.2 ring
                                (recovery never blocks), the pump
                                drains ring -> ARC -> apply; shadow
                                runs the IMMEDIATE pacer (the old
                                apply path with the estimator recording
                                what live WOULD impose — the promotion
                                evidence), live runs QG, killed is
                                the oracle; shadow/killed share the
                                pacer mode by design, differing
                                procedurally (what W4.4 gates on vs
                                its rollback target); stop flushes
                                (6e70ec4)
  W4.4 deploy controller     : done — start() arms in shadow, the
                                promotion gate (24 h + SLO clean), the
                                live SLO watch with kill
                                recommendation/auto-kill, the operator
                                switch (c8b83e7, 9201ad6)
  W4.5 e2e + runbook leg     : done — sidecar modes + ARC goldens +
                                the scripted runbook flow shadow ->
                                live -> killed; the kill-switch
                                runbook is executed, not just
                                documented (a215a46)

  WAVE 4 COMPLETE — milestone M4 reached: the P3 sidecar is whole —
  the ARC-QG core proven against the paper's analytic cycle on the
  real clock, the sidecar with shadow/live/killed, the deploy
  controller with the promotion gate and the always-armed kill
  switch, and the runbook executed as a test.

  W5.1 fuzzers                : done — the three WAM-shape fuzzers
                                (fountain streams, embodiment vectors,
                                ARC release streams) with smoke budgets
                                in ctest and full legs verified green;
                                the ARC leg found a REAL bug on its
                                first run: the QG park broke the drain
                                loop, stranding one absurd-stamp entry
                                per poll pass (545d5ec) — the same class
                                of find as wam's M.1 fuzz history
                                (a20bfed)
  W5.2 packaging + embedding  : done — version.h as the single source
                                of truth, one .pc per link granularity,
                                find_package(sdf) imported targets, the
                                DESTDIR-staged install leg in ci
                                (external hello-world via pkg-config +
                                a fresh CMake consumer); embedding
                                notes (thread contract,
                                no-alloc-after-warmup) in design 05;
                                live find: unquoted pc-in heredocs had
                                shipped empty libdir/exec_prefix
                                (d9d92a2, 175477a)
  W5.3 Rust seam               : done as designed — the sdf-fountain
                                skeleton crate, cbindgen over the
                                declared surface, the
                                declaration-surface diff as the gate;
                                cargo absent here: the leg skips
                                cleanly and is advisory (the hosted
                                matrix owns the authority — the wam
                                hosted-matrix posture); no functional
                                Rust (6eee0a2)

  WAVE 5 COMPLETE — milestone M5 reached: hardening (3 fuzzers with
  a live bug find), packaging (install + pkg-config + find_package
  legs in ci), and the Rust seam validated by construction. The
  plan is COMPLETE: all five waves, all six milestones, every
  prototype (P1 dispatch, P2 repair, P3 pacing) built, tested, and
  gated end to end — 57 prompt landings from the baseline import
  to the last seam.

  KNOWN GAPS (owned — recorded at the M5 truth-pass; Wave 6 closes
  all five. The first hosted run is now green; only retained nightly
  history remains external evidence):
  1. live scrape endpoint  — CLOSED by W6.1 (8942d34..b029aec): the
                             K.1b endpoint behind
                             SDF_METRICS_ENDPOINT serves the proxy
                             and the ARC sidecar live.
  2. arc_schedule.json     — CLOSED by W6.2 (ab5ec88, e7ed21f): the
                             12.3 cycles live in the vector file,
                             extracted through the public ABI; the
                             parity gate validates 556 comparisons,
                             zero pending.
  3. telemetry chain      — CLOSED by W6.1 (67ccd86..b029aec): the
                             proxy pushes per-origin stats into the
                             ADR-0002 ring, acquires the published
                             profile per tick, and the PROCESS-PAIR
                             harness (e2e_shm_chain.sh + the Python
                             driver) closed it live — 16 published
                             versions, v16 acquired, 28 slots,
                             7 heartbeat ticks, scrape green.
  4. hosted CI             — CLOSED by W6.5 (07a7a18, e5141e2,
                             cbc5bd9): the matrix (style/tidy,
                             gcc+clang, sanitizers, e2e, bench gate,
                             install, isolated wheel, Helm,
                             paired fountain bench, authoritative
                             rust seam, full fuzz) is hosted and
                             green: run 44774290565 at 0d6e60a,
                             10/10 jobs succeeded on 2026-11-20
                             (https://github.com/aoboy/open_storage/
                             actions/runs/44774290565). The nightly
                             history line is still open because
                             bench.yml has no retained baseline yet.
  5. Rust seam             — CLOSED by W6.5 (9599843): cargo 1.100
                             ran the declaration diff for the first
                             time and found a real divergence (8 of
                             15 functions, c_void handles, uintptr_t/
                             int32_t, cbindgen tag style). The crate
                             now declares 15 functions / 5 typedefs
                             / 2 structs / 1 enum; the checker is a
                             parsed surface with a selftest; CI runs
                             --require (missing cargo is a failure).

  W6.5 hosted CI matrix      : done — first hosted run is green:
                                workflow run 44774290565 at 0d6e60a,
                                10/10 jobs succeeded on 2026-11-20
                                (https://github.com/aoboy/open_storage/
                                actions/runs/44774290565). ci.yml now
                                has 11 jobs (the 0d6e60a run predates the
                                Helm and paired-fountain additions): style
                                (clang-format 23.1.5 + clang-tidy
                                23.1.4, both pinned; format check +
                                analyzer gate), build-and-test on
                                gcc AND clang, ASan+UBSan + TSan, the
                                four e2e harnesses, the seeded-spray
                                bench gate, the W5.2 install leg, the
                                W6.16 isolated wheel leg, the rust
                                seam with --require, and the
                                full fuzz budgets. bench.yml: the
                                design-00 §11.3 nightly gate (p99 >
                                5% vs the 7-day median; first two
                                nights skip, min 3 samples), with
                                offline selftests for the history
                                fetch + gate. The seam found a real
                                divergence on its first real run;
                                the skeleton now declares the full
                                ABI and the line-based checker was
                                replaced by a parsed surface (the
                                old one false-failed on wrapped
                                prototypes). Latent lint debt paid:
                                5 C analyzer findings, 6 ruff, 13
                                strict-mypy errors. Local evidence:
                                make ci green with ruff/mypy/cargo
                                on PATH — 23/23 x {default,
                                ASan+UBSan, TSan} + install + seam;
                                e2e x4; bench 5.0/5.8 cyc/dec (gate
                                30); fuzz 200k/200k/100k in 0.3 s;
                                actionlint 1.7.12 clean.

  W6.2 ARC goldens extracted  : done — gen_goldens_arc drives the
                                public ABI on the real clock and
                                pins the steady-state D trajectory
                                (100 samples x 2 entries: the
                                linear cycle measured exactly on the
                                paper's analytic pair {94.7934,
                                93.8016} ms; the exponent band inside
                                the paper's ~[97.9, 100.5]); the 60
                                fps replay stays in C by design
                                (timing-based — its note says so in
                                the file); arc.abi_v2 live in
                                test_parity: 556 comparisons, ZERO
                                pending. Live finds: python json.dump
                                writes em-dashes as \u2014, which the
                                C golden parser rejects BY DESIGN —
                                vector text is ASCII-only; the JSON
                                params are numbers (the atof-strings
                                draft failed the type check).

  W6.1 live scrape + chain  : done — the K.1b endpoint (unit test
                                pins the wam contract: exact
                                Content-Length, 405, 500, stop-closes),
                                both scrape surfaces wired (proxy
                                series incl. the heartbeat + acquired
                                version; the ARC sidecar's own dump),
                                the in-process SHM leg (pre-publish ->
                                acquire -> recompile, exact telemetry
                                accounting), and the process-pair
                                harness with the Python loop driving a
                                LIVE proxy through the segment.
                                Live finds: dispatch-side pacing is
                                the honest harness knob (the loopback
                                deadline scheduler batches due units —
                                origin throttling does not stretch a
                                run); attach-or-create with the API's
                                own rc (the post-close -errno draft
                                could return a clobbered value).

  W6.3 multi-block repair     : done — the W3.5 coordinator owned one
                                decoder, budget, gate and ESI cursor;
                                W6.3 moved the loop to the
                                coordinator while the fountain
                                contract stays ONE block per codec
                                object. cfg gains n_blocks (0/1 =
                                legacy single block, <= 64), stats
                                gain per-block sent/received/bytes/
                                rounds/fallback/rank/ok; symbols are
                                admitted by decode-rank deficit
                                (descending, ties by block index),
                                with per-block ARQ fallback. `ok` is
                                now a BYTE-EXACT gate: every block
                                is memcmp'd against the deterministic
                                sender block (per-block salt), not
                                just fd_decode's rank result. Live
                                find: the old LT integration fixture
                                fed LT's decoder a Nanorq-encoded
                                stream and passed on garbage — the
                                byte-exact gate exposed it; the
                                fixture now encodes the arm/eps/
                                blocks under test. Evidence: 4-block
                                nanorq, eps=0.02, loss=30%, wave=64
                                -> 5 coordinator rounds, block_rounds
                                5,5,5,5, block_rank 66,66,66,65,
                                block_ok 1,1,1,1; e2e ladder green;
                                23/23 ctest (ac9332c, 3104fd2).
  W6.4 CFFI control surface   : done — the cdef chain follows the
                                include graph (origin -> wm -> arc ->
                                fluid -> fountain); the extension
                                links the PIC static archives
                                (nanorq rebuilt -fPIC and installed)
                                and builds against an installed
                                prefix via --prefix; the loader
                                honours SDF_CFFI_DIR. The ARC
                                sidecar gained arc_set_mode() +
                                arc_sidecar_set_mode(): the desired
                                mode is atomic and lands on the next
                                pump, and entering killed drains the
                                QG backlog (IMMEDIATE bypass) in
                                that same pump; stop() now flushes
                                the ring before clearing running.
                                CffiSidecarTransport drives the real
                                C sidecar, and promotion baselines
                                budget_alerts_seen so shadow
                                evidence is not re-counted as fresh
                                live breaches. Evidence: C one-pump
                                flip + kill-drain test; live CFFI
                                deploy test (promote -> live parks
                                an entry -> kill -> drained);
                                e2e_apply runbook through CFFI; the
                                install leg builds the extension
                                from $DEST/usr and drives the
                                sidecar; make ci green (9d56e78,
                                c099e03, 301e306). KNOWN LIMITATION at
                                that landing, recorded not claimed:
                                shadow still mapped to IMMEDIATE at
                                the pacer, so true shadow added-
                                latency metrics — and therefore
                                non-vacuous promotion SLO evidence —
                                were open. Closed by W6.10; see the
                                W6.9/W6.10 entries below.
  W6.6 v1.0 beachhead release : done — include/sdf/version.h is 1.0.0;
                                the .pc/find_package versions follow
                                by configure, and the Rust seam crate
                                and Python control plane are aligned
                                at 1.0.0. The version bump exposed
                                the SameMajorVersion coupling in the
                                install consumer (0.2 -> 1.0).
                                packaging/debian/control is real;
                                scripts/build_deb.sh stages a /usr
                                install and dpkg-deb builds a -dev
                                payload; scripts/test_deb.sh performs
                                dpkg --root installation into an
                                isolated rootfs and compiles the
                                external hello-world through
                                PKG_CONFIG_SYSROOT_DIR. `make deb`
                                joined `make ci` and the hosted
                                install job; rpm/Helm explicitly
                                remain placeholders. Release notes
                                compiled from this ledger in
                                docs/releases/v1.0.0.md. Evidence:
                                make ci green (lint; 23/23 C tests x
                                default/ASan+UBSan/TSan; Python
                                19/19; install incl. installed CFFI;
                                deb; rust seam 15/5/2/1 identical),
                                all four e2e harnesses green, WM
                                bench 4.1/4.9/5.8 cyc/dec,
                                libsdf-dev_1.0.0_amd64.deb built and
                                installed (ef737b9). No hosted run
                                is fabricated; it remains pending a
                                remote/runner.
  W6.8 functional Rust LT     : done — lt.rs behaviorally ports the C
                                xorshift64* stream, robust-soliton draw,
                                deterministic neighbors and peeling over
                                the same caller-owned repr(C) layouts.
                                Only the internal lt_*/lt_prng_next arm
                                is replaced: C fe_*/fd_* dispatch and
                                nanorq remain, with no codec heap
                                allocation and panic catch/abort at the
                                FFI boundary. SDF_USE_RUST_FOUNTAIN=ON
                                selects Rust; OFF remains the default.
                                Cargo's staticlib is merged into the
                                existing libsdf_fountain.a, so callers,
                                public headers and installed archive
                                names do not change (77e0292, 568c30c,
                                180f025). Evidence: pinned Rust 1.100.0
                                fmt/clippy clean, cargo tests 4/4;
                                cbindgen declarations 15 functions /
                                5 typedefs / 2 structs / 1 enum
                                identical; C and Rust ctest 28/28 each;
                                complete parity reports byte-identical —
                                1,410 comparisons, ZERO pending, of which
                                930 are fountain (including the expanded
                                LT and nanorq geometries). The W6.8-era
                                556 figure is superseded, not current
                                harness evidence.
                                The merged archive has exactly 15 public
                                plus 13 internal symbols once each and
                                no C LT objects; Rust-source rebuild/
                                remerge/relink is green. Rust-backed
                                ASan+UBSan is 28/28 (C callers and the
                                allocator boundary instrumented, not a
                                claim of Rust-internal ASan). Installed
                                pkg-config, find_package, CFFI and dpkg
                                consumers execute BOTH LT and nanorq in
                                both variants. Full make ci is green:
                                C default/ASan+UBSan/TSan 28/28 each,
                                Python 121/121, plus the dual-variant
                                gates above. The comparison window stays
                                open: no C removal, default cutover or
                                performance claim. The hosted rust-seam
                                leg at 0d6e60a ran the current tree green.
  W6.9 ARC entry retention   : done — arc_submit() now reserves the slab
                                slot before the arrival path mutates the
                                estimator, sequence, or auto windows, so
                                a refused submit has no side effects. The
                                sidecar holds a ring-accepted entry that
                                the pacer refused (-EAGAIN) in a pump-
                                owned pending slot and retries it before
                                the next ring pop. The public SPSC ring
                                still returns -EAGAIN to the caller, who
                                keeps ownership; that is distinct from
                                already-accepted internal backpressure.
                                Stop and every mode transition drain the
                                pending slot, ring, and pacer. Transition
                                flushes re-key queued entries to release
                                in submit order (seq breaks T_n ties),
                                including the live-to-killed drain.
                                Evidence: test_arc_core's side-effect-free
                                backpressure + ordered-flush legs and
                                test_arc_sidecar's saturation retention
                                leg (kill drains old backlog, pending
                                entry retried, exactly four applies, no
                                duplicate) pass. Local gate: make build
                                test and make ci green — 23/23 C tests
                                in default, ASan+UBSan, and TSan; 25/25
                                Python; install/deb legs green. The
                                cargo-dependent Rust legs were skipped
                                in this environment; no Rust or hosted
                                claim is made (3e25194).
  W6.10 true ARC shadow      : done — shadow keeps the pacer in
                                ARC_MODE_QG, computes the same schedule
                                live would, applies each accepted entry
                                immediately and exactly once, and the QG
                                callback records the schedule release
                                instead of applying again. Separate
                                epoch-scoped shadow schedule/immediate
                                counters and added-latency percentiles
                                share no counter with live applies. The
                                additive public API adds
                                ARC_SHADOW_RECORD_CAP (256),
                                arc_shadow_record_t, arc_shadow_stats_t,
                                arc_sidecar_shadow_stats(), and
                                arc_sidecar_shadow_records(); existing
                                arc_cfg_t/arc_stats_t layouts and symbols
                                are unchanged. Records are per-entry
                                (block_n/seq/S_n/A_n/T_n/release_ns,
                                released + immediate_applied) and expose
                                the newest 256 accepted entries in submit
                                order with explicit total/dropped counts.
                                Leaving shadow discards only the already-
                                applied schedule; entering shadow flushes
                                live backlog first; stop leaves no late
                                callback. The deploy gate rejects absent,
                                empty, stale (> shadow_evidence_max_age_s,
                                default 300 s), inactive, inconsistent
                                (including record totals that do not match
                                immediate applies), or IMMEDIATE-only
                                evidence on top of the 24 h window and
                                alert budget, and records the reason.
                                The scrape endpoint exposes the
                                sdf_arc_shadow_* schedule and immediate
                                series. Evidence: test_abi pins the new
                                layouts (record 64 B, stats 120 B);
                                test_arc_sidecar covers evidence,
                                exactly-once transitions, the 256-record
                                window, and W6.9 saturation;
                                test_arc_core covers ordered transitions;
                                Python 16/16 test_arc_deploy, including
                                the live CFFI promote->live-park->kill
                                leg; e2e_apply now requires real shadow
                                samples before promotion. Local gate:
                                make build test and make ci green —
                                23/23 C tests in default, ASan+UBSan,
                                and TSan; 25/25 Python; install/deb
                                legs green; e2e_apply green. The cargo-
                                dependent Rust legs were skipped in
                                this environment. No hosted, performance,
                                or 24 h soak claim is made (3e25194).
  W6.11 expanded conformance : partially done — local conformance and
                                the authoritative hosted matrix have
                                landed; run 44774290565 at 0d6e60a is
                                green (10/10 jobs). Nightly history is
                                still absent. Landed:
                                WM rejection immutability and real
                                residual-fairness hit/conservation
                                assertions; a deterministic ARC replay
                                tool with the W5 park/stranding and
                                W6.9 saturation corpora (the park case
                                was proven to fail the pre-fix `break`
                                drain); input-derived fountain fuzz
                                geometry for both codecs with directed
                                boundary shapes; the 1,024-entry nanorq
                                ESI bitmap fix; fail-closed rejection of
                                unknown codec selectors, overflow-checked
                                repair counts, a nanorq readiness guard
                                for missing backend storage, and unique
                                applied-payload checks in the sidecar
                                saturation test; a persistent fountain
                                replay corpus that keeps exact ESIs,
                                checks readiness after every push, and
                                fails the historical 512-entry nanorq
                                bitmap with false readiness at rank 1;
                                a WM profile-update corpus with exact
                                return codes and post-update profiles;
                                five additional public-ABI-extracted
                                fountain golden geometries with every
                                original value unchanged; and an
                                installed-prefix CFFI consumer that
                                complements the C harness
                                and reports 1,376 applicable comparisons
                                (WM 280, ARC 200, real-codec fountain
                                896) plus 34 toy-only comparisons that
                                shipped artifacts do not contain.
                                Evidence: C parity validates 1,410
                                comparisons with zero pending; 28/28
                                CTest in default, ASan+UBSan, and TSan;
                                Python 121/121; 200,000 expanded fountain
                                streams; `scripts/test_install.sh`
                                green including the installed-CFFI
                                golden leg, and the isolated wheel now
                                runs the same 1,376-comparison consumer
                                through its bundled extension;
                                clang-format/tidy clean.
                                `make ci` is green: lint + ruff + mypy,
                                28/28 C in default/ASan+UBSan/TSan,
                                121/121 Python, install, Deb, and wheel
                                legs. Hosted run 44774290565 is green,
                                including rust-seam and the isolated
                                wheel artifact. Nightly history,
                                TLS/certificate legs, paired performance
                                measurements, and other unexecuted
                                external legs are not claimed
                                (b05b0e0, 40e496f, bf9e88c, 5b0a87a,
                                51b0c78, 502fadc, ce17588).
  W6.12 paired fountain bench: partially done — bench_fountain records
                                init/generate/push/decode, p99, and
                                throughput for the same binary built
                                with C and Rust LT. bench-paired builds
                                both variants, runs identical K/symbol/
                                loss configurations, writes
                                out/bench/fountain_paired.txt, and the
                                comparator rejects missing, duplicate,
                                or mixed-runner pairs. Local 8-record
                                run covered K=16/32/64 at 0/10/20/30%
                                loss; these are single-runner
                                observations, not a baseline. CI has a
                                --require fountain-bench job, and
                                bench.yml now records a nightly
                                fountain-paired artifact. Still open:
                                a stable named reference runner and
                                seven retained nightly samples.
  W6.13 LT window decision   : decided — C remains the default LT arm; Rust LT
                                stays opt-in and no C is removed. ADR-0010
                                records the incomplete evidence honestly:
                                the current 28-test, 1,410-comparison tree
                                and hosted matrix are green, but no paired
                                same-runner performance measurements or
                                seven nightly samples exist. Revisit criteria require
                                both variants passing the current suites and
                                parity, a green hosted matrix, seven paired
                                nightly samples within the >5% noise rule,
                                both-variant installed consumers, and a
                                separate reviewed landing with an exercised
                                rollback. No code, ABI, golden vector, or
                                default switched; no performance claim is
                                made. W6.13 does not close the comparison
                                window and does not authorize deletion.
  W6.14 executable observability: implemented — design 05 now carries the
                                canonical `sdf_` naming/schema contract,
                                exact 32-bucket histogram bounds, bounded
                                label domains, compatibility aliases, and
                                the external-engine input contract. The C
                                metric primitive renders cumulative
                                `_bucket`/`_sum`/`_count` families with
                                `le="+Inf"`. WM exports per-path/arm
                                dispatch, fill bytes, kill state, and
                                exact dispatch-to-completion latency
                                through a bounded lock-free slot table;
                                FLUID exports repair latency and admit
                                round size; ARC exports release batch,
                                live/shadow added latency, accepted
                                entries, caller-refused backpressure,
                                invalid/engine drops, and kill state.
                                W6.9 saturation keeps the drop counter at
                                zero while caller refusal is distinct.
                                Python SdfMetrics owns a private registry,
                                records WM/ARC control samples, and exports
                                deterministic 2/10-missed-interval
                                liveness. The alert specification generates
                                docs/ops/alerts/sdf-alerts.yml and tests
                                every warning/page boundary. The three
                                importable Grafana JSON dashboards resolve
                                against C-generated scrape fixtures and the
                                declared external engine fixture. Evidence:
                                make test green — 23/23 C and 58/58 Python;
                                the new dashboard/query leg is 11/11. Local
                                make ci is green: 23/23 C tests in default,
                                ASan+UBSan, and TSan; 58/58 Python; ruff and
                                mypy clean; install and Debian legs green.
                                The cargo-dependent Rust legs skipped in
                                this environment. Local query validation is
                                not a hosted Grafana import; no hosted,
                                performance, or 24 h evidence is claimed.

Percentages are engineering judgement weighted by scope and test
evidence, not a contract; each entry names what is missing. This ledger
is the single source of completion truth — a landed import without its
golden-parity gate (ADR-0009) counts as 0%.

---

## Table of contents

- [0. Scope, goals, non-goals](#0-scope-goals-non-goals)
- [1. Execution model](#1-execution-model)
- [2. Prototype ↔ wave ↔ design-doc map](#2-prototype--wave--design-doc-map)
- [3. Phase W0 — Foundations](#3-phase-w0--foundations)
- [4. Phase W1 — Origin driver seam](#4-phase-w1--origin-driver-seam)
- [5. Phase W2 — Whack-a-Mole dispatch (Prototype 1)](#5-phase-w2--whack-a-mole-dispatch-prototype-1)
- [6. Phase W3 — FLUID repair (Prototype 2)](#6-phase-w3--fluid-repair-prototype-2)
- [7. Phase W4 — ARC pacing sidecar (Prototype 3)](#7-phase-w4--arc-pacing-sidecar-prototype-3)
- [8. Phase W5 — Hardening, packaging, Rust seams](#8-phase-w5--hardening-packaging-rust-seams)
- [9. Testing map](#9-testing-map)
- [10. Bench gates](#10-bench-gates)
- [11. Risk register](#11-risk-register)
- [12. Sizes & milestones](#12-sizes--milestones)
- [13. Work-log protocol](#13-work-log-protocol)
- [14. Phase W6 — Production hardening & the migration begins](#14-phase-w6--production-hardening--the-migration-begins)

---

## 0. Scope, goals, non-goals

### Goals

- Deliver the three prototypes of [00-overview §6](00-overview.md#6-prototype-sequence-roirisk-order)
  as independently deployable runtimes over one shared C core
  (`libsdf_{common,wm,fountain,fluid,arc}`):
  1. **P1 cache-fill proxy** — deterministic, seeded origin dispatch with
     graded shedding;
  2. **P2 repair coordinator** — fountain-coded, slack-gated WAN repair
     with ARQ fallback;
  3. **P3 apply sidecar** — ARC-QG receiver pacing with shadow/live/kill
     modes.
- **Import-first execution** (07 §1): every data-plane module starts from
  the proven wack_a_mole implementation, adapted behind `include/sdf`,
  and is accepted by golden-vector parity (ADR-0009).
- ABI discipline: `include/sdf/*.h` + `golden_vectors/` are the contract
  (ADR-0001); every delta is an ADR (0005–0009 register in 07 §5).
- Every prototype is independently A/B-able with the rollout gates
  already written in design docs 01–03.

### Non-goals (v1)

- No new storage engine, no replica protocol changes, no on-wire format of
  our own (repair feedback is count-based at the coordinator).
- No S3/peer drivers beyond the seam — `[[origin]]` classes loopback and
  HTTP ship; s3/peer are deferred design notes (09 §9).
- No Rust functionality — W5.3 validates the seam only (ADR-0003 order).
- No packet transports imported (07 §4).

### Key artifacts to implement verbatim (from the papers, via the import)

- [WAM]: bit-reversal spray θ, shuffle1/2 with seed pairs, discrepancy
  bounds ≤ ℓ / ≤ 2ℓ over any contiguous window, embodiments 1–4 with
  global residual, Σb = m invariant.
- [FLUID]: block budget N = ceil(K/(1−ε)); loss-product π = L_ℓ/N
  identity; efficiency K/N ≥ (1−ε)·K/(K+1); ARQ fallback safety.
- [ARC]: (S_n, A_n) → D/E → T_n schedule; ARC-QG ordered release;
  receiver clock only; IMMEDIATE as test oracle, never production.

---

## 1. Execution model

Work lands in **waves**; each prompt is a self-contained package with an
objective, the wack_a_mole source it imports (if any), deliverables,
contract, and an **acceptance** block that a reviewer or agent can verify
mechanically. The protocol (expanded in §13 and `CONTRIBUTING.md`):

1. `plan:` commit lands or amends the prompt before implementation.
2. `src:` commit references the prompt ID (e.g. `W2.3`) in its message.
3. Acceptance must be demonstrable from the repo: tests, goldens, gates.
4. `docs: <ID> outcome recorded, next = <ID>` sync commit follows each
   landing; the ledger above is refreshed per wave and pinned to a hash.

Velocity assumption (inherited from the wack_a_mole sizing document):
agent-assisted execution on well-specified prompts runs at 0.35–0.55×
solo-human time; design-heavy items 0.6–0.8×. Halve confidence if a
prompt goes stale mid-flight.

---

## 2. Prototype ↔ wave ↔ design-doc map

| Prototype | Wave | Design doc | Success metrics |
|---|---|---|---|
| 1 — profile dispatch + soft shedding | W1 + W2 | [01](01-whack-a-mole.md) | fill p99/variance down, bytes neutral |
| 2 — slack fountain repair | W3 | [02](02-fluid-repair.md) | repair p99 vs overhead Pareto; ARQ fallback < 2% |
| 3 — ARC pacing sidecar | W4 | [03](03-arc-pacing.md) | engine apply p99, backpressure events, added ≤ 10 ms p99 |
| cross-cutting | W0, W5 | [05](05-observability.md), [06](06-nanorq-evaluation.md) | CI gates, goldens, packaging |

Dependency spine: W0.2 → W2.3/W2.4 (SHM/seqlock) · W1 → W2.5/W3.5
(origin I/O) · W3.1–W3.4 → W3.5 · W4.1–W4.2 → W4.3. Waves W2, W3, W4
share W0 and W1 but are otherwise parallelizable across engineers.

---

## 3. Phase W0 — Foundations

> shared primitives, config, parity harness, gates. Everything below
> depends on this wave. Dependency: none.

### PROMPT W0.1 — Repo, gates, and work-log discipline ✅

**Objective:** version control and the execution protocol exist before
any code lands.
**Deliverables:** git repository at the `open_storage` root (papers +
code together, wack_a_mole layout); `.gitignore`; `CONTRIBUTING.md`
(commit prefixes, doc-sync protocol, evidence rules); this plan.
**Acceptance:** `git log` shows the baseline import and the doc series;
every subsequent landing follows §13.
**Size:** 0.5–1 day. **Status: done** — delivered with this plan's series.

### PROMPT W0.2 — Common primitives import

**Objective:** `src/c/common/*` implement the `include/sdf/common/*.h`
APIs with the proven wack_a_mole algorithms.
**Import:** `include/wam/spsc.h` (SPSC ring, TSan-clean) → `ring.c`;
`include/wam/profpub.h` (seqlock publish) → `seqlock.c`; pool/arena
patterns from WAM J.2 → `pool.c`; clock helpers → `timer.c`; ADR-0002
segment map → `shm.c`; log → `log.c`. Provenance blocks per ADR-0009.
**Contract:** exactly the existing `include/sdf/common/*.h` signatures —
no header changes in this prompt.
**Acceptance:** unit tests: ring wrap order + 2²² two-thread soak
(adapt `test_spsc`); seqlock no-torn-read under contention (adapt
`test_profpub`); pool no-malloc-after-warmup assert; SHM layout offset
asserts per ADR-0002. `scripts/run_tests.sh` green from clean build.
**Size:** 2–3 days.

### PROMPT W0.3 — Strict TOML loader + example configs

**Objective:** ops config loads with hard schema errors (ADR-0005).
**Import:** `src/config/config.c` (wack_a_mole K.2 loader, ~1.4 kloc) →
`src/c/common/config.c`, adapted to the sdf schema:
`[wm]`, `[fluid]`, `[arc]`, `[[origin]]`, `[ab]`, `[metrics]`.
**Deliverables:** loader behind `include/sdf/common/config.h`;
`configs/{dev,staging,prod}/{wm,fluid,arc}.toml` replacing the `.yaml`
placeholders; `tests/c/unit/test_config.c`.
**Contract:** precedence CLI > file > defaults; presence-bit merge (a
partial file cannot clobber built-in defaults); unknown key/type/range =
hard `path:line:` error.
**Acceptance:** `test_config` loads every shipped example, pins
defaults, and asserts rejection cases (adapt WAM `test_config`).
**Size:** 2–3 days.

### PROMPT W0.4 — Golden-parity harness

**Objective:** the ADR-0009 gate exists before the first import.
**Import:** golden material from the wack_a_mole test suite: θ(249,10)=636
and seed-pair sequences (`test_spray`), embodiment vectors
(`test_profile`), ARC §12.3 schedules (`test_arc`), FLUID O.1 identities
(`test_fluid`) — as cited in 07 §3's parity column.
**Deliverables:** `tests/c/test_parity.c` + a minimal JSON-subset reader
(the `golden_vectors/*.json` format stays); `golden_vectors/wm_select.json`
filled (θ golden, seed sequences, discrepancy vectors);
`arc_schedule.json`/`fountain_esi.json` gain their vector schemas now
(values land with W4.2/W3.1).
**Acceptance:** `test_parity` runs in ctest and validates every present
vector; CI (W0.5) includes it.
**Size:** 1–2 days.

### PROMPT W0.5 — Metrics conventions + Makefile gates

**Objective:** observability conventions and local gates mirror the
wack_a_mole `makefile.wam` set.
**Import:** counter/reservoir-histogram conventions from
`include/wam/metrics.h` → `src/c/common/metrics.c` (sdf series names per
00 §11.1 stay authoritative); gate structure from `makefile.wam`.
**Deliverables:** `metrics.c` behind the existing header; Makefile
targets `ci` (format+tidy+gcc+clang where available), `asan`, `tsan`,
`bench` (recorded), `docs`; `scripts/run_bench.sh` wired to record into
`build/bench_results.txt` (never ctest-gated).
**Acceptance:** `make ci` green from clean checkout; `make asan`/`tsan`
build and run the suite; bench target records spray/arc micro-benches
(thresholds activate in W2/W4).
**Size:** 1–2 days.

**W0 subtotal: 6–10 engineer-days.**

---

## 4. Phase W1 — Origin driver seam

> origins are the paths (ADR-0008, design 09). Depends on W0 only for
> gates; W1.1 has no code dependencies.

### PROMPT W1.1 — `include/sdf/origin.h` ABI

**Objective:** the vtable contract of design 09 §2–3 becomes a header.
**Deliverables:** `include/sdf/origin.h` (`sdf_origin_ops_t`,
`sdf_fetch_req_t`, `sdf_origin_stats_t`, `sdf_fetch_done_fn`,
`sdf_origin_cfg_t` with class enum); `tests/c/test_abi.c` extended with
layout asserts; ADR-0008 referenced from the header comment.
**Acceptance:** ABI test green; review confirms the link rule (no driver
types above the seam) is expressible.
**Size:** 1–2 days.

### PROMPT W1.2 — Loopback origin driver (the harness)

**Objective:** byte-range serving from local files with live degradation
knobs — the netem analogue.
**Deliverables:** `src/c/origin/loopback/loopback_origin.c` implementing
`sdf_origin_ops_t`; per-origin `throttle_ns`, `loss_pct`, `err_pct`,
`lat_jitter_ns` knobs, live-reloadable mid-run; completion callback on an
internal driver thread; `tests/c/unit/test_origin_loopback.c`.
**Acceptance:** stats exactness under each knob; knob reload mid-stream
takes effect without restart; drop/fail units produce correct
`reqs_failed`/`ewma_err`; no allocation per fetch after warmup.
**Size:** 2–3 days.

### PROMPT W1.3 — HTTP origin driver

**Objective:** a real-network reference driver behind the same ops.
**Deliverables:** `src/c/origin/http/http_origin.c` — POSIX-socket
HTTP/1.1 `Range` GET, connection reuse, `SDF_HAVE_CURL` optional path
(default OFF); origin config `url_template`, `timeout_ms`, `max_inflight`.
**Acceptance:** integration test against a local Python `http.server`
serving ranged files (loopback sockets, no external dependency);
timeouts surface as failed units, not hangs.
**Size:** 3–4 days.

### PROMPT W1.4 — Origin bench + degradation harness

**Objective:** repeatable multi-origin runs for every later e2e.
**Deliverables:** `tools/loadgen/origin_loadgen.c` (drives N origins
through a compiled profile, prints per-origin tallies + timing);
degradation profile files for the e2e scripts (steady, one-origin-slow,
one-origin-dies-mid-run, flapping).
**Acceptance:** loadgen runs each profile; tallies match
`wm_select_batch` counts exactly for a static profile.
**Size:** 1–2 days.

**W1 subtotal: 7–11 engineer-days.**

---

## 5. Phase W2 — Whack-a-Mole dispatch (Prototype 1)

> design [01](01-whack-a-mole.md); upgrades the implemented v1 core per
> ADR-0007 and completes the proxy around it.
> Dependencies: W0.2 (seqlock/SHM), W0.3 (config), W1 (origins).

### PROMPT W2.1 — Spray upgrade: methods, seeds, discrepancy

**Objective:** selection carries the paper's shuffle methods and seed
pairs; the bounded-discrepancy property becomes a test.
**Import:** `src/core/spray.c` — `wam_theta` (256-byte bit-reversal
table), `wam_spray_key` (plain/shuffle1/shuffle2), wrap rotation,
`wam_measure_discrepancy` harness.
**Deliverables:** `wm_select.c` upgraded: `wm_compile_cfg_t {method, sa,
sb}` (sb validated odd), seed rotation on wrap, batch form kept;
`tests/c/unit/test_wm_select.c` extended; `golden_vectors/wm_select.json`
seed-sequence + discrepancy vectors consumed by `test_parity`.
**Contract:** ADR-0007 exactly; `wm_compiled_t` layout change pinned in
`test_abi.c`; `SDF_VERSION` minor bump.
**Acceptance:** θ golden; deviation ≤ ℓ (shuffle1) and ≤ 2ℓ (shuffle2)
over contiguous-window property sweeps; two jobs with equal profiles and
different seeds never emit identical sequences over the first m units.
**Size:** 2–3 days.

### PROMPT W2.2 — Profile updates + severity controller

**Objective:** graded shedding: the controller removes `e(i)` balls with
deterministic redistribution; the C fast path whacks on hard signals
without Python.
**Import:** `src/core/profile.c` — `wam_profile_update` (embodiments
1–4, global residual r, renormalize with uint64 intermediates);
`src/ctrl/controller.c` — severity windows, θ_hi/θ_lo pulls, t_min rate
limit.
**Deliverables:** `wm_profile_update()` in `wm_profile.c`; severity
controller in `wm_shed.c` consuming `sdf_origin_stats_t` deltas
(ADR-0008 link rule: no driver headers); `tests/c/unit/test_wm_profile.c`
extended.
**Acceptance:** embodiment goldens (adapt `test_profile`); Σb = m and
residual fairness invariants; last-healthy-path never removed; C-side
whack latency bounded by t_min (unit test with synthetic stats).
**Size:** 3–4 days.

### PROMPT W2.3 — SHM profile publication

**Objective:** the Python loop publishes profiles; C readers never see a
torn profile (ADR-0002).
**Import:** `include/wam/profpub.h` publication protocol → the ADR-0002
segment map (double buffer + seqlock + `active_buf` flip + version).
**Deliverables:** `wm_shm.c` implemented: create/attach, publish
(inactive buffer → flip), acquire snapshot (reader), teardown;
`tests/python/test_shm.py` extended against a live segment.
**Acceptance:** no-torn-read contention test (reader loop vs writer
hammer, adapted `test_profpub`); version increments monotonically;
teardown leaves no segment (loop-restart test).
**Size:** 2–3 days.

### PROMPT W2.4 — Python control loop over live SHM

**Objective:** the 4 Hz loop closes against real telemetry.
**Import:** design 01 §Control-plane loop (reference `wm_control.py`
stays the scoring authority); presence-bit merge pattern from WAM K.2.
**Deliverables:** `python/sdf/control/wm_control.py` + `sdf/common/shm.py`
wired: read telemetry ring → score (missing telemetry = "no opinion") →
multiplicative decrease / additive increase → renormalize (largest
remainder stays) → write inactive buffer → flip; liveness heartbeat
metric; resolve 01 §Open-questions #3 (controller observes `down_mask`
and does not undo a C hard-down for one interval).
**Acceptance:** `tests/python/test_wm_control.py` end-to-end against a
stubbed SHM segment with scripted telemetry; unit tests for the
no-opinion scoring edge case; heartbeat visible to the metrics endpoint.
**Size:** 2–3 days.

### PROMPT W2.5 — Cache-fill proxy main

**Objective:** P1 exists as a runnable proxy over the origin seam.
**Deliverables:** `wm_proxy_main.c`: job admission (object + byte ranges
→ units), `wm_select` assignment with per-job counters, fetch issue +
completion ring accounting, A/B bucketing `bucket = xxhash64(key) % 100`
with arm assignment from config, metrics per 01 §Metrics, TOML config
per ADR-0005, live scrape endpoint (adapt WAM `metrics_endpoint.c`,
`SDF_METRICS_ENDPOINT`); `tests/c/integration/test_wm_proxy.c`.
**Acceptance:** integration test over loopback origins: per-origin
dispatch counts within the discrepancy bound of the compiled profile;
mid-run degradation redistributes within one control interval; kill
switch (config `wm.rollout_pct = 0`) falls back to round-robin arm.
**Size:** 3–5 days.

### PROMPT W2.6 — P1 e2e + rollout gates

**Objective:** the design-01 rollout table is measurable.
**Deliverables:** `tests/system/e2e_cache_fill.sh` over W1.4 degradation
profiles (steady / slow-origin / dies-mid-run / flapping); bench target
`bench_wm` records select cost (cycles/decision) and fill completion
p99/variance per arm; gates wired into `scripts/run_bench.sh`.
**Acceptance:** e2e green under every degradation profile with zero
failed fills (retries are controller-visible); dispatch-counts assertion
inside discrepancy bound; bytes-neutral check (treatment vs control);
spray decision cost ≤ 15 cyc recorded (gate activates: fail CI if
exceeded by 2× on the reference runner).
**Size:** 3–4 days.

**W2 subtotal: 15–22 engineer-days. Milestone M2 = P1 at the 5% gate of
design 01's rollout table.**

---

## 6. Phase W3 — FLUID repair (Prototype 2)

> design [02](02-fluid-repair.md); evaluation framing
> [06](06-nanorq-evaluation.md) (three arms, codec A/B).
> Dependencies: W0.4 (parity harness), W1 (origins), W2.1 (select for
> repair traffic can share live health).

### PROMPT W3.1 — Fountain ABI v2

**Objective:** close the ADR-0001 open item before any codec ships.
**Deliverables:** `include/sdf/fountain.h` gains `fe_sizeof(cfg)` /
`fd_sizeof(cfg)` + init-in-place forms; ESI/tag fields made explicit in
`fe_generate`/`fd_push`; `SDF_VERSION` minor bump;
`golden_vectors/fountain_esi.json` schema + first roundtrip vectors.
**Acceptance:** ABI test green; a toy codec implementing the ABI passes
`test_parity` on the schema vectors.
**Size:** 1–2 days.

### PROMPT W3.2 — nanorq vendoring + RaptorQ arm

**Objective:** the RaptorQ arm runs pinned, vendored, behind the ABI.
**Import:** nanorq pinned to the commit family wack_a_mole validated
(import map §7 — correct the `ursoid-tech` references in
`third_party/README.md` and 00 §2.4 in the same commit); encode/decode
block handling adapted from WAM `src/codec/codec.c` (block-level only —
no session layer, no S_n at this ABI).
**Deliverables:** `third_party/nanorq` vendored + `CMakeLists` builds
`libnanorq.a`; `fe_codec_nanorq.c`; fuzz budget hook in
`tests/c/fuzz/fuzz_fountain.c`.
**Acceptance:** roundtrip golden vectors; 20%-loss repair completes
(adapt `test_codec`); decode-abort budget respected (bounded work per
symbol push).
**Size:** 2–3 days.

### PROMPT W3.3 — LT reference arm (fresh code — no WAM source)

**Objective:** the A/B codec baseline (exception to import-first per
07 §1; cite the import map in the commit).
**Deliverables:** `fe_prng.c` (xorshift64*), Robust Soliton degree
distribution, `fe_codec_lt.c` (systematic LT: K source + on-demand
repair symbols), dense-GF(2)-free XOR decode.
**Acceptance:** roundtrip + loss-repair tests; overhead per block
recorded for the Pareto harness; `fuzz_fountain` budget runs 60 s clean.
**Size:** 3–5 days.

### PROMPT W3.4 — FLUID both halves

**Objective:** sender budget and coordinator gate implement the paper
together, with the WAM O.1/O.5 evidence attached.
**Import:** `include/wam/fluid.h` — `wam_fluid_budget_N`
(near-integer snap), `_efficiency_ok`, `_cost_ok`, `_first_round`,
admit/report state machine → `fluid_round.c` (sender budget per block)
and `fluid_gate.c` (coordinator π = L_ℓ/N gate, existing `fg_*` API).
**Deliverables:** `include/sdf/fluid.h` v2 (budget half added per 07
§5); `tests/c/unit/test_fluid_gate.c` with the O.1 sweep
(K = 1..64Ki × ε ∈ {0.01..0.2}) and randomized never-past-allowed
property; loss-product identity property.
**Acceptance:** all O.1 bounds hold verbatim; efficiency and cost
identities pass; ε = 0 disables cleanly (zero overhead path, WAM
pattern: NULL-check only).
**Size:** 2–3 days.

### PROMPT W3.5 — Repair coordinator loop

**Objective:** the design-02 repair loop runs over origins with ARQ
fallback.
**Deliverables:** `fluid_repair_main.c`: per-class job admission
(`hot-metadata` ε = 0.05, `cold-checkpoint` ε = 0.15 per 02 §Classes),
wave send via `wm_select` over origin stats, `fg_observe` on
`stats()` deltas, decode-sufficiency exit, **ARQ fallback when the gate
fires before `fd_ready`** (never ship an incomplete block), decode-abort
budget, metrics per 02 §Metrics.
**Acceptance:** integration test: completes under 20% injected loss with
ARQ fallback < 2% at ε = 0.10; gate-fires-early path returns a complete
block via fallback; class routing from TOML config.
**Size:** 3–5 days.

### PROMPT W3.6 — Three-arm A/B + loss ladder e2e

**Objective:** the design-02 Pareto experiment is reproducible.
**Deliverables:** `tests/system/e2e_repair.sh`: arms Control (ARQ) /
A (FLUID+LT) / B (FLUID+Nanorq), ε ∈ {0.01, 0.05, 0.10, 0.20}, loss
ladder {0..50%} via loopback knobs (adapt WAM `test_fluid_ladder_e2e`
shape); results to `build/bench_results.txt` + `python/sdf/bench`
plots (p99 vs overhead per arm).
**Acceptance:** ladder completes byte-exact at every step with fallback
bounded; the harness prints the Pareto table; soak leg with dropped
feedback (adapt `test_fluid_soak_e2e`: every 3rd report lost — budget
lags, never stalls).
**Size:** 4–6 days.

**W3 subtotal: 15–24 engineer-days. Milestone M3 = P2 at the 10% gate of
design 02's rollout table.**

---

## 7. Phase W4 — ARC pacing sidecar (Prototype 3)

> design [03](03-arc-pacing.md); ABI per ADR-0006.
> Dependencies: W0.2 (ring for handoff), W0.4 (parity harness).
> Highest interaction risk — shadow-first, kill switch always armed.

### PROMPT W4.1 — `arc.h` v2 (ADR-0006)

**Objective:** the ABI carries the real algorithm before the core lands.
**Deliverables:** `include/sdf/arc.h` v2: modes (`ARC_MODE_BASE/Q/QG`,
`ARC_MODE_IMMEDIATE` oracle), full parameter set (ρ_u/ρ_l, λ_u/λ_l, U, δ,
γ, J, G, T_idle, auto-param gains, `min_smooth_bytes`, `budget_ms`),
`arc_sizeof`, `arc_poll(p, now)` release loop, extended stats (D, E,
added-latency histogram, interval/jitter, guards, δ-clamps, budget
alerts, S_n rejects); `tests/c/test_abi.c` layout + version bump;
config schema ranges in the strict loader (ADR-0005).
**Acceptance:** ABI test green; loader rejects out-of-range params
(γ ∈ [8,20] ms etc. per the paper's static ranges).
**Size:** 1–2 days.

### PROMPT W4.2 — ARC core import

**Objective:** the validated wack_a_mole scheduler runs behind v2.
**Import:** `src/arc/arc.c` — asymmetric D update, ARC-Q E hysteresis,
ARC-QG guard ordering, idle re-anchor, δ-clamp, auto-params from RTT and
Δ_n quantiles, IMMEDIATE oracle, heap slab (adapt `test_arc` goldens).
**Deliverables:** `arc_offset.c` + `arc_heap.c` + `arc_pacer.c`;
`golden_vectors/arc_schedule.json` filled with paper §12.3 vectors;
`tests/c/unit/test_arc_offset.c` + `test_arc_heap.c`.
**Acceptance:** §12.3 goldens via `test_parity`; quantization invariants
(E ≥ D, 0 ≤ E−D < γ); guard bound; idle re-anchor; ARC per-block cost
≤ 200 ns recorded (`bench_arc`, gate activates at 2× on the reference
runner).
**Size:** 3–5 days.

### PROMPT W4.3 — Sidecar main + SPSC handoff

**Objective:** the sidecar sits between a recovery buffer and an engine
apply callback (design 03 §Objective).
**Deliverables:** `arc_sidecar_main.c`: entries in over an SPSC ring,
`arc_poll` release to the apply callback, small-entry bypass
(`min_smooth_bytes`), batch drain by `batch_max`/`batch_deadline_ns`,
modes wired: **shadow** (compute + metrics, apply immediately via the
old path), **live** (`ARC_MODE_QG`), **killed** (`ARC_MODE_IMMEDIATE`);
metrics per 03 §Metrics. W6.10 later replaced shadow's IMMEDIATE mapping
with the non-applying QG schedule described in the ledger; this prompt
records the original W4.3 delivery.
**Acceptance:** integration test: entry ordering preserved under
bursty injection (heap key `(release_time, producer_ts, seq)`, ties by
seq); killed-mode added latency = 0 by construction; shadow metrics
match live metrics' schedule exactly (same inputs, different apply path).
**Size:** 2–4 days.

### PROMPT W4.4 — Python deploy orchestration

**Objective:** shadow/live/killed is an operator action, not a rebuild.
**Import:** WAM K.3 live-tune pattern (control-plane updates live
sessions).
**Deliverables:** `python/sdf/control/arc_deploy.py` (stub today):
pipeline selection, mode commands over the control channel, shadow
24 h clock, added-latency SLO watch with auto-kill recommendation;
`tests/python/test_arc_deploy.py`.
**Acceptance:** deploy tests drive a running sidecar (mode flips land
within one poll interval); SLO breach recommends kill and the runbook
step is scriptable.
**Size:** 1–2 days.

### PROMPT W4.5 — Trace replay + kill-switch verification

**Objective:** the strongest ARC evidence transfers to this repo, and the
runbook is a test.
**Import:** WAM L.6 harness — 60 fps peak/valley trace, N = 600, through
IMMEDIATE/BASE/Q/QG (adapt `test_arc_experiments`).
**Deliverables:** `tests/c/integration/test_arc_sidecar.c` replay leg +
`tests/system/e2e_apply.sh` with a bursty recovery generator; the
`docs/ops/runbooks/arc-killswitch.md` steps executed as a scripted leg.
**Acceptance:** replay leg pins: QG 0 order violations, median interval
error 0, p99 added delay ≤ the 38 ms-class result under the same trace;
kill-switch leg: flip to killed under load, zero drops, ordering intact;
shadow→live promotion asserts the 03 rollout table's measurements are
produced (added latency ≤ 10 ms p99 gate on the harness generator).
**Size:** 2–3 days.

**W4 subtotal: 9–16 engineer-days. Milestone M4 = P3 at the shadow gate
of design 03's rollout table.**

---

## 8. Phase W5 — Hardening, packaging, Rust seams

> cross-cutting; can start opportunistically once W2 lands.

### PROMPT W5.1 — Fuzzers

**Import:** WAM M.1/X4 fuzzer shapes: ESI/tag streams over
`fd_push`; feedback + embodiment vectors over `wm_profile_update`;
`(S_n, A_n)` release streams over the ARC heap.
**Deliverables:** `tests/c/fuzz/{fuzz_fountain,fuzz_wm_profile}` filled +
`fuzz_arc_stream.c`; 60 s smoke in the `ci` gate; invariants: decoder
budget bounded, Σb = m, ordering never violated by guard logic.
**Acceptance:** clean 60 s runs at the CI seed budget; any invariant
breach fails the gate.
**Size:** 2–4 days.

### PROMPT W5.2 — Packaging + embedding

**Import:** WAM R2/M.2 pattern — install rules, pkg-config, version
macro as single source of truth, core/engine split.
**Deliverables:** CMake install + `sdf.pc` + `find_package(sdf)`
config; version macro `include/sdf/version.h`; cffi bindings verified
against **installed** artifacts (not the build tree); embedding notes
(thread contract, no-malloc-after-warmup) in `docs/design/05`.
**Acceptance:** external hello-world links via pkg-config and
`find_package`; `test_install` leg in CI.
**Size:** 3–4 days.

### PROMPT W5.3 — Rust seam validation

**Import:** none (seam only, per ADR-0003 order: fountain first).
**Deliverables:** `bindings/cbindgen.toml` exercised: a Rust
`sdf-fountain` skeleton crate regenerates a header diffed against
`include/sdf/fountain.h` in CI; golden vectors pass through the Rust
fountain stub where implemented; no functional Rust modules yet.
**Acceptance:** header diff empty for unchanged ABIs; any intentional
delta fails CI until an ADR bumps the vectors (ADR-0001 process).
**Size:** 2–3 days.

**W5 subtotal: 7–11 engineer-days.**

---

## 9. Testing map

| Test | Wave | Covers |
|---|---|---|
| `test_abi` | W0/W1/W2/W3/W4 | struct offsets, version bumps (extended per prompt) |
| `test_ring` | W0.2 | wrap order, 2²² two-thread soak |
| `test_seqlock` | W0.2 | no torn read under contention |
| `test_pool` | W0.2 | no-malloc-after-warmup |
| `test_shm` | W0.2/W2.3 | ADR-0002 offsets, publish/flip, teardown |
| `test_config` | W0.3 | every example TOML, defaults, strict rejections |
| `test_parity` | W0.4+ | every golden vector; the ADR-0009 gate |
| `test_metrics` | W0.5 | series names pinned |
| `test_origin_loopback` | W1.2 | stats exactness, knob live-reload |
| `test_origin_http` | W1.3 | ranged GET vs local http.server, timeout = failed unit |
| `test_wm_select` | W2.1 | θ golden, seeds, wrap, discrepancy properties |
| `test_wm_profile` | W2.2 | embodiments 1–4, Σb=m, residual fairness, last-path |
| `test_wm_proxy` | W2.5 | dispatch counts vs profile, degradation redistribution |
| `test_fluid_gate` | W3.4 | O.1 budget/efficiency/cost sweeps, π identity |
| `test_fe_codec` | W3.2/W3.3 | both arms: roundtrip, loss repair, abort budget |
| `test_fluid_repair` | W3.5 | coordinator loop, ARQ fallback, class routing |
| `test_arc_offset` | W4.2 | §12.3 goldens, quantization, re-anchor, clamps |
| `test_arc_heap` | W4.2 | guard bound, release order, slab |
| `test_arc_sidecar` | W4.3/W4.5 | ordering under bursts, modes, trace replay, kill switch |
| `test_{wm,fluid}_control`, `test_arc_deploy` | W2.4/W4.4 | Python loops against live segments/sidecars |
| `e2e_{cache_fill,repair,apply}.sh` | W2.6/W3.6/W4.5 | degradation profiles, loss ladder, bursty apply |
| `fuzz_*` | W5.1 | codec streams, embodiment vectors, ARC release streams |

## 10. Bench gates

Recorded by `make bench` into `build/bench_results.txt` (never
ctest-gated), thresholds enforced by `make ci` on the reference runner
at 2× headroom:

| Bench | Threshold (activate) | Source gate |
|---|---|---|
| `bench_wm` select | ≤ 15 cyc/decision (W2.6) | WAM J.3 spray gate |
| `bench_arc` | ≤ 200 ns/block target; 400 enforced on a named reference runner (W6.12) | WAM J.3 ARC gate |
| `bench_origin` | fetch issue+completion p99 (W1.4, recorded only) | new |
| `bench_e2e_fill` | fill p99/variance per arm (W2.6, recorded) | design 01 gates |
| `bench_e2e_repair` | p99 vs overhead per arm (W3.6, recorded) | design 02 Pareto |

Nightly `bench.yml` fails on > 5% regression vs the 7-day rolling median
(00 §11.3 stays authoritative).

## 11. Risk register

| ID | Risk | Mitigation (design-in) |
|---|---|---|
| S1 | Adaptation breaks imported semantics silently | ADR-0009 golden parity + bridge tests per 07 §3; parity gate in `ci` |
| S2 | ARC ABI v2 diverges from paper under refactors | §12.3 goldens + L.6-style replay pinned in `test_parity` |
| S3 | LT arm (fresh code) has a decode bug | roundtrip + loss tests + fuzz budget; LT is the A/B reference, never the default |
| S4 | nanorq pin mismatch vs validated upstream | import map §7: pin to the wack_a_mole-validated commit family; correct URLs in the same commit |
| S5 | SHM seqlock misuse (torn profile) | ADR-0002 single-writer/single-reader; contention tests; version field per in-flight fill (design 01 open question #1) |
| S6 | Python loop stalls → stale profile | C `hard_down` fast path (W2.2); liveness metric with pager (W2.4); proxy keeps last-good profile |
| S7 | Controller fights C-side mark-downs | W2.4 resolves design 01 open question #3 (one-interval deference) |
| S8 | Origin HTTP driver variance masks e2e signal | loopback knobs are the primary gates (W2.6/W3.6); HTTP leg is a smoke, not a gate |
| S9 | ARC added latency on sparse pipelines | `budget_ms` clamp + shadow-first + auto-kill (W4.3/W4.4) |
| S10 | Repair gate fires before decode sufficiency | ARQ fallback mandatory (W3.5); fallback rate is a rollout gate (< 2%) |
| S11 | Config schema sprawl | strict loader rejects unknown keys (ADR-0005); schema changes ride their ADR |
| S12 | Rust seam drift (W5.3+) | cbindgen header diff in CI; golden vectors shared with C |

## 12. Sizes & milestones

Sizes are engineer-days, **P50–P90** (±30–40%), agent-assisted calendar
at 0.35–0.55× solo time (§1 velocity model).

| Wave | Scope | P50–P90 | Subtotal |
|---|---|---|---|
| W0 | foundations (W0.2–W0.5) | 6–8 | 6–10 incl. W0.1 |
| W1 | origin seam | 7–11 | 7–11 |
| W2 | P1 dispatch + proxy | 15–22 | 15–22 |
| W3 | P2 FLUID repair | 15–24 | 15–24 |
| W4 | P3 ARC sidecar | 9–16 | 9–16 |
| W5 | hardening/packaging | 7–11 | 7–11 |
| **Total** | | **59–92** | **~60–95 engineer-days** |

| Milestone | Waves | Gate (done when) |
|---|---|---|
| M0 | W0 | `make ci` green from clean clone; parity harness runs goldens; all example TOMLs load strict |
| M1 | W1 | loopback + HTTP drivers serve ranged fetches; stats exact under knobs; loadgen reproduces profiles |
| M2 — P1 5% | W2 | e2e_cache_fill green under all degradation profiles; discrepancy bounds hold; bytes neutral; spray cost ≤ 15 cyc |
| M3 — P2 10% | W3 | loss ladder byte-exact with ARQ fallback < 2% at ε = 0.10; Pareto table printed; O.1 bounds pass |
| M4 — P3 shadow | W4 | replay: QG 0 violations; kill-switch leg green; deploy loop drives a live sidecar |
| M5 | W5 | fuzz smoke in `ci`; install + cffi against installed artifacts; cbindgen diff clean |

Calendar (solo + agent, this repo's mode): **M0–M2 ≈ 3 weeks; M0–M5 ≈
5–7 weeks.** Two-engineer split: W3 ∥ W4 after W2, ≈ 4–5 weeks.
Contingency +20–25% if a prompt goes stale mid-flight (§1).

## 13. Work-log protocol

Full rules in `CONTRIBUTING.md` (repo root). Summary:

1. `plan: <prompt ID>` before implementation; `src:`/`tests:`/`include:`
   commits cite the ID (e.g. `src: W3.4 — FLUID sender budget`).
2. After each landing: `docs: <ID> outcome recorded, next = <ID>` —
   update the ledger above (per-wave %, what is missing) and the owning
   design doc's status header.
3. Per milestone: `docs: truth-pass` syncing every status claim against
   the tree, and a ledger refresh pinned to the head hash.
4. Evidence rule: completion claims cite test names, gate numbers, or
   commit hashes — never adjectives.
5. Exceptions to import-first (07 §1) must cite the import map in the
   commit message (see W3.3 for the precedent).

## 14. Phase W6 — Production hardening & the migration begins

The post-M5 backlog, written at the truth-pass that closed Wave 5.
Its first three prompts close the KNOWN GAPS recorded in the ledger
above (owned, not discovered later); the rest discharge the
environment-blocked legs and begin ADR-0003's migration for real.
Sizes are engineer-days, P50–P90, the §1 velocity model.

Sequencing (leverage first):

```text
W6.1 (scrape + telemetry chain)   <- closes gaps 1+3, highest value
W6.2 (ARC goldens)                <- cheap, finishes the parity story
W6.5 (hosted CI matrix)           <- DONE (cbc5bd9): legs real + wired
   |- W6.3 (multi-block)          <- DONE (ac9332c, 3104fd2)
   |- W6.4 (CFFI surface)         <- DONE (9d56e78, c099e03, 301e306)
W6.6 (v1.0 release)              <- DONE (ef737b9)
W6.8 (Rust LT)                   <- DONE (77e0292, 568c30c, 180f025);
                                    C/Rust comparison window open; C default
W6.7 (S3/peer)                   <- existing; endpoint/stand-in gated
W6.9 (ARC entry retention)       <- DONE (this change set)
   |- W6.10 (true ARC shadow)     <- DONE (this change set)
   |    └─ W6.14 (observability)  <- DONE (this change set)
   └─ W6.12 (measurement gates)   <- benchmark the corrected ARC
W6.11 (hosted + conformance)     <- NEXT; external runner; expanded corpus
   |- W6.12 -> W6.13 (LT decision) <- evidence review, not automatic cutover
   |- W6.15 (production HTTP)     <- hosted integration/transport evidence
   └─ W6.16 (isolated wheel)      <- hosted packaging evidence
W6.16 + W6.14 -> W6.17 (RPM/Helm) <- deploy only tested artifacts
W6.9 + W6.10 + W6.11 + W6.12 + W6.13
                                  -> W6.18 (opt-in Rust ARC)
```

### PROMPT W6.1 — Live metrics scrape + the telemetry chain, closed end to end

**Objective:** close the two production-facing gaps in one prompt: a
live scrape endpoint (gap 1) and the full C-proxy -> SHM telemetry ->
Python-loop -> published-profile chain running as one live pair
(gap 3).

**Import:** wack_a_mole `include/wam/metrics_endpoint.h` +
`src/runtime/metrics_endpoint.c` @ a0f0422 (K.1b: env-gated
`WAM_METRICS_ENDPOINT`, minimal HTTP GET serving the text dump,
exact Content-Length, pinned by `test_metrics_endpoint`).

**Deliverables:** `include/sdf/common/metrics_endpoint.h` +
`src/c/common/metrics_endpoint.c` behind `SDF_METRICS_ENDPOINT`
(host required; empty = 127.0.0.1); wired into wm-proxy and the ARC
sidecar, each serving its own dump; wm-proxy pushes
`sdf_origin_stats` snapshots via `sdf_wm_shm_telemetry_push` on its
control ticks; the Python loop (`wm_control.py`) gains a live leg
driving a real proxy through the segment.

**Acceptance:** endpoint unit test in the WAM shape (bad addrs
refused, 200 + exact Content-Length, accept loop keeps serving);
one integration harness: live proxy + Python loop as one process
pair, profile version advancing through SHM per control interval,
heartbeat visible on the scrape; `e2e_cache_fill.sh` gains a
`--scrape` smoke leg.

**Size:** 3–5 days.

### PROMPT W6.2 — ARC goldens extracted; the parity flip

**Objective:** make `golden_vectors/arc_schedule.json` the pin, as
the W0.4 schema intended — the 12.3 cycle currently lives in C
(`test_arc_core`), the vector file is empty.

**Import:** the extraction pattern from `tools/gen_goldens_fountain.c`
(W3.3); the cycle already proven on the real clock (W4.2):
S_n = now − d, expected D pair {94.7934, 93.8016} ms.

**Deliverables:** `tools/gen_goldens_arc.c` (extracts through the
public ABI: the linear-cycle drive + expected D pair; the exponent
band entry); `arc_schedule.json` `paper_12_3` group filled;
`test_parity` IMPLEMENTED gains `arc.abi_v2` and validates;
`trace_replay_60fps` stays in C (the replay is timing-based, not a
vector — note that in the file's comment).

**Acceptance:** 375+ comparisons, zero pending groups; extractor
provenance documented in the file.

**Size:** 1–2 days.

### PROMPT W6.3 — Multi-block repair jobs

**Status:** done — see the ledger (ac9332c, 3104fd2).

**Objective:** the fountain contract pinned one block per codec
object with multi-block looping at the coordinator; W3.5 ran single
blocks. Real repair jobs are multi-block objects.

**Import:** none new — the W3.5 coordinator's per-block loop
generalizes; per-class eps comes from the design-02 class table
(hot-metadata 0.05, cold-checkpoint 0.15).

**Deliverables:** job admission for N-block objects (per-block codec
objects, one budget/gate per block), ranked-symbol scheduling across
blocks, per-block ARQ fallback, per-block round accounting in the
Pareto line.

**Acceptance:** a multi-block e2e leg (a 4-block checkpoint object
under the loss ladder) byte-exact per block; the REPAIR line carries
per-block rounds; single-block behavior unchanged (existing legs
green untouched).

**Size:** 3–4 days. Parallel-safe after W6.1.

### PROMPT W6.4 — CFFI bindings for the full control surface

**Status:** done — see the ledger (9d56e78, c099e03, 301e306). The
true-shadow limitation recorded at that landing was closed by W6.10.

**Objective:** `build_ffi` cdefs `origin.h` + `wm.h` today; the
Python deploy loop runs against a stubbed transport. Extend the
chain so the control plane is real end to end.

**Import:** the header-graph lesson from W2.4's CFFI fix (cdef order
follows the include graph).

**Deliverables:** cdef chain extended to `arc.h`, `fluid.h`,
`fountain.h` (order per the include graph); verified against
INSTALLED artifacts (the W5.2 install leg's surface, not the build
tree); `arc_deploy.py`'s transport gains a CFFI-backed
implementation; `sdf_cffi` re-exports the new surface.

**Acceptance:** the deploy-controller integration test drives a live
C ARC sidecar through CFFI — mode flips land within one pump
interval (the design-03 contract), verified against installed
artifacts.

**Size:** 2–3 days. Parallel-safe after W6.1.

### PROMPT W6.5 — Hosted CI matrix (the advisory legs go real)

**Objective:** discharge the wack_a_mole L.8 posture for this repo: a
runner that executes what this environment could only skip.

**Deliverables:** `ci.yml` made real: clang-format + clang-tidy legs
(first real format/tidy pass — expect churn, land it as its own
`style:` commit first); clang in the matrix; the full ctest suite
with sanitizers; the bench-gate leg (seeded spray > 30 cyc fails);
the W5.2 install leg; and — once cargo exists — the rust-seam
declaration diff flips from advisory to AUTHORITATIVE. `bench.yml`
nightly: fail on > 5% regression vs the 7-day rolling median
(00 §11.3, never ctest-gated).

**Acceptance:** one green hosted run of the full matrix; the
rust-seam leg green on the runner; a follow-up `style:` commit if
format/tidy found churn (expected).

**Size:** external-runner dependent; 1–2 days of wiring. Unblocks
W6.8.

### PROMPT W6.6 — v1.0 beachhead release

**Status:** done — see the ledger and
[docs/releases/v1.0.0.md](../releases/v1.0.0.md) (ef737b9). The tag is
`v1.0.0`; the later hosted run 44774290565 at 0d6e60a is green, while
retained nightly history remains an external evidence item.

**Objective:** the wam release pattern: the honest changelog is
already written — it is the ledger.

**Deliverables:** version bump to 1.0.0 in `include/sdf/version.h`
(single source of truth — the .pc and find_package versions follow);
release notes compiled FROM the ledger (per-wave evidence, the
instructive finds, the goldens counts); tag `v1.0.0`; the
placeholder `packaging/debian` made real — one working `.deb` built
and installed by a leg (rpm/helm stay placeholders with a note).

**Acceptance:** the install leg + the package-build leg green;
release notes cite hashes/test counts, no adjectives.

**Size:** 2–3 days. After W6.1–W6.5 land.

### PROMPT W6.7 — Deferred-origin drivers: S3 + NVMe peer (design-note-gated)

**Objective:** ADR-0008 deferred these behind design notes; schedule
them when a real endpoint exists to test against.

**Deliverables:** the two design notes first (09 §9's open
questions: S3 authorization model — instance roles vs signed URLs;
drain timeout semantics for `set_enable`); then the drivers behind
the same ops, tested against local stand-ins (a minio-style S3 for
the former; a peer loopback for the latter).

**Acceptance:** both drivers pass the loopback-leg test shapes
(stats exactness, drain, EAGAIN); the design notes close 09 §9.

**Size:** 4–6 days. Schedule-gated on a real endpoint (or the
stand-in decision).

### PROMPT W6.8 — Functional Rust: `sdf-fountain` behind the goldens

**Status:** done — implementation, build/gates and design documentation
landed as 77e0292, 568c30c and 180f025. Local and hosted acceptance are
green; the comparison window remains open with C as the default because
paired performance and nightly history are still missing. Hosted rust-seam
run 44774290565 at 0d6e60a passed the then-current 27-test/1,410-comparison tree.

**Objective:** begin ADR-0003's migration for real — the internal LT arm
in Rust, exporting the same `lt_*`/`lt_prng_next` `extern "C"` symbols
behind unchanged public C dispatch, gated by the golden vectors and the
cbindgen declaration-surface diff.

**Deliverables:** the `sdf-fountain` crate grows a real implementation of
the LT arm first (the reference codec — simpler, fully specified in-repo);
CMake links the Rust staticlib in place of only the C LT/PRNG objects behind
`SDF_USE_RUST_FOUNTAIN`, merging it into the existing `libsdf_fountain.a`.
The current harness's **76 fountain comparisons** — 17 roundtrip, 17
lossy-repair, 21 nanorq and 21 LT — run through BOTH builds within **556
total comparisons**. The prompt-era 356-fountain figure is superseded,
not current harness evidence. The aggregate gate runs locally and is wired
into hosted CI.

**Acceptance:** golden vectors pass through both implementations with
byte-identical complete reports; the parsed cbindgen declaration diff is
empty; no caller changes (ADR-0003's contract). nanorq stays C (the vendored
library IS the validated upstream — migrating the adapter adds no proof).
Full `make ci` is green, including both 23/23 variant suites, Rust 4/4,
Rust-backed ASan+UBSan 23/23, exact archive symbols, and installed
pkg-config/CMake/CFFI/Debian consumers executing both codecs. Sanitizer
coverage is limited to instrumented C callers and the allocator boundary;
no Rust-internal ASan, performance-cutover, C-removal or hosted-run claim
is made.

**Size:** 5–8 days. Gated on W6.5 (cargo in CI).

### PROMPT W6.9 — Preserve ARC sidecar entries across pacer backpressure

**Status:** done — see the ledger; the saturation, duplicate, ordering,
kill-drain, and lifecycle legs are green.

**Objective:** close the accepted-entry loss window in the ARC sidecar when
its input ring accepts an entry but `arc_submit()` subsequently returns
`-EAGAIN` because the pacer slab or heap is full.

**Evidence/rationale:** source inspection at the baseline shows
`arc_sidecar_pump_once()` popping a slot and breaking on `-EAGAIN` without
restoring or retaining it (`src/c/arc/arc_sidecar_main.c`, pump loop).
`arc_submit()` can return `-EAGAIN` on slab/heap exhaustion
(`src/c/arc/arc_pacer.c`). The comment says to re-push the entry, but no such
operation exists. This is a source-audited finding; the new regression test
must demonstrate it before the fix rather than claiming a reproduced failure
now.

**Deliverables:**

- A regression that fills a future-dated QG pacer, refills the input ring,
  forces internal pacer backpressure, and then drains all accepted entries.
- Pump-owned pending-entry retention, or an equivalent non-destructive ring
  handoff, preserving FIFO and exactly-once application without changing the
  public ABI.
- Explicit stop/mode-flip behavior for pending plus ring plus pacer entries;
  cleanup must not free storage while an accepted entry remains stranded.
- Focused saturation, duplicate, ordering, kill-drain, and lifecycle tests.
- Design/ledger documentation distinguishing caller-refused `-EAGAIN` at the
  public submission ring from internal, already-accepted backpressure.

**Acceptance:**

- The new saturation regression fails on the pre-fix implementation and passes
  after it.
- Every submission for which `arc_sidecar_submit()` returned 0 is eventually
  applied exactly once, with payload identity and required ordering intact,
  across repeated saturation and shadow/live/killed transitions.
- A full input ring still returns `-EAGAIN` to the caller, which retains
  ownership of that unaccepted entry; it is not conflated with internal retry.
- Stop and kill-drain leave no accepted entry in the pending slot, input ring,
  or pacer, and produce no late callback after destruction.
- Existing ARC goldens and tests remain green; focused ARC tests,
  ASan+UBSan, TSan where applicable, and full `make ci` pass.

**Size:** 1–2 days. **Depends on:** none.

---

### PROMPT W6.10 — True ARC shadow scoring and non-vacuous promotion evidence

**Status:** done — see the ledger; shadow now runs a real non-applying QG
schedule and the promotion gate rejects vacuous evidence.

**Objective:** replace shadow's current mapping to `ARC_MODE_IMMEDIATE` with a
real non-applying QG schedule, so shadow added-latency and promotion decisions
measure the algorithm that live mode would execute.

**Evidence/rationale:** `docs/design/03-arc-pacing.md` recorded at the W6.4
baseline that shadow metrics and promotion SLO evidence were vacuous. The
deploy controller therefore needed both real schedule metrics and a
non-vacuity gate; both landed in this prompt.

**Deliverables:**

- A second non-applying release path or shadow-marked schedule that computes
  QG release times while the existing path applies each entry immediately and
  exactly once.
- Separate shadow schedule and immediate-apply metrics; no double-counting of
  applies, drops, backlog, added latency, or budget alerts.
- Deterministic per-entry shadow schedule records suitable for replay and a
  later C/Rust dual-pacer comparison.
- Deploy-controller rejection of promotion when shadow evidence is absent,
  empty, stale, or IMMEDIATE-only, while retaining the existing time/SLO gates.
- Unit, integration, CFFI, fuzz, and transition tests for
  shadow→live→killed, backlog draining, and W6.9 saturation.
- Internal-only stats/layout extensions where possible; otherwise stop and
  process the required public-ABI ADR/version change before implementation.

**Acceptance:**

- For identical timestamped inputs, shadow computes the same release schedule
  as live QG while shadow apply timing remains immediate.
- The bursty-trace harness emits genuine shadow added-latency samples; zero
  valid samples or IMMEDIATE-only samples cannot satisfy promotion even after
  a simulated 24 hours.
- Valid clean samples plus the existing time and alert requirements permit
  promotion; historical shadow alerts are not recounted as live breaches.
- Killed mode drains accepted backlog with zero drops, no ordering violations,
  and no late shadow callback; mode changes cannot duplicate an apply.
- Existing extracted ARC goldens pass unchanged. Any new shadow vectors are
  extracted from the corrected C implementation through its public ABI.
- Python deploy tests, C ARC tests, both sanitizer suites, and full `make ci`
  pass. A fixture-shortened run is not reported as a production 24-hour soak.

**Size:** 4–6 days. **Depends on:** W6.9.

---

### PROMPT W6.11 — Authoritative hosted and expanded conformance evidence

**Status:** partially implemented — the authoritative hosted matrix is green
at `0d6e60a` (run 44774290565, 10/10 matrix jobs); retained nightly history is
still pending. The local conformance slices, counts, and honest skips are
recorded in the ledger above.

**Objective:** close M6's remaining hosted-evidence line and turn the current
smoke/fixed-shape checks into a durable regression corpus without inflating
comparison counts or claiming unexecuted coverage.

**Deliverables:**

- Configure the repository remote/runner and execute the complete existing CI
  matrix at recorded revisions, including the authoritative W6.8 Rust leg.
- Exercise the nightly workflow and preserve honest baseline/insufficient-
  history behavior; do not claim a seven-day median before it exists.
- Persistent minimized fuzz corpora and regression inputs, including W5's ARC
  park/stranding find and W6.9's saturation case, plus deterministic replay.
- Input-derived fountain fuzz geometries across supported K/symbol lengths and
  both codecs; malformed streams retain bounded/no-crash behavior.
- WM rejection-immutability checks and real residual-fairness hit/conservation
  assertions replacing placeholder checks.
- Additional provenance-recorded fountain geometries extracted from the C
  implementation, without altering existing expected bytes by hand.
- An installed-artifact CFFI golden consumer that complements, rather than
  replaces, `tests/c/test_parity.c` and the existing C harness (1,410
  comparisons).
- Ledger entries with hosted run IDs, revisions, toolchains, exact comparison
  counts, skips, and failures.

**Acceptance:**

- One actual full-matrix hosted run is green at a recorded revision. Met by
  run 44774290565 at `0d6e60a`; W6.11 remains open only for retained nightly
  history.
- Nightly execution is observed, and first-baseline/insufficient-history skips
  are reported exactly as executed. This line is not met yet.
- Every checked-in regression input reproduces its intended invariant in the
  replay leg; smoke and full fuzz budgets pass.
- New fountain tests cover multiple supported shapes and both codecs; the WM
  rejection test compares complete pre/post profile state, and fairness tests
  inspect actual recipient hit counts.
- The CFFI consumer runs against installed artifacts and reports zero pending
  applicable groups. Existing aggregate parity remains zero-pending; new
  counts are derived from the harness/vector files, not guessed.
- No hosted, coverage, or historical-performance statement lacks a linked run
  artifact.

**Size:** 3–5 days plus runner and nightly-history waits.
**Depends on:** external remote/runner; W6.9 for its final regression corpus.

---

### PROMPT W6.12 — ARC benchmark activation and paired C/Rust LT measurements

**Status:** partially implemented locally. Landed: `bench_arc` records the
IMMEDIATE and QG paths for named batch/queue sizes (64/256/1024) with
submit/poll/release ns/block, block counts, entry size, revision, runner,
build, and collection time; `make bench` records all three batch sizes and
then runs `scripts/bench_arc_gate.py`. The gate requires canonical immediate
and QG records, fails on missing, malformed, stale, incomplete, or
wrong-runner samples, reports the 200 ns/block target as met or missed
separately, and enforces the 400 ns/block limit only when
`SDF_BENCH_REFERENCE_RUNNER` names the runner (or `--reference-runner` is
passed). Negative tests cover those paths, and the existing synthetic
nightly-history selftest exercises the >5% rule and insufficient-history skip.
`bench_fountain` records init/generate/push/decode, p99, and throughput;
`make bench-paired` builds the same binary with C and Rust LT, runs identical
configurations, validates the pairs, and writes
`out/bench/fountain_paired.txt`. `make bench` also writes
`out/bench/bench_report.json`, a machine-readable aggregate of every recorded
WM, ARC, origin, fountain, and REPAIR line.
Local single-runner records put canonical batch-64 QG at roughly 115–170
ns/block and immediate at roughly 115–170 ns/block; batch 1024 reached roughly
220–290 ns/block in the same runs, so the 200 ns target is not universal across
queue depths. These are local observations, not a baseline or a cross-machine
claim. Paired same-runner C/Rust LT measurements now exist locally
(`out/bench/fountain_paired.txt`); CI has a dedicated `fountain-bench` job,
and the nightly bench workflow records and uploads a `fountain-paired`
artifact, fetches the prior seven days, validates each historical file as a
complete C/Rust pair, and writes a per-configuration multi-night median
report. Still open: a stable named reference runner, seven retained nightly
samples, and the later C/Rust ARC comparison hooks. No hosted or cross-machine
performance evidence is claimed yet.

**Objective:** implement the missing performance evidence before using ARC's
200 ns/block target or Rust LT results as a gate, baseline, or cutover argument.

**Deliverables:**

- `bench_arc` workloads for submit/poll/release, immediate and queued QG paths,
  named batch sizes and queue depths, with machine-readable ns/block output.
- Reference-runner activation of the 200 ns/block target and the plan's 2× CI
  failure headroom; microbenchmarks remain outside CTest.
- Negative tests proving missing, malformed, stale, and regressed ARC samples
  fail a required gate rather than silently skipping.
- Paired same-runner C/Rust LT encode, generate/push, peel/decode, and repair
  benchmarks across several supported K, symbol sizes, and loss patterns.
- Raw benchmark artifacts keyed by revision/runner/flags and reusable hooks for
  a later C/Rust ARC comparison.
- Synthetic nightly-history tests for the >5% regression rule and its
  insufficient-history behavior.

**Acceptance:**

- `make bench` runs `bench_arc` and emits stable named records; the enforced
  reference-runner threshold fails above 400 ns/block, while the 200 ns/block
  result is reported separately as target met or missed.
- Existing WM thresholds and e2e benchmark reports remain intact.
- Identical C/Rust LT workloads and iteration budgets produce comparable p99
  and throughput artifacts; no cross-machine comparison is accepted as paired
  evidence.
- Synthetic regressions trip the intended gates; insufficient real history
  skips rather than fabricating a baseline.
- Results are published as measurements only. This prompt makes no default
  cutover, C-removal, or universal performance claim.

**Size:** 3–5 days plus baseline collection.
**Depends on:** W6.9 and W6.11's stable reference runner.

---

### PROMPT W6.13 — Fountain comparison-window evidence review and decision

**Status:** decided locally — C remains the default LT arm; Rust LT stays
opt-in; no C is removed and no default changes. ADR-0010 records the retention
decision, explicit revisit criteria, and rollback plan. The accompanying
[review](w6.13-lt-comparison-review.md) records the available W6.8 and local
evidence plus the missing hosted/paired measurements. This is the prompt's
status-quo branch, not a cutover recommendation.

**Objective:** close the W6.8 LT comparison window with an explicit per-module
decision grounded in hosted correctness and same-runner performance evidence.

**Deliverables:**

- A joint report covering W6.8 ABI/golden/symbol/package results, W6.11 hosted
  and expanded-conformance evidence, and W6.12 raw paired measurements.
- At least seven paired same-runner nightly samples, or a documented decision
  to retain the status quo while evidence remains insufficient.
- Confirmation that a release containing the opt-in Rust LT and retained C arm
  has completed the ADR-required comparison window before any default-change
  recommendation.
- An ADR recording one decision: retain C default, retain both with Rust
  opt-in, or authorize a subsequent default change with rollback criteria.
- Explicit C-retention/removal criteria and a rollback plan; nanorq remains C
  and outside this decision.

**Acceptance:**

- A cutover recommendation is invalid without hosted correctness, unchanged
  public declarations/callers, byte-identical deterministic parity, both-
  variant consumers, and performance within the documented nightly noise rule.
- Missing, stale, mixed-runner, or contradictory evidence results in C retained
  and default, not a conditional performance claim.
- W6.13 itself neither changes the CMake default nor deletes C. Any approved
  default switch/removal is a separate reviewed landing with its own tests and
  rollback.
- Historical v1.0.0 release notes remain the record at that tag; current
  status lives in the design/ADR/ledger documents.

**Size:** 2–3 days plus comparison-release and sample-collection waits.
**Depends on:** W6.11 and W6.12.

---

### PROMPT W6.14 — Executable metrics, dashboards, alerts, and Python export

**Status:** implemented — local gate and hosted CI are green; no hosted
Grafana import claim.

**Objective:** reconcile the documented metric inventory, actual C/Python
series, and dashboard queries so every rollout guardrail is backed by an
emitted or explicitly external signal.

**Deliverables:**

- One documented naming/schema contract reconciling design names, `sdf_`-prefixed
  scrapes, histogram conventions, bounded labels, and compatibility needs.
- Prometheus cumulative histogram `_bucket`, `_sum`, and `_count` series for
  owned latency/batch metrics, including `le="+Inf"` and monotonic buckets.
- ARC drop/backpressure accounting using W6.9's exact accepted-entry semantics,
  and genuine shadow/live series using W6.10's separated statistics.
- Python control-plane metric registration/export replacing the remaining
  metrics TODO, with bounded labels and liveness samples.
- Importable WM, FLUID, and ARC dashboards plus warning/page alert tests at the
  documented thresholds; externally owned engine metrics require an explicit
  input/exporter contract rather than silently empty panels.
- Automated query validation against live scrapes and checked-in external
  fixtures where applicable.

**Acceptance:**

- Every dashboard query resolves to a locally emitted series or a declared
  external fixture; no nonexistent histogram/drop series remains hidden.
- Histogram counts equal observed events, sums match test inputs within their
  numeric tolerance, buckets are cumulative, and labels remain bounded.
- W6.9's accepted-entry saturation test keeps the dropped counter at zero;
  caller-refused submissions and actual invalid-input drops are distinguishable.
- ARC dashboards distinguish shadow schedule latency from immediate/live apply
  behavior and expose kill-switch state.
- Python tests exercise `prometheus_client.generate_latest()` and liveness;
  alert tests cover every warning/page boundary, including any ARC drop.
- Full Python/C tests and the new observability validation leg pass. Local
  query validation is not represented as a hosted Grafana import.

**Size:** 3–5 days. **Depends on:** W6.9 and W6.10.

---

### PROMPT W6.15 — Production HTTP origins, HTTPS, and measured transport A/B

**Status:** partially implemented locally; this slice is explicitly not the
full production-transport gate. Landed: explicit origin-class selection for WM
and FLUID (loopback and HTTP drivers, clear `-EINVAL`/`-ENOTSUP` failures for
mixed/unknown/S3/peer sets), strict `[[origin]]` materialization in both
coordinators, retained harness modes, controlled in-process HTTP tests for WM
fill/drain and multi-block FLUID repair, bounded rejection of overlong bodies,
and a reusable grow-on-demand per-origin HTTP body buffer (no body allocation
after warmup, verified by transport-buffer counters). Controlled HTTP coverage
now also includes chunked partial responses, truncated-body recovery, and
queue saturation returning the documented `-EAGAIN` without blocking. The
driver enables `TCP_NODELAY` on its persistent connection; without it the
measured sequential p50 was dominated by a ~41 ms Nagle/delayed-ACK stall.
A keep-alive peer may also close an idle connection before sending any response
byte; the driver now reopens and resends the idempotent Range GET once instead
of surfacing a transient `-EIO`. Controlled tests add that stale-connection
reconnect, a slow-but-in-timeout response with latency-estimator evidence, and
a malformed 206 without `Content-Length`, alongside the exact-stat checks.
`bench_origin` now records issue-to-completion p50/p99, goodput,
connection reuse, post-warmup allocation, and saturation behavior against a
deterministic in-process Range server, naming the driver, server, runner,
build, flags, and sample counts. These are single-runner loopback
measurements, not a production
or hosted baseline, and no improvement is claimed. Still open: HTTPS with
CA/hostname verification, TLS certificate-failure tests, repeated named-runner
baselines before any nightly gate, and the optional `SDF_HAVE_CURL` A/B (the
decision is recorded in design 09 §9; it stays out of the default build). No
hosted TLS, production-network, or soak evidence has been produced, and the
hosted CI pass does not exercise HTTPS. Does not
include W6.7 S3/NVMe-peer drivers.

**Objective:** move WM and FLUID beyond forced-loopback harness origins and
turn the reference HTTP driver into a measured, bounded production candidate
without weakening loopback determinism.

**Evidence/rationale:** the WM harness constructs loopback files and overwrites
origin classes; FLUID likewise forces loopback. HTTP currently accepts only
`http://`, allocates per unit, and is explicitly a smoke/reference leg, while
shipped environment examples contain HTTPS origins. Risk S8 and the HTTP
parking-lot entries therefore require both integration and measurement work.

**Deliverables:**

- Class-based driver selection and real origin configuration for WM and FLUID,
  retaining explicit harness modes and failing clearly on unsupported classes
  instead of silently converting them to loopback.
- HTTPS with certificate/hostname verification and explicit CA configuration;
  no insecure default or silent HTTP downgrade.
- Bounded connection/response resources, correct partial-response and timeout
  handling, and removal of per-unit allocation after warmup on the fetch/
  completion path.
- Local controlled HTTP/TLS integration tests for WM fill and FLUID repair,
  including reconnect, slow server, malformed response, certificate failure,
  drain/restore, saturation, and exact normalized statistics.
- `bench_origin` issue-to-completion p99, goodput, connection reuse, and post-
  warmup allocation measurements against named local servers/runners.
- A design decision on optional `SDF_HAVE_CURL`; if admitted, implement it OFF
  by default behind the same origin vtable with POSIX/curl correctness and
  performance A/B. No libcurl dependency enters the default build.

**Acceptance:**

- HTTP/HTTPS configurations select `sdf_http_ops()` in both coordinators;
  loopback e2e behavior and injected-knob determinism remain unchanged.
- Local TLS WM fill and multi-block FLUID repair are byte-exact under scripted
  loss/reconnect/timeout cases, with ARQ fallback and exact stats intact.
- Invalid certificates fail closed; unsupported S3/peer classes fail clearly
  and remain exclusively W6.7 scope.
- No fetch/completion-path allocation occurs after warmup; saturation returns
  the documented backpressure result without dispatcher head-of-line blocking.
- Benchmark artifacts identify server, runner, flags, workload, and sample
  counts. Establish a baseline before activating any nightly regression gate;
  no improvement is claimed without repeated measurements.
- Builds without libcurl stay clean; if the optional leg lands, equivalent
  responses yield equivalent normalized origin statistics.

**Size:** 6–10 days plus baseline collection.
**Depends on:** W6.11 for authoritative hosted integration legs; W6.14 for
production rollout telemetry, but core implementation may proceed in parallel.

---

### PROMPT W6.16 — Isolated-installable Python/CFFI wheel

**Status:** implemented locally for both C (LT + nanorq) and Rust-ON variants;
the hosted C wheel is green and the matrix now also builds the ON wheel, which
has not executed yet. Hosted run 44774290565 at 0d6e60a
built the cp312 Linux wheel, ran the isolated venv and bundled-CFFI golden
consumer, and uploaded the `sdf-wheel` artifact. Landed: one CFFI
loader (`sdf._cffi`, re-exported by `sdf_cffi`) resolving `SDF_CFFI_DIR`, the
in-tree `build/cffi`, the wheel-bundled extension, then a top-level
`_sdf_cffi`; a staged-prefix wheel build that folds the static archives and
nanorq into one platform wheel; content, RPATH/RUNPATH/NEEDED, isolated-venv,
console-entry-point, ARC-sidecar, LT/nanorq-roundtrip, 1,376-comparison golden-
consumer, and explicit-development-override checks; `make wheel`/`make
wheel-test` with the test wired into the local `ci` target; and
build/variant/limitation documentation. On this runner the OFF wheel is
`sdf-1.0.0-cp314-cp314-linux_x86_64.whl`; the isolated venv runs outside the
checkout with `PYTHONPATH` and `SDF_CFFI_DIR` unset, copies the golden consumer
outside `bindings/cffi` so only the wheel-bundled extension can satisfy the
import, and reports WM/ARC/real-codec fountain coverage plus the 34 toy-only
comparisons that shipped artifacts do not contain. A hosted setuptools link
can add an RPATH/RUNPATH; the build strips it before packaging (patchelf or
chrpath) and re-checks the result. The
offline runner creates that venv with `--system-site-packages` and installs
with `--no-deps`, so this is payload/runtime evidence, not dependency-
resolution evidence. The local ON wheel is green: the cp314 platform wheel
bundles the Rust LT staticlib and passes the isolated venv, ARC sidecar,
LT/nanorq roundtrip, and 1,376-comparison golden checks. The hosted wheel job now has an OFF/ON matrix; only OFF has executed, so no
hosted ON artifact is claimed yet.

**Objective:** ship the Python control plane and its compiled binding as one
installable artifact, eliminating the current checkout/`PYTHONPATH`/
`SDF_CFFI_DIR` dependency for installed consumers.

**Deliverables:**

- Package the CFFI loader and generated `_sdf_cffi` shared object together with
  the `sdf` Python package and console entry point.
- A reproducible wheel build against a named installed C or Rust archive
  variant, preserving development-only loader overrides without requiring them
  at runtime.
- Wheel content and native dependency inspection, plus isolated installation
  tests from a working directory outside the checkout.
- Documentation for build prerequisites, bundled versus external native
  libraries, supported platforms, and variant selection.

**Acceptance:**

- The wheel contains the Python loader, package data, entry point, and compiled
  `_sdf_cffi` object; its dependency resolution contains no accidental source-
  or build-tree library path.
- In a fresh venv outside the checkout, with `PYTHONPATH` and `SDF_CFFI_DIR`
  unset, installing only the wheel and declared dependencies executes ARC
  sidecar control and LT/nanorq fountain operations successfully.
- The isolation test runs for both `SDF_USE_RUST_FOUNTAIN=OFF` and `ON` without
  caller changes; development overrides continue to work only when explicitly
  supplied.
- Existing installed-prefix CFFI tests, Python tests, and package consumers
  remain green.

**Size:** 2–4 days. **Depends on:** W6.11 for authoritative hosted wheel legs.

---

### PROMPT W6.17 — RPM artifacts and an operable control-plane Helm chart

**Status:** partially implemented locally — the long-running `sdf serve`
entry point loads the strict TOML environment, ticks controllers at the WM
cadence, attaches WM through `SDF_WM_SHM` and the ARC sidecar through
`SDF_ARC_SIDECAR`, writes atomic health JSON including ARC mode and
FLUID class policy, serves optional `/healthz` and `/readyz` HTTP probes, and
shuts down on SIGINT/SIGTERM. `sdf status` reads the health record including
ARC mode and FLUID classes, and the installed-wheel smoke runs two service
ticks and the status command outside the checkout. The control-plane Helm
chart now exists with strict ConfigMap mounting, HTTP probes, security
context, resources, and dev/staging/prod values; `make helm` passes Helm
4.3.2 lint and rendering, and ci.yml has a pinned-Helm job that uploads the
rendered manifests. RPM packaging and cluster deployment remain blocked on a
named target distro and test cluster.

**Objective:** replace the RPM and Helm placeholders with tested distribution
artifacts while preserving the existing static-library surface and the rule
that the storage platform owns data-plane sidecar injection.

**Deliverables:**

- Select and document an RPM target; build from the same staged `/usr` install
  payload as Debian and reuse its external C/pkg-config/fountain consumer tests.
- A real long-running Python control-plane service entry point, replacing the
  CLI-only placeholder for deployment use.
- A Helm chart for the control plane only: image, strict configuration,
  Service/scrape annotations, readiness/liveness, security context, resources,
  upgrade/rollback settings, and kill-switch-safe rollout defaults.
- Dev/staging/prod values with rollout percentages defaulting to zero until
  signed gates exist; no invented production endpoints or measured-optimal
  knobs.
- RPM and Helm lint/build/test legs plus installation, upgrade, rollback, and
  uninstall documentation with explicit environment boundaries.

**Acceptance:**

- RPM installation into an isolated rootfs supports compiling and running the
  external WM/ARC and LT/nanorq consumers against only that payload.
- `helm lint` and `helm template` pass for supplied values; an available kind
  or equivalent cluster reaches Ready, executes a real control/profile
  operation, exposes W6.14 metrics, and completes rollback.
- The chart deploys no S3/NVMe-peer driver and no data-plane sidecar.
- Versioning continues to derive from `include/sdf/version.h`; public archive,
  header, and package names do not fork.
- Missing distro/cluster environments remain explicit blockers; local template
  success is not reported as cluster deployment evidence.

**Size:** 5–8 days plus external environment waits.
**Depends on:** W6.14 and W6.16; W6.11 for authoritative hosted package legs.

---

### PROMPT W6.18 — Functional opt-in Rust ARC behind the unchanged C ABI

**Status:** partially implemented locally — the dependency-free `sdf-arc`
offset-estimator slice mirrors Algorithm 1/2 and reproduces the C golden fixed
point, the release queue mirrors the stable `(T_n, seq)` ordering, and the QG
guard/deadline drain covers predecessor re-queue, park timeout, and successor
wake, plus heap-depth, added-delay percentile, and interval-error statistics;
`make rust-arc` passes fmt/clippy/tests. The C pacer remains the default;
callbacks, slab storage, sidecar behavior, the public-ABI staticlib
integration, and consumer parity are still pending.

**Objective:** port the corrected ARC pacer to Rust while preserving ordering,
deadline, callback, shadow, deployment, and rollback semantics behind the
existing public headers and callers.

**Deliverables:**

- `src/rust/sdf-arc` implementation of offset estimation, Q/QG scheduling,
  heap ordering, guard/deadline behavior, drain, callbacks, statistics, and the
  sidecar behavior required by the existing ABI.
- `SDF_USE_RUST_ARC=ON|OFF`, default OFF, with the Rust staticlib folded into
  the existing `libsdf_arc.a`; no caller, Python, public-header, or installed
  archive-name change.
- Panic containment at every FFI export and allocation-free steady-state
  operation after initialization; exact archive-symbol and cbindgen
  declaration-surface gates.
- Both-variant C/Python/e2e/fuzz suites, extracted-golden comparisons, and
  installed pkg-config/CMake/CFFI/wheel/Debian/RPM consumers.
- Dual-pacer shadow comparison using W6.10's per-entry records and paired ARC
  benchmarks using W6.12's harness.
- Comparison-window documentation modeled on W6.8/W6.13, without a cutover or
  C-removal claim.

**Acceptance:**

- Both implementations pass the same ABI, unit, integration, Python, e2e, fuzz,
  and extracted-golden suites; deterministic reports are byte-identical.
- QG ordering, guard timeout, idle re-anchor, deadline, small-entry bypass,
  W6.9 saturation, true-shadow, and kill-drain semantics match C exactly.
- C and Rust shadow runs produce matching per-entry schedules within a
  documented timing tolerance; timing-based replay does not become a fake
  hand-authored JSON golden.
- No panic crosses FFI, no allocation occurs after warmup, and sanitizer
  reports state precisely which C/Rust code is instrumented.
- Both variants execute through all installed consumers; ARC benchmark results
  satisfy the same regression process as LT before any future decision.
- C remains compiled, installed, selectable, and default. This prompt does not
  migrate Whack-a-Mole or nanorq and does not authorize ARC cutover.

**Size:** 6–9 days plus comparison-window evidence.
**Depends on:** W6.9, W6.10, W6.11, W6.12, and W6.13's decision-process
precedent; W6.16/W6.17 for the complete installed-consumer matrix.

---

### Parking lot (remaining candidates, unsized)

| Item | Note |
|---|---|
| TSNC recoding behind the fountain ABI | admission-gated on a named relay/use case and a wire-format decision; a labeled option, never default |
| Time-varying WM schedules | admission-gated on a named consumer and per-fill profile-version lifecycle |
| Signed P1/P2/P3 staged rollouts | external deployment plus 24 h/72 h/7 d soak evidence; lab results cannot substitute |

### Milestone M6

Implementation-complete at W6.1 + W6.2 + W6.5: **the production chain
closed end to end** — scrape live, telemetry driving a live proxy, the
goldens complete, the matrix wired and hosted green. Run 44774290565 at
0d6e60a completed 10/10 jobs on 2026-11-20. W6.6 (the release) has since
landed as v1.0.0 (ef737b9 plus its documentation/tag commits). W6.8 has
landed the functional Rust LT arm (77e0292, 568c30c, 180f025): the hosted
rust-seam leg now runs the current 28-test suites and byte-identical 1,410-
comparison reports. Nightly history remains the one outstanding external
evidence line.
The C/Rust comparison window is open with C still default; no cutover,
C-removal or performance claim is made. W6.7 remains gated on an endpoint/
stand-in decision. W6.9 and W6.10 have landed as the corrected ARC semantics
change set (ledger above). W6.11's local conformance slices have since landed
(deterministic ARC replay, expanded fountain fuzzing and goldens, the installed
CFFI golden consumer, and 1,410 zero-pending C comparisons). Its authoritative
hosted matrix is green at 0d6e60a (run 44774290565); retained nightly history
remains externally gated on time and runner scheduling. W6.13 is decided:
ADR-0010 retains the C default and keeps Rust LT opt-in because paired
performance and nightly evidence are still missing. The remaining W6.12 and
W6.14–W6.18 packages follow.

### Milestone M7 (proposed)

**M7 — Corrected ARC semantics and evidence-driven migration candidates** is
implementation-complete only when:

- W6.9 proves exactly-once retention under backpressure and W6.10 makes shadow
  promotion evidence non-vacuous. Both are implemented and locally proven (see
  the ledger); this bullet does not claim a hosted or production-soak run.
- W6.11's hosted full-matrix run is green at `0d6e60a` (run 44774290565)
  and the durable conformance corpora landed; retained nightly history and
  W6.12's reference-runner benchmark artifacts remain.
- W6.13 records an explicit LT decision (ADR-0010 retains the C default while
  evidence is insufficient) without implying automatic C removal.
- Metrics/dashboards, production HTTP, and distribution artifacts have their
  executable local and available-hosted acceptance legs.
- W6.18 provides an opt-in Rust ARC comparison candidate while C remains the
  default for both fountain LT and ARC.

M7 does **not** claim production P1/P2/P3 rollout gates, a 24-hour/72-hour/
seven-day soak, a Rust default cutover, or W6.7 endpoint availability.

### Deferred beyond W6.9–W6.18

- **W6.7 S3/NVMe-peer:** already specified; schedule when its endpoint or
  stand-in decision is made.
- **Signed staged production rollouts:** require real deployment owners,
  engine telemetry, dashboards, safe configs, and calendar soaks. Lab evidence
  cannot substitute for these gates.
- **TSNC recoding and time-varying WM schedules:** remain admission-gated on a
  named consumer, interoperability/wire-format decisions, and separately sized
  implementation prompts; TSNC is never default.
- **Rust Whack-a-Mole or Rust RaptorQ:** no migration merely for completeness;
  both need a new documented reason and A/B case.

*End of document.*
