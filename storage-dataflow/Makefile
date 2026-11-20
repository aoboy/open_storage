.PHONY: all build test bench format lint doc clean asan tsan ci install deb wheel wheel-test rust-seam rust-fountain

# Gates run serially (plan W0.5): a gate that races another gate is not
# a gate.
.NOTPARALLEL:

all: build

build:
	./scripts/build.sh

test:
	./scripts/run_tests.sh

bench:
	./scripts/run_bench.sh

format:
	./scripts/format_c.sh --write

lint:
	./scripts/lint.sh

doc:
	./scripts/build_doc.sh

# Sanitizer gates (plan W0.5): separate build trees, full suite each.
asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" >/dev/null
	cmake --build build-asan --parallel
	ctest --test-dir build-asan --output-on-failure

tsan:
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" >/dev/null
	cmake --build build-tsan --parallel
	ctest --test-dir build-tsan --output-on-failure

# Local CI gate (plan W0.5, mirroring the wack_a_mole `make ci` shape):
# format/tidy where installed (lint.sh skips cleanly), build + full
# suite, then both sanitizer legs.
ci: lint test asan tsan install deb wheel-test rust-seam rust-fountain
	@echo "ci gate green: lint(+skips) test asan tsan install deb wheel-test rust-seam rust-fountain(+skips)"

# Plan W5.3 — the Rust seam leg (advisory without cargo; see the script).
rust-seam:
	./scripts/test_rust_seam.sh

# Plan W6.8 — functional Rust: C/Rust full-suite A/B, Rust ASan+UBSan,
# installed CFFI/pkg-config/find_package and the Debian package surface.
rust-fountain:
	./scripts/test_rust_fountain.sh

# Plan W5.2 — the install leg (DESTDIR-staged pkg-config + find_package).
install:
	./scripts/test_install.sh

# Plan W6.6 — the Debian package leg (build + dpkg-install + consumer).
deb:
	./scripts/test_deb.sh

# Plan W6.16 — the installable Python/CFFI wheel and its isolated venv leg.
wheel:
	./scripts/build_wheel.sh

wheel-test:
	./scripts/test_wheel.sh

clean:
	rm -rf build build-asan build-tsan out
