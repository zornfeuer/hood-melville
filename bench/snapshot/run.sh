#!/usr/bin/env bash
# Опыты по разнице C и Rust в snapshot (docs/benchmarks.md, «Разница C и Rust
# в snapshot»). Собирает в build/snapshot: микротест rd (C, C с порядком
# «освободить, потом выделить» — order.patch, Rust) и подставные аллокаторы.
#   bench/snapshot/run.sh           # сборка
#   DYLD_INSERT_LIBRARIES=build/snapshot/lifo.dylib build/snapshot/rd-c 4
#   DYLD_INSERT_LIBRARIES=build/snapshot/lifo.dylib build/par/C snapshot 4
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/snapshot
mkdir -p "$out/order"
for a in lifo quarantine bump split pad128 owner; do
    cc -O2 -dynamiclib -o "$out/$a.dylib" "bench/snapshot/$a.c"
done
cp -R c/include c/src "$out/order/"
patch -s -d "$out/order" -p0 < bench/snapshot/order.patch
cc -O3 -march=native -flto -DNDEBUG -Ic/include bench/snapshot/rd.c c/src/fqueue.c -o "$out/rd-c"
cc -O3 -march=native -flto -DNDEBUG -DFQ_NOPOOL -DFQ_FREE_FIRST -I"$out/order/include" \
    bench/snapshot/rd.c "$out/order/src/fqueue.c" -o "$out/rd-c-order"
cc -O3 -march=native -flto -DNDEBUG -DFQ_NOPOOL -DFQ_FREE_FIRST -I"$out/order/include" \
    c/bench/fqueue_par.c "$out/order/src/fqueue.c" -o "$out/par-c-order"
cp bench/snapshot/rd.rs rust/examples/zz_snapshot_rd.rs
trap 'rm -f rust/examples/zz_snapshot_rd.rs' EXIT
(cd rust && RUSTFLAGS="-C target-cpu=native" cargo build -q --profile max --example zz_snapshot_rd)
cp rust/target/max/examples/zz_snapshot_rd "$out/rd-rust"
