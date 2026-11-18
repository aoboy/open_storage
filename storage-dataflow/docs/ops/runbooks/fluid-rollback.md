# Runbook: FLUID Repair Rollback

**Component:** Prototype 2 — slack-based fountain-coded EC repair
**Owner:** Repair pipeline
**Last reviewed:** 2026-11-16

## Purpose

Return a repair class to ARQ immediately when FLUID is unsafe, too expensive,
or failing its latency target.

## Triggers

- Repair p99 regression or unstable tail.
- Overhead ratio above 1.50.
- ARQ fallback rate above 5%.
- Any incomplete or corrupt block returned to the caller.
- Decoder CPU saturation or timeouts.
- Codec crash or suspected Nanorq correctness bug.

## Immediate rollback

1. Set `fluid.rollout_pct_by_class.<class>: 0` in the environment config.
2. Verify `fluid.arq_fallback: true` (it must always be true in production).
3. Deploy the control plane and confirm the class drains from the treatment
   arms.
4. Confirm no repair job is left in a FLUID-only path:
   - `fluid_rounds` stops increasing for the class.
   - `fluid_arq_fallback_total` returns to baseline.
   - Repair p99 returns to the ARQ baseline.
5. Check for incomplete blocks and page the storage owner if any are found.

## If the fallback itself is failing

1. Disable FLUID globally: set every class rollout to 0 and redeploy.
2. If the repair pipeline is still unhealthy, pause new repair jobs for the
   affected class through the normal repair-control procedure.
3. Preserve the codec logs, ESI traces, and golden-vector failure output.

## Diagnosis

- Compare the LT and Nanorq arms separately; a single codec may be at fault.
- Check `fluid_decode_rank` and `fluid_repair_bytes{sent,useful}`.
- Re-run the failing block through `golden_vectors/fountain_esi.json`.
- Confirm one codec object per worker/job; concurrent use is a known hazard.

## Re-enable criteria

- Root cause identified and fixed or mitigated.
- ARQ fallback path re-tested end to end.
- Golden vectors pass for both codecs.
- Overhead and p99 are back within the Stage 1 gate.
- Re-enter at 10% of one class, not at the previous rollout percentage.

## Escalation

| Situation | Escalate to |
|---|---|
| Data loss / incomplete block | Storage durability owner immediately |
| Codec correctness | Fountain codec owner |
| Overhead or p99 | Repair pipeline owner + data scientist |
| Nanorq defect | Library owner; consider vendored patch or LT fallback |

## Post-incident

- Record the failing ε, class, arm, and codec.
- Add a regression vector if the failure was correctness-related.
- Update `docs/design/02-fluid-repair.md` and the experiment log.
