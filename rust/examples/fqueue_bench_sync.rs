//! Та же нагрузка, что `fqueue_bench`, но с `SyncQueue` (атомарные счётчики
//! ссылок): цена потокобезопасности в одном потоке. Сценарии и контрольные
//! суммы те же; использование — как у `fqueue_bench`.

// С `--features pool` — то же на очереди с пулом узлов (`docs/pool.md`).
#[cfg(not(feature = "pool"))]
type Q = hood_melville::SyncQueue<i32>;
#[cfg(feature = "pool")]
type Q = hood_melville::SyncPooledQueue<i32>;

include!("shared/bench.rs");
