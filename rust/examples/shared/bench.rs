// Тело нагрузки, общее для `fqueue_bench` (`Queue`), `fqueue_bench_sync`
// (`SyncQueue`) и их вариантов с пулом (`fqueue_bench_pool[_sync]`):
// подключается `include!`, тип очереди `Q` задаёт подключающий.

use std::process::ExitCode;
use std::time::Instant;


#[allow(clippy::cast_sign_loss, reason = "как `(uint64_t)x` в C")]
fn mix(h: u64, x: i32) -> u64 {
    h.wrapping_mul(31).wrapping_add(i64::from(x) as u64)
}

fn mix_front(h: u64, q: &Q) -> u64 {
    mix(h, *q.front().expect("очередь не пуста"))
}

/// Замер фаз; без `FQ_PHASES=1` — no-op.
struct Phases {
    on: bool,
    t0: Instant,
}

impl Phases {
    fn mark(&mut self, name: &str) {
        if self.on {
            let t = Instant::now();
            eprintln!("phase {name} {:.3} ms", (t - self.t0).as_secs_f64() * 1e3);
            self.t0 = t;
        }
    }
}

/// N push'ей, затем опустошение. Старая версия освобождается сразу.
fn run_fill(n: i32, ph: &mut Phases) -> u64 {
    let mut h = 0;
    let mut q = Q::default();
    for i in 0..n {
        q = q.push(i);
    }
    ph.mark("build");
    // Как в C: сначала голова, затем pop.
    while !q.is_empty() {
        h = mix_front(h, &q);
        q = q.pop().expect("очередь не пуста");
    }
    ph.mark("drain");
    h
}

/// push, push, pop до размера N, затем опустошение.
fn run_mixed(n: i32, ph: &mut Phases) -> u64 {
    let n = usize::try_from(n).unwrap();
    let (mut h, mut i) = (0, 0);
    let mut q = Q::default();
    while q.len() < n {
        // Два присваивания, как два `replace` в C: старая версия освобождается
        // сразу после каждой операции (цепочка `q.push(i).push(i + 1)` держала
        // бы промежуточную и старую версии живыми до конца выражения).
        q = q.push(i);
        q = q.push(i + 1);
        i += 2;
        h = mix_front(h, &q);
        q = q.pop().unwrap();
    }
    ph.mark("build");
    // Как в C: сначала голова, затем pop.
    while !q.is_empty() {
        h = mix_front(h, &q);
        q = q.pop().expect("очередь не пуста");
    }
    ph.mark("drain");
    h
}

/// fill и mixed через операции на месте: одна версия, без копий и
/// освобождения старой. Контрольные суммы совпадают с fill и mixed.
fn pop_value(q: &mut Q) -> i32 {
    q.pop_mut().expect("очередь не пуста")
}

fn run_fill_mut(n: i32, ph: &mut Phases) -> u64 {
    let mut h = 0;
    let mut q = Q::default();
    for i in 0..n {
        q.push_mut(i);
    }
    ph.mark("build");
    while !q.is_empty() {
        h = mix(h, pop_value(&mut q));
    }
    ph.mark("drain");
    h
}

fn run_mixed_mut(n: i32, ph: &mut Phases) -> u64 {
    let n = usize::try_from(n).unwrap();
    let (mut h, mut i) = (0, 0);
    let mut q = Q::default();
    while q.len() < n {
        q.push_mut(i);
        q.push_mut(i + 1);
        i += 2;
        h = mix(h, pop_value(&mut q));
    }
    ph.mark("build");
    while !q.is_empty() {
        h = mix(h, pop_value(&mut q));
    }
    ph.mark("drain");
    h
}

/// Волны: живое множество растёт до N и сжимается до N/100, `WAVES` раз,
/// через операции на месте; затем опустошение (удержание памяти после
/// сжатия, `docs/pool.md`).
const WAVES: usize = 4;

fn run_wave(n: i32, ph: &mut Phases) -> u64 {
    let n = usize::try_from(n).unwrap();
    let (mut h, mut i) = (0, 0);
    let mut q = Q::default();
    for _ in 0..WAVES {
        while q.len() < n {
            q.push_mut(i);
            i += 1;
        }
        ph.mark("grow");
        while q.len() > n / 100 {
            h = mix(h, pop_value(&mut q));
        }
        ph.mark("shrink");
    }
    while !q.is_empty() {
        h = mix(h, pop_value(&mut q));
    }
    h
}

/// Как mixed, но все версии живы до конца и освобождаются в порядке создания.
fn last(v: &[Q]) -> &Q {
    v.last().expect("версий не меньше одной")
}

fn run_persistent(n: i32, ph: &mut Phases) -> u64 {
    let n = usize::try_from(n).unwrap();
    let (mut h, mut i) = (0, 0);
    let mut versions = vec![Q::default()];
    while last(&versions).len() < n {
        let next = last(&versions).push(i);
        versions.push(next);
        let next = last(&versions).push(i + 1);
        versions.push(next);
        i += 2;
        h = mix_front(h, last(&versions));
        let next = last(&versions).pop().unwrap();
        versions.push(next);
    }
    ph.mark("build");
    while !last(&versions).is_empty() {
        h = mix_front(h, last(&versions));
        let next = last(&versions).pop().expect("версия не пуста");
        versions.push(next);
    }
    ph.mark("drain");
    drop(versions); // Vec освобождает элементы по порядку, как цикл в C
    ph.mark("destroy");
    h
}

/// Ветвящаяся история: `BRANCHES` активных веток. На каждом шаге случайная
/// ветка порождает потомка от своей вершины (вершина заменяется потомком,
/// прежняя версия остаётся живой). Раз в 64 шага ветка вместо этого
/// ответвляется от случайной версии из всей истории. Голова родителя читается
/// (повторное чтение старых версий); pop с вероятностью 1/3, иначе push.
/// Выбор задаёт xorshift64 — одинаково в C и Rust. Все версии живы до конца и
/// освобождаются в порядке создания.
#[allow(clippy::cast_possible_truncation, reason = "индексы < versions.len()")]
#[allow(clippy::verbose_bit_mask, reason = "то же выражение, что в C")]
fn run_branching(n: i32, ph: &mut Phases) -> u64 {
    const BRANCHES: u64 = 8;
    let (mut x, mut h): (u64, u64) = (0x9E37_79B9_7F4A_7C15, 0);
    let mut heads = [0_usize; BRANCHES as usize];
    let mut versions = vec![Q::default()];
    for k in 0..n {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        let slot = ((x >> 3) % BRANCHES) as usize;
        let idx = if x & 63 == 0 {
            ((x >> 12) % versions.len() as u64) as usize
        } else {
            heads[slot]
        };
        let parent = &versions[idx];
        let child = match parent.front() {
            None => parent.push(k),
            Some(&v) => {
                h = mix(h, v);
                if (x >> 40) % 3 == 0 {
                    parent.pop().expect("родитель не пуст")
                } else {
                    parent.push(k)
                }
            }
        };
        heads[slot] = versions.len();
        versions.push(child);
    }
    ph.mark("build");
    drop(versions);
    ph.mark("destroy");
    h
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let [_, scenario, n] = args.as_slice() else {
        eprintln!("usage: fqueue_bench <fill|mixed|persistent|branching|fill_mut|mixed_mut|wave> <N>");
        return ExitCode::from(2);
    };
    let Ok(n) = n.parse() else {
        eprintln!("N must be an integer: {n}");
        return ExitCode::from(2);
    };
    let mut ph = Phases {
        on: std::env::var_os("FQ_PHASES").is_some_and(|v| v == "1"),
        t0: Instant::now(),
    };
    let h = match scenario.as_str() {
        "fill" => run_fill(n, &mut ph),
        "mixed" => run_mixed(n, &mut ph),
        "persistent" => run_persistent(n, &mut ph),
        "branching" => run_branching(n, &mut ph),
        "fill_mut" => run_fill_mut(n, &mut ph),
        "mixed_mut" => run_mixed_mut(n, &mut ph),
        "wave" => run_wave(n, &mut ph),
        _ => {
            eprintln!("unknown scenario: {scenario}");
            return ExitCode::from(2);
        }
    };
    println!("{h}");
    ExitCode::SUCCESS
}
