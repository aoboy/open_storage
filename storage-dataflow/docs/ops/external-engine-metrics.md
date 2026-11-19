# External engine metric input contract

The ARC sidecar does **not** own storage-engine apply latency or leader
backpressure. The ARC dashboard's two engine panels therefore require a separate
engine exporter in the same Prometheus job/federation namespace. Without that
input, the panels have no data; they are not evidence of a zero measurement.

## Required series

| Series | Type | Required labels | Unit/semantics |
|---|---|---|---|
| `engine_apply_p99_seconds` | gauge | `pipeline`, `mode` | Most recent engine apply p99 in seconds |
| `engine_backpressure_events_total` | counter | `pipeline`, `mode` | Monotonic cumulative leader backpressure events |

`pipeline` must come from the deployment's bounded configured pipeline set, not
from object IDs or other unbounded values. `mode` is exactly `bypass` (the killed
or pre-ARC baseline) or `arc` (the paced path). The same engine window and
workload must feed both modes before their values are compared. The exporter
owns scrape freshness and must stale/remove samples when the engine disappears.

`docs/ops/fixtures/engine.prom` is a checked-in parser/query fixture with both
modes. It is not production evidence. Local tests validate dashboard metric
references against this fixture and against C-generated live scrape bodies; no
local test performs or claims a hosted Grafana import.
