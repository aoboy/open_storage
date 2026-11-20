# sdf-control-plane Helm chart

Deploys only the Python control plane (`sdf serve`). Data-plane sidecars are
injected by the storage platform and are not part of this chart. S3/NVMe-peer
drivers are not included.

## Prerequisites

- A container image containing the installed `sdf` wheel and its `sdf`
  console entry point. The chart does not build or publish an image.
- A ConfigMap in the target namespace with `wm.toml`, `fluid.toml`, and
  `arc.toml` from a reviewed environment config. The chart mounts it; it does
  not generate or rewrite rollout percentages.
- Kubernetes with HTTP probes available.

## Install

```sh
kubectl -n sdf create configmap sdf-config-dev \
  --from-file=wm.toml=storage-dataflow/configs/dev/wm.toml \
  --from-file=fluid.toml=storage-dataflow/configs/dev/fluid.toml \
  --from-file=arc.toml=storage-dataflow/configs/dev/arc.toml

helm install sdf ./storage-dataflow/packaging/helm/sdf \
  -n sdf --create-namespace \
  -f ./storage-dataflow/packaging/helm/sdf/values-dev.yaml \
  --set image.repository=<your-registry>/sdf --set image.tag=<tag>
```

Use `values-staging.yaml` or `values-prod.yaml` for those environments. Set
`config.existingConfigMap` to the reviewed ConfigMap name in each case.

Rollout percentages live in the mounted TOML. Keep them at zero until the
signed gates permit promotion; this chart does not infer or override them.

## Probes and service

`sdf serve` listens on `health.port` and serves `/healthz` and `/readyz`.
The Deployment uses both as Kubernetes probes. The Service exposes the health
port; set `service.annotations` for scrape/ownership metadata as needed.

The runtime health file lives on an `emptyDir` at `/var/run/sdf/health.json`.
The container root filesystem is read-only, Linux capabilities are dropped,
and the pod runs as non-root.

## Upgrade and rollback

```sh
helm upgrade sdf ./storage-dataflow/packaging/helm/sdf -n sdf -f <values>
helm rollback sdf <revision> -n sdf
```

The Deployment uses `Recreate` by default so two control-plane writers never
run concurrently. Change `strategyType` only with a reviewed coordination
plan.

## Validation

```sh
HELM=/path/to/helm make helm
```

This runs `helm lint --strict` and renders default, dev, staging, and prod
values. It checks the Deployment, Service, control-plane container, both
probes, and the read-only root filesystem. Template success is not a cluster
deployment claim; an available kind/cluster is still required for readiness,
rollback, and real control-operation evidence.

## Boundaries

- The chart deploys no data-plane sidecar and no S3/NVMe-peer driver.
- RPM packaging and a named target distro remain separate W6.17 work.
- No production image registry, endpoint, or measured-optimal knob is baked
  into the defaults.
