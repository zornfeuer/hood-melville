//! Та же нагрузка, что `fqueue_bench_sync`, но с `SyncPooledQueue` (узлы из
//! пула, `docs/pool.md`). Сценарии, контрольные суммы и использование — как
//! у `fqueue_bench`.

type Q = hood_melville::SyncPooledQueue<i32>;

include!("shared/bench.rs");
