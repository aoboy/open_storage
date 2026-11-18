# Runbook: Whack-a-Mole Rollout

**Component:** Prototype 1 — profile-based dispatch + soft shedding
**Owner:** Storage data plane
**Last reviewed:** 2026-11-16

## Purpose

Increase Whack-a-Mole traffic safely from 5% to 25% to 100%, stopping on any
guardrail regression.

## Preconditions

- [ ] `main` is green: C build, ABI smoke test, Python tests, lint.
- [ ] Golden vectors pass for `wm_select`.
- [ ] Target environment config has `kill_switch: false`.
- [ ] Dashboard `sdf-wm` is live and the baseline window is at least 7 days.
- [ ] On-call SRE and the proxy owner are available for the observation window.

## Stage 1: 5%

1. Set `wm.rollout_pct: 5` in `configs/<env>/wm.yaml` and deploy the control
   plane.
2. Confirm `wm_dispatch_total{arm="treatment"}` is non-zero and the control
   arm is still serving.
3. Observe for at least 24 h.

Proceed if:

- Cache-fill p99 is at least 10% better than baseline.
- Completion-time variance is at least 20% better.
- `wm_fill_bytes_total` is neutral (no material increase).
- Error rate has not regressed.

## Stage 2: 25%

1. Set `wm.rollout_pct: 25`; keep `kill_switch: false`.
2. Observe for 72 h.

Proceed to 100% only if all Stage 1 guardrails hold at 25% and no
pager-worthy events occurred.

## Stage 3: 100%

1. Set `wm.rollout_pct: 100`.
2. Keep the kill switch enabled and tested.
3. Observe for one week before closing the rollout.

## Abort criteria

Abort immediately on any of:

- Cache-fill p99 regression greater than 5% vs. baseline.
- Any error-rate regression attributable to the treatment arm.
- Byte increase greater than 1%.
- Profile publish failures, stale profiles, or control-loop liveness alerts.
- `wm_discrepancy{path}` persistently above the expected bound.

## Rollback

1. Set `wm.rollout_pct: 0` and `kill_switch: true` in the environment config.
2. Deploy the control plane and confirm the treatment arm drains.
3. Verify cache-fill p99, errors, and bytes return to baseline.
4. Preserve logs and metric snapshots for the post-incident review.

## Escalation

| Situation | Escalate to |
|---|---|
| p99 or error regression | Storage data-plane owner + SRE |
| Control loop not publishing | Control-plane owner |
| ABI / crash | Systems engineer on call |

## Post-rollout

- Record the final configuration and dates in the experiment log.
- Update `docs/design/01-whack-a-mole.md` if the rollout gates changed.
