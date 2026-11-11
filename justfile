default: test

# --- C -----------------------------------------------------------------------

c-build:
    cmake -S c -B build/c > /dev/null
    cmake --build build/c

c-test: c-build
    ./build/c/fqueue_test --skip-benchmarks
    ./build/c/fqueue_limits_test

# C-тесты под AddressSanitizer и UBSan.
c-asan:
    cmake -S c -B build/c-asan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" > /dev/null
    cmake --build build/c-asan --target fqueue_test fqueue_limits_test
    ./build/c-asan/fqueue_test --skip-benchmarks
    ./build/c-asan/fqueue_limits_test

# Утечки в C-тестах на macOS, где LeakSanitizer недоступен (на Linux его
# включает c-asan).
c-leaks: c-build
    leaks --atExit -- ./build/c/fqueue_test --skip-benchmarks
    leaks --atExit -- ./build/c/fqueue_limits_test

# Установка C-библиотеки (разделяемой) и сборка примера через find_package.
c-install-check:
    cmake -S c -B build/c-pkg -DBUILD_SHARED_LIBS=ON -DHOOD_MELVILLE_BUILD_TESTS=OFF \
        -DHOOD_MELVILLE_BUILD_BENCH=OFF -DHOOD_MELVILLE_INSTALL=ON > /dev/null
    cmake --build build/c-pkg
    cmake --install build/c-pkg --prefix "$PWD/build/c-prefix" > /dev/null
    cmake -S c/examples/consumer -B build/consumer -DCMAKE_PREFIX_PATH="$PWD/build/c-prefix" > /dev/null
    cmake --build build/consumer
    ./build/consumer/consumer

# --- Rust --------------------------------------------------------------------

rust-test:
    cd rust && cargo test

rust-lint:
    cd rust && cargo fmt --check
    cd rust && cargo clippy --all-targets -- -W clippy::pedantic -D warnings

# Тесты под Miri: проверка unsafe в rust/src/node.rs на UB и утечки
# (Stacked Borrows для всех тестов, Tree Borrows — для юнит-тестов).
rust-miri:
    cd rust && cargo +nightly miri test --lib --test scenarios --test model --test branching
    cd rust && MIRIFLAGS="-Zmiri-tree-borrows -Zmiri-strict-provenance" cargo +nightly miri test --lib

# --- Zig 0.16 ----------------------------------------------------------------

zig-test:
    cd zig && zig build --global-cache-dir .zig-cache-global test

zig-lint:
    zig fmt --check zig/

zig-build:
    cd zig && zig build --global-cache-dir .zig-cache-global -Doptimize=ReleaseFast -Dcpu=native

test: c-test rust-test zig-test

# --- Сравнение C, Rust и Zig (методика — docs/benchmarks.md) ----------------------

# Нагрузки трёх реализаций с максимальной оптимизацией (O3/LTO/native):
# build/c/fqueue_bench, rust/target/max/examples/fqueue_bench, zig/zig-out/bin/fqueue_bench.
bench-build: zig-build
    cmake -S c -B build/c > /dev/null
    cmake --build build/c --target fqueue_bench
    cd rust && RUSTFLAGS="-C target-cpu=native" cargo build --profile max --example fqueue_bench

# Сценарии и размеры по умолчанию для сравнений.
bench_cases := "--case fill:1000000 --case mixed:1000000 --case persistent:1000000 --case branching:1000000 --case fill:10000000 --case mixed:10000000"
# Отдельный бенчмарк операций на месте (fill_mut, mixed_mut) рядом с теми же
# историями через персистентные операции.
bench_cases_mut := "--case fill:1000000 --case fill_mut:1000000 --case mixed:1000000 --case mixed_mut:1000000 --case fill:10000000 --case fill_mut:10000000 --case mixed:10000000 --case mixed_mut:10000000"

# Варианты раскладки кода (выравнивание функций и циклов, байт). Склонность к
# медленному режиму аллокатора зависит от раскладки сборки, поэтому реализации
# сравниваются по нескольким раскладкам, одинаково для C и Rust.
bench_layouts := "16 32 64 128"

# Быстрое сравнение текущих сборок (одна раскладка).
bench-compare rounds="20": bench-build
    python3 -I bench/compare.py --rounds {{rounds}} --out bench-results/compare {{bench_cases}} \
        C=build/c/fqueue_bench Rust=rust/target/max/examples/fqueue_bench Zig=zig/zig-out/bin/fqueue_bench

# Собрать варианты раскладки текущих исходников: build/bench-layout/<label>/{C,Rust,Zig}-al<N>.
bench-layouts label="cur":
    #!/usr/bin/env bash
    set -euo pipefail
    dir=build/bench-layout/{{label}}
    mkdir -p "$dir"
    (cd zig && zig build --global-cache-dir .zig-cache-global -Doptimize=ReleaseFast -Dcpu=native)
    # Zig 0.16 не предоставляет CLI-опций выравнивания всех функций/циклов.
    # До появления таких опций эти четыре имени обозначают одну сборку.
    for a in {{bench_layouts}}; do
        cp zig/zig-out/bin/fqueue_bench "$dir/Zig-al$a"
        cc -O3 -march=native -flto -DNDEBUG -Ic/include -falign-functions=$a -falign-loops=$a \
            c/bench/fqueue_bench.c c/src/fqueue.c -o "$dir/C-al$a"
        log2=0; x=$a; while (( x > 1 )); do x=$(( x / 2 )); log2=$(( log2 + 1 )); done
        (cd rust && RUSTFLAGS="-C target-cpu=native -C llvm-args=-align-all-functions=$log2 -C llvm-args=-align-loops=$a" \
            cargo build -q --profile max --example fqueue_bench --target-dir target/layout-al$a)
        cp rust/target/layout-al$a/max/examples/fqueue_bench "$dir/Rust-al$a"
    done
    echo "раскладки: $dir"

# Сравнение C, Rust и Zig по всем вариантам раскладки — основной режим для выводов.
bench-compare-layouts rounds="8": (bench-layouts "cur")
    #!/usr/bin/env bash
    set -euo pipefail
    bins=()
    for impl in C Rust Zig; do
        for a in {{bench_layouts}}; do bins+=("$impl/al$a=build/bench-layout/cur/$impl-al$a"); done
    done
    python3 -I bench/compare.py --settle 120 --rounds {{rounds}} --out bench-results/compare-layouts \
        {{bench_cases}} "${bins[@]}"

# C, Rust и Zig на операциях на месте, по 4 раскладкам.
bench-compare-mut rounds="8": (bench-layouts "cur")
    #!/usr/bin/env bash
    set -euo pipefail
    bins=()
    for impl in C Rust Zig; do
        for a in {{bench_layouts}}; do bins+=("$impl/al$a=build/bench-layout/cur/$impl-al$a"); done
    done
    python3 -I bench/compare.py --settle 120 --rounds {{rounds}} --out bench-results/compare-mut \
        {{bench_cases_mut}} "${bins[@]}"

# A/B двух наборов раскладок одной реализации (собранных bench-layouts <label>):
# just bench-ab-layouts ctl exp Rust [rounds].
bench-ab-layouts a b impl rounds="8":
    #!/usr/bin/env bash
    set -euo pipefail
    bins=()
    for label in {{a}} {{b}}; do
        for al in {{bench_layouts}}; do bins+=("$label/al$al=build/bench-layout/$label/{{impl}}-al$al"); done
    done
    python3 -I bench/compare.py --settle 120 --rounds {{rounds}} --out bench-results/ab-{{a}}-{{b}}-{{impl}} \
        {{bench_cases}} "${bins[@]}"
