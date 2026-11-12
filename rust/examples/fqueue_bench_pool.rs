//! Та же нагрузка, что `fqueue_bench`, но с `PooledQueue` (узлы из пула,
//! `docs/pool.md`). Сценарии, контрольные суммы и использование — как у
//! `fqueue_bench`.

type Q = hood_melville::PooledQueue<i32>;

include!("shared/bench.rs");
