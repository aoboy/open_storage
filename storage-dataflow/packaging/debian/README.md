# Debian packaging (plan prompt W6.6)

`control` is the package manifest. `@SDF_VERSION@` and `@DEB_ARCH@` are
substituted by `scripts/build_deb.sh` from `include/sdf/version.h` and
`dpkg --print-architecture`; do not hard-code a version here.

    ./scripts/build_deb.sh    # build/deb/libsdf-dev_<version>_<arch>.deb
    ./scripts/test_deb.sh     # build + dpkg --root install + consumer compile

The package is a development payload only: public headers, static
archives (including the vendored `libnanorq.a`), pkg-config files and
the CMake `find_package(sdf)` config. No shared library, no maintainer
scripts, no runtime dependencies. The install prefix is `/usr`; the test
leg installs into an isolated rootfs with `dpkg --root` and compiles
`tools/install_hello.c` against the payload via pkg-config's sysroot.

`rpm` and `helm` remain placeholders (see their READMEs).
