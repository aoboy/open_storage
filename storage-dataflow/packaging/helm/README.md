# Helm packaging

The control-plane chart lives in [`sdf/`](sdf/). It deploys only the Python
control plane and mounts reviewed `wm.toml`, `fluid.toml`, and `arc.toml`
configuration from an existing ConfigMap.

```sh
HELM=/path/to/helm make helm
```

The local gate runs `helm lint --strict`, renders default plus dev/staging/prod values, and schema-validates them with kubeconform when available. It is not cluster deployment evidence; the chart
still needs an available kind/cluster plus a real image and environment
ConfigMap before readiness, rollback, or control-operation claims are made.

RPM packaging and a named target distro remain separate W6.17 work.
