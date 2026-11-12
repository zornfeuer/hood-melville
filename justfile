default: test

# --- C -----------------------------------------------------------------------

c-build:
    cmake -S c -B build/c > /dev/null
    cmake --build build/c

c-test: c-build
    ./build/c/fqueue_test --skip-benchmarks
    ./build/c/fqueue_limits_test

# Многопоточные C-тесты (fqueue_sync) под ThreadSanitizer.
c-tsan:
    cmake -S c -B build/c-tsan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=thread" -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" > /dev/null
    cmake --build build/c-tsan --target fqueue_test
    ./build/c-tsan/fqueue_test "[sync]"

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
    cd rust && cargo +nightly miri test --lib --test scenarios --test model --test branching --test sync
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
    cmake --build build/c --target fqueue_bench fqueue_bench_sync
    cd rust && RUSTFLAGS="-C target-cpu=native" cargo build --profile max --example fqueue_bench --example fqueue_bench_sync

# Сценарии и размеры по умолчанию для сравнений.
bench_cases := "--case fill:1000000 --case mixed:1000000 --case persistent:1000000 --case branching:1000000 --case fill:10000000 --case mixed:10000000"
# Отдельный бенчмарк операций на месте (fill_mut, mixed_mut) рядом с теми же
# историями через персистентные операции.
bench_cases_mut := "--case fill:1000000 --case fill_mut:1000000 --case mixed:1000000 --case mixed_mut:1000000 --case fill:10000000 --case fill_mut:10000000 --case mixed:10000000 --case mixed_mut:10000000"
# Сетка аллокаторов (#8): персистентные и на месте, без повторов.
bench_cases_alloc := bench_cases + " --case fill_mut:1000000 --case mixed_mut:1000000 --case fill_mut:10000000 --case mixed_mut:10000000"

# Варианты раскладки кода (выравнивание функций и циклов, байт). Склонность к
# медленному режиму аллокатора зависит от раскладки сборки, поэтому реализации
# сравниваются по нескольким раскладкам, одинаково для C, Rust и Zig.
bench_layouts := "16 32 64 128"

# Быстрое сравнение текущих сборок (одна раскладка).
bench-compare rounds="20": bench-build
    python3 -I bench/compare.py --rounds {{rounds}} --out bench-results/compare {{bench_cases}} \
        C=build/c/fqueue_bench Rust=rust/target/max/examples/fqueue_bench Zig=zig/zig-out/bin/fqueue_bench

# Zig-нагрузка как LLVM bitcode (sync: true/false): в Zig 0.16 нет опций
# выравнивания функций и циклов, поэтому раскладки собирает clang из bitcode
# (-falign-functions/-falign-loops, как у C). Скорость сборки через clang
# совпадает с родной zig build (persistent 10^6: 515–525 млн тактов у обеих).
zig-layout-bc sync out:
    #!/usr/bin/env bash
    set -euo pipefail
    out=$(cd "$(dirname "{{out}}")" && pwd)/$(basename "{{out}}")
    echo "pub const sync = {{sync}};" > "$out.options.zig"
    cd zig && zig build-exe -O ReleaseFast -mcpu native -lc --global-cache-dir .zig-cache-global \
        --dep fqueue --dep bench_options -Mroot=bench/fqueue_bench.zig -Mfqueue=src/root.zig \
        -Mbench_options="$out.options.zig" -femit-llvm-bc="$out" -fno-emit-bin

# Собрать варианты раскладки текущих исходников: build/bench-layout/<label>/{C,Rust,Zig}-al<N>.
bench-layouts label="cur":
    #!/usr/bin/env bash
    set -euo pipefail
    dir=build/bench-layout/{{label}}
    mkdir -p "$dir"
    just zig-layout-bc false "$dir/zig.bc"
    for a in {{bench_layouts}}; do
        clang -O3 -mcpu=native -falign-functions=$a -falign-loops=$a "$dir/zig.bc" -o "$dir/Zig-al$a" 2>/dev/null
        cc -O3 -march=native -flto -DNDEBUG -Ic/include -falign-functions=$a -falign-loops=$a \
            c/bench/fqueue_bench.c c/src/fqueue.c -o "$dir/C-al$a"
        log2=0; x=$a; while (( x > 1 )); do x=$(( x / 2 )); log2=$(( log2 + 1 )); done
        (cd rust && RUSTFLAGS="-C target-cpu=native -C llvm-args=-align-all-functions=$log2 -C llvm-args=-align-loops=$a" \
            cargo build -q --profile max --example fqueue_bench --target-dir target/layout-al$a)
        cp rust/target/layout-al$a/max/examples/fqueue_bench "$dir/Rust-al$a"
    done
    echo "раскладки: $dir"

# Потокобезопасные варианты (fqueue_sync, SyncQueue, SyncQueue(T)) тех же
# исходников: build/bench-layout/<label>/{C,Rust,Zig}-sync-al<N>.
bench-layouts-sync label="cur":
    #!/usr/bin/env bash
    set -euo pipefail
    dir=build/bench-layout/{{label}}
    mkdir -p "$dir"
    just zig-layout-bc true "$dir/zig-sync.bc"
    for a in {{bench_layouts}}; do
        clang -O3 -mcpu=native -falign-functions=$a -falign-loops=$a "$dir/zig-sync.bc" -o "$dir/Zig-sync-al$a" 2>/dev/null
        cc -O3 -march=native -flto -DNDEBUG -DFQUEUE_BENCH_SYNC -Ic/include -falign-functions=$a -falign-loops=$a \
            c/bench/fqueue_bench.c c/src/fqueue.c -o "$dir/C-sync-al$a"
        log2=0; x=$a; while (( x > 1 )); do x=$(( x / 2 )); log2=$(( log2 + 1 )); done
        (cd rust && RUSTFLAGS="-C target-cpu=native -C llvm-args=-align-all-functions=$log2 -C llvm-args=-align-loops=$a" \
            cargo build -q --profile max --example fqueue_bench_sync --target-dir target/layout-al$a)
        cp rust/target/layout-al$a/max/examples/fqueue_bench_sync "$dir/Rust-sync-al$a"
    done
    echo "раскладки: $dir"

# Цена потокобезопасности в одном потоке: каждая реализация рядом со своим
# вариантом на атомарных счётчиках, по 4 раскладкам.
bench-compare-sync rounds="8": (bench-layouts "cur") (bench-layouts-sync "cur")
    #!/usr/bin/env bash
    set -euo pipefail
    bins=()
    for impl in C C-sync Rust Rust-sync Zig Zig-sync; do
        for a in {{bench_layouts}}; do bins+=("$impl/al$a=build/bench-layout/cur/$impl-al$a"); done
    done
    python3 -I bench/compare.py --settle 120 --rounds {{rounds}} --out bench-results/compare-sync \
        {{bench_cases}} "${bins[@]}"

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

# --- Параллельная нагрузка (#6) -----------------------------------------------

# Сборки fqueue_par: build/par/{C,Rust,Zig}. Режим pool в C — OpenMP из
# libomp (brew install libomp); без него C собирается только с режимом threads.
bench-par-build: zig-build
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p build/par
    omp=()
    if prefix=$(brew --prefix libomp 2>/dev/null) && [ -f "$prefix/include/omp.h" ]; then
        omp=(-Xclang -fopenmp -I"$prefix/include" -L"$prefix/lib" -lomp)
    else
        echo "libomp не найден: C без режима pool (brew install libomp)" >&2
    fi
    cc -O3 -march=native -flto -DNDEBUG -Ic/include ${omp[@]+"${omp[@]}"} \
        c/bench/fqueue_par.c c/src/fqueue.c -o build/par/C
    (cd rust && RUSTFLAGS="-C target-cpu=native" cargo build -q --profile max --example fqueue_par)
    cp rust/target/max/examples/fqueue_par build/par/Rust
    cp zig/zig-out/bin/fqueue_par build/par/Zig

# Масштабирование по числу потоков и доле операций над общими версиями.
bench-par rounds="5" modes="pool,threads": bench-par-build
    python3 -I bench/parallel.py --settle 60 --rounds {{rounds}} --modes {{modes}} \
        --out bench-results/parallel C=build/par/C Rust=build/par/Rust Zig=build/par/Zig

# --- Аллокаторы (#8) ----------------------------------------------------------

# Системный malloc, mimalloc и jemalloc (brew install mimalloc jemalloc) у всех
# трёх реализаций: те же бинарники, аллокатор подставляется через
# DYLD_INSERT_LIBRARIES. mimalloc перехватывает вызовы сам; jemalloc — через
# прокладку bench/alloc/jemalloc_interpose.c (без неё free идёт через поиск
# зоны libmalloc, docs/benchmarks.md).

# Прокладка jemalloc: build/alloc/jemalloc_interpose.dylib.
alloc-build:
    #!/usr/bin/env bash
    set -euo pipefail
    jp=$(brew --prefix jemalloc)
    mkdir -p build/alloc
    cc -O2 -dynamiclib -I"$jp/include" -DJEMALLOC_DYLIB="\"$jp/lib/libjemalloc.2.dylib\"" \
        -o build/alloc/jemalloc_interpose.dylib bench/alloc/jemalloc_interpose.c \
        -lSystem -L"$jp/lib" -ljemalloc 2>&1 | grep -v 'duplicate libraries' || true

# Однопоточная сетка «язык × аллокатор» по 4 раскладкам; сводка — alloc_report.py.
bench-alloc rounds="6": alloc-build (bench-layouts "alloc")
    #!/usr/bin/env bash
    set -euo pipefail
    mi=$(brew --prefix mimalloc)/lib/libmimalloc.dylib
    je=$PWD/build/alloc/jemalloc_interpose.dylib
    bins=(); envs=()
    for impl in C Rust Zig; do
        for al in sys mi je; do
            for a in {{bench_layouts}}; do
                n="$impl-$al/al$a"
                bins+=("$n=build/bench-layout/alloc/$impl-al$a")
                case $al in
                    mi) envs+=(--env "$n:DYLD_INSERT_LIBRARIES=$mi") ;;
                    je) envs+=(--env "$n:DYLD_INSERT_LIBRARIES=$je") ;;
                esac
            done
        done
    done
    python3 -I bench/compare.py --settle 60 --rounds {{rounds}} --out bench-results/alloc-single \
        "${envs[@]}" {{bench_cases_alloc}} "${bins[@]}"
    python3 -I bench/alloc_report.py bench-results/alloc-single/compare.json

# Параллельная нагрузка с тремя аллокаторами (режим pool и snapshot).
bench-alloc-par rounds="5": alloc-build bench-par-build
    #!/usr/bin/env bash
    set -euo pipefail
    mi=$(brew --prefix mimalloc)/lib/libmimalloc.dylib
    je=$PWD/build/alloc/jemalloc_interpose.dylib
    bins=(); envs=()
    for impl in C Rust Zig; do
        for al in sys mi je; do
            bins+=("$impl-$al=build/par/$impl")
            case $al in
                mi) envs+=(--env "$impl-$al:DYLD_INSERT_LIBRARIES=$mi") ;;
                je) envs+=(--env "$impl-$al:DYLD_INSERT_LIBRARIES=$je") ;;
            esac
        done
    done
    python3 -I bench/parallel.py --settle 60 --rounds {{rounds}} --modes pool \
        --shares 0,1,5,50 --threads 1,4,10 --readers 0,1,4,9 \
        --out bench-results/alloc-par "${envs[@]}" "${bins[@]}"

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
