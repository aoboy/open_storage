# Python/CFFI wheel

Plan prompt W6.16 packages the Python control plane and its compiled CFFI
binding as one installable artifact. An installed consumer no longer needs the
source checkout, `PYTHONPATH`, or `SDF_CFFI_DIR`.

## Build

Prerequisites:

- CMake >= 3.24 and a C17 compiler for the staged install prefix.
- Python >= 3.10 with `pip`, `setuptools`, `wheel`, and `cffi`.
- `cargo` only for `--variant ON` (the opt-in Rust LT fountain arm). The
  nanorq arm stays C in both variants.

From `storage-dataflow/`:

```bash
./scripts/build_wheel.sh                              # OFF, build/wheel
./scripts/build_wheel.sh --out /tmp/wheels --variant OFF
./scripts/build_wheel.sh --prefix /opt/sdf --variant ON
make wheel
```

Progress goes to stderr; the last stdout line is the wheel path. Without
`--prefix` the script configures, builds, and installs a temporary Release
prefix, builds `_sdf_cffi` against that installed prefix, and assembles a
platform wheel. `--prefix` reuses an existing installed sdf prefix instead of
rebuilding it, but the prefix must match the requested variant.

## Contents

The wheel contains:

- the `sdf` Python package, including `py.typed` and the CLI entry point;
- `sdf_cffi.py`, the compatibility loader;
- `sdf/_cffi.py`, which resolves the extension in this order:
  `SDF_CFFI_DIR`, in-tree `build/cffi`, the wheel-bundled
  `sdf/_sdf_cffi*.so`, then a top-level `_sdf_cffi`;
- the bundled `_sdf_cffi` extension linked statically against the sdf
  archives and vendored nanorq.

The wheel is tagged `cpXY-cpXY-<platform>` (for example
`cp314-cp314-linux_x86_64`) because the extension follows the interpreter ABI
and machine architecture. Build one wheel per supported platform; it is not a
`py3-none-any` artifact. The extension must not carry an RPATH/RUNPATH or a
path-like `NEEDED` entry pointing into the checkout, staging prefix, or build
tree; `scripts/test_wheel.sh` checks both with `readelf`.

## Install and test

```bash
python3 -m venv /tmp/sdf-wheel-venv
/tmp/sdf-wheel-venv/bin/pip install sdf-1.0.0-cp314-cp314-linux_x86_64.whl
/tmp/sdf-wheel-venv/bin/sdf --version
```

`make wheel-test` / `./scripts/test_wheel.sh` builds the wheel, inspects its
contents and dynamic dependencies, installs it into a fresh venv outside the
checkout with `PYTHONPATH` and `SDF_CFFI_DIR` unset, and executes the ARC
sidecar plus LT and nanorq fountain roundtrips. It also confirms that the
development overrides still bind the in-tree extension only when supplied.

This runner is offline, so the isolation test creates the venv with
`--system-site-packages` and installs the wheel with `--no-deps`. That verifies
the wheel payload and runtime behavior without downloading anything; it is not
evidence of dependency resolution or a hosted install. The authoritative
hosted legs depend on W6.11. The `--variant ON` leg is skipped when `cargo` is
unavailable, and no Rust-ON wheel evidence is claimed from such a run.

## Variants and native libraries

`OFF` (default) bundles the C LT fountain arm plus the C nanorq arm. `ON`
replaces only the LT arm with the Rust static library and retains nanorq; the
same folded `libsdf_fountain.a` surface and public C ABI are used. No external
sdf shared library is required at runtime, and the wheel declares no native
system package.

Runtime Python dependencies are declared in the generated project metadata:
`cffi`, `prometheus-client`, `pyyaml`, and `xxhash`. Building does not resolve
them; a normal online `pip install` does.
