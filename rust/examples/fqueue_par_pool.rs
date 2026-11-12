//! Та же параллельная нагрузка, что `fqueue_par`, но с `SyncPooledQueue`
//! (узлы из пула, `docs/pool.md`). Сценарии, контрольные суммы и
//! использование — как у `fqueue_par`.

type Q = hood_melville::SyncPooledQueue<i32>;

include!("shared/par.rs");
