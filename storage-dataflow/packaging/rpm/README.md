# Packaging placeholder (rpm)

Still a placeholder as of v1.0.0 (plan prompt W6.6). The Debian package
is the one real distribution artifact: `packaging/debian/control` +
`scripts/build_deb.sh` + the `scripts/test_deb.sh` leg in `ci`.

An rpm should be generated from the same staged DESTDIR install surface
(prefix `/usr`) once a target distro is named; see
docs/design/00-overview.md section 8.
