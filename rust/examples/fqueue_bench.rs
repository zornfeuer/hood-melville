//! Нагрузка для сравнения с C-реализацией (`just bench-compare`).
//! C-двойник: `c/bench/fqueue_bench.c` — сценарии и контрольная сумма должны
//! совпадать один в один.
//!
//! Использование:
//! `fqueue_bench <fill|mixed|persistent|branching|fill_mut|mixed_mut> <N>`.
//! `fill_mut`, `mixed_mut` — те же истории через `push_mut`/`pop_mut`
//! (отдельный бенчмарк, `just bench-compare-mut`).
//! Печатает контрольную сумму извлечённых значений (зависит от порядка).
//! С `FQ_PHASES=1` печатает в stderr длительность фаз (build/drain/destroy).

// С `--features pool` — то же на очереди с пулом узлов (`docs/pool.md`).
#[cfg(not(feature = "pool"))]
type Q = hood_melville::Queue<i32>;
#[cfg(feature = "pool")]
type Q = hood_melville::PooledQueue<i32>;

include!("shared/bench.rs");
