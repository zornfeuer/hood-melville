//! Та же нагрузка, что `fqueue_bench`, но с `SyncQueue` (атомарные счётчики
//! ссылок): цена потокобезопасности в одном потоке. Сценарии и контрольные
//! суммы те же; использование — как у `fqueue_bench`.

type Q = hood_melville::SyncQueue<i32>;

include!("shared/bench.rs");
