# Packaging placeholder (helm)

Still a placeholder as of v1.0.0 (plan prompt W6.6). The Debian package
is the one real distribution artifact (`packaging/debian`,
`scripts/build_deb.sh`, `scripts/test_deb.sh`).

The chart for the storage dataflow control plane has not been created
yet. When added, it should deploy only the Python control plane; the
data-plane sidecars are injected by the storage platform, not by this
chart. See docs/design/00-overview.md section 8.
