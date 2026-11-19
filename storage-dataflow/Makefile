.PHONY: all build test bench lint doc clean asan tsan ci

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
ci: lint test asan tsan install deb rust-seam
	@echo "ci gate green: lint(+skips) test asan tsan install deb rust-seam(+skips)"

# Plan W5.3 — the Rust seam leg (advisory without cargo; see the script).
rust-seam:
	./scripts/test_rust_seam.sh

# Plan W5.2 — the install leg (DESTDIR-staged pkg-config + find_package).
install:
	./scripts/test_install.sh

# Plan W6.6 — the Debian package leg (build + dpkg-install + consumer).
deb:
	./scripts/test_deb.sh

clean:
	rm -rf build build-asan build-tsan out
