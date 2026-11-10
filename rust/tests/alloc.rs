//! Детерминированное доказательство O(1): число и объём аллокаций каждой
//! отдельной операции ограничены константой и не растут с размером очереди.
//!
//! В отличие от замеров времени, результат не зависит от кэшей и шума.
//! Каждая операция выполняется над версией, которая остаётся живой, — так
//! все узлы разделяемые и ротация вынуждена клонировать значения (худший
//! случай). Отдельный бинарник без тестового раннера (`harness = false`):
//! dhat считает аллокации всего процесса, и потоки раннера вносили бы шум.

use std::rc::Rc;

use hood_melville::Queue;

#[global_allocator]
static ALLOC: dhat::Alloc = dhat::Alloc;

/// В debug-сборке dhat заметно медленнее — там хватает 10^4.
const MAX_N: usize = if cfg!(debug_assertions) {
    10_000
} else {
    100_000
};

/// Корзины по числу цифр в размере очереди: 0, 1–9, 10–99, ...; размер
/// ровно `MAX_N` попадает в последнюю.
const BUCKETS: usize = MAX_N.ilog10() as usize + 1;

/// С этой корзины (размер ≥ 100) все фазы ротации уже встречаются, и
/// худший случай должен быть одинаковым во всех следующих корзинах.
const FLAT_FROM: usize = 3;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
struct Cost {
    blocks: u64,
    bytes: u64,
}

impl Cost {
    fn raise(&mut self, other: Self) {
        self.blocks = self.blocks.max(other.blocks);
        self.bytes = self.bytes.max(other.bytes);
    }
}

fn measure<R>(op: impl FnOnce() -> R) -> (R, Cost) {
    let before = dhat::HeapStats::get();
    let result = op();
    let after = dhat::HeapStats::get();
    let cost = Cost {
        blocks: after.total_blocks - before.total_blocks,
        bytes: after.total_bytes - before.total_bytes,
    };
    (result, cost)
}

fn bucket(len: usize) -> usize {
    len.checked_ilog10()
        .map_or(0, |d| d as usize + 1)
        .min(BUCKETS - 1)
}

fn bucket_label(b: usize) -> String {
    match b {
        0 => "0".into(),
        b => format!("10^{}..", b - 1),
    }
}

/// Узлы, создаваемые операцией: push — узел `rear` + до 2 узлов на каждый
/// шаг ротации; pop — только шаги ротации (теорема 2 в
/// `docs/proof.md`).
const STEPS: u64 = 1; // шагов ротации на операцию (`ROTATION_STEPS` в queue.rs)
const PUSH_NODES: u64 = 1 + 2 * STEPS;
const POP_NODES: u64 = 2 * STEPS;

struct Spec<T> {
    name: &'static str,
    make: fn(usize) -> T,
    /// Аллокаций на один `clone()` значения.
    clone_blocks: u64,
}

fn profile<T: Clone>(spec: &Spec<T>) {
    let baseline = dhat::HeapStats::get().curr_blocks;
    let mut push = [Cost::default(); BUCKETS];
    let mut pop = [Cost::default(); BUCKETS];

    {
        let mut q = Queue::new();
        let mut i = 0;
        let mut do_push = |q: &mut Queue<T>| {
            let value = (spec.make)(i);
            i += 1;
            let (next, cost) = measure(|| q.push(value));
            push[bucket(q.len())].raise(cost);
            *q = next;
        };
        let mut do_pop = |q: &mut Queue<T>| {
            let (next, cost) = measure(|| q.pop().expect("очередь не пуста"));
            pop[bucket(q.len())].raise(cost);
            *q = next;
        };

        // Рост по схеме push, push, pop: ротации запускаются и завершаются
        // на фоне обоих видов операций.
        while q.len() < MAX_N {
            do_push(&mut q);
            do_push(&mut q);
            do_pop(&mut q);
        }
        // Опустошение: ротации под одними pop'ами.
        while !q.is_empty() {
            do_pop(&mut q);
        }
    }
    // До печати: буфер stdout создаётся лениво и живёт до конца процесса.
    let alive = dhat::HeapStats::get().curr_blocks;
    assert_eq!(
        alive, baseline,
        "{}: после удаления всех версий остались блоки",
        spec.name
    );

    println!("\n{} (size_of = {} Б)", spec.name, size_of::<T>());
    println!(
        "  {:<10} {:>18} {:>18}",
        "размер", "push: блоки / Б", "pop: блоки / Б"
    );
    for b in 0..BUCKETS {
        let fmt = |c: Cost| format!("{} / {}", c.blocks, c.bytes);
        println!(
            "  {:<10} {:>18} {:>18}",
            bucket_label(b),
            fmt(push[b]),
            fmt(pop[b])
        );
    }

    // Каждый узел, созданный ротацией, несёт клон значения.
    let push_bound = PUSH_NODES + (PUSH_NODES - 1) * spec.clone_blocks;
    let pop_bound = POP_NODES * (1 + spec.clone_blocks);
    for b in 0..BUCKETS {
        assert!(
            push[b].blocks <= push_bound,
            "{}: push {:?} > {push_bound}",
            spec.name,
            push[b]
        );
        assert!(
            pop[b].blocks <= pop_bound,
            "{}: pop {:?} > {pop_bound}",
            spec.name,
            pop[b]
        );
    }
    // Оценка точная: худший случай достигается.
    assert_eq!(push[FLAT_FROM].blocks, push_bound, "{}: push", spec.name);
    assert_eq!(pop[FLAT_FROM].blocks, pop_bound, "{}: pop", spec.name);
    for b in FLAT_FROM..BUCKETS {
        assert_eq!(push[b], push[FLAT_FROM], "{}: push растёт с N", spec.name);
        assert_eq!(pop[b], pop[FLAT_FROM], "{}: pop растёт с N", spec.name);
    }
}

/// Крупное значение без аллокаций внутри: узел в 10 раз больше, чем для `i32`.
#[derive(Clone)]
struct Big(#[allow(dead_code, reason = "нужен только размер")] [u64; 32]);

fn main() {
    let _profiler = dhat::Profiler::builder().testing().build();

    let ((), idle) = measure(|| ());
    assert_eq!(idle, Cost::default(), "сам замер не должен аллоцировать");

    profile(&Spec {
        name: "i32",
        make: |i| i32::try_from(i).unwrap(),
        clone_blocks: 0,
    });
    profile(&Spec {
        name: "String (32 символа)",
        make: |i| format!("{i:032}"),
        clone_blocks: 1,
    });
    profile(&Spec {
        name: "[u64; 32]",
        make: |i| Big([i as u64; 32]),
        clone_blocks: 0,
    });
    profile(&Spec {
        name: "Rc<str> (32 символа)",
        make: |i| Rc::<str>::from(format!("{i:032}")),
        clone_blocks: 0,
    });

    println!("\nаллокации на операцию ограничены и не зависят от N — ok");
}
