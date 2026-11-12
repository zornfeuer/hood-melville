//! Пул узлов — альтернативное хранение узлов (`docs/pool.md`; C и Zig
//! следуют тому же алгоритму с теми же константами).
//!
//! Очередь с пулом — [`PooledQueue`](crate::PooledQueue) (`Queue<T,
//! Pooled<Local>>`) и [`SyncPooledQueue`](crate::SyncPooledQueue)
//! (`Queue<T, Pooled<Atomic>>`): семантика, счётчики ссылок и освобождение
//! узла при обнулении счётчика — те же, но узел уходит в пул, а не в
//! глобальный аллокатор. Память берётся плитами по 65 536 байт у глобального
//! аллокатора и не возвращается, пока в пуле есть живой узел: пул держит пик
//! живого множества до [`trim`].
//!
//! Пулы — по классам размера узла (8, 16, …, 256 байт): обобщённых
//! статических переменных в Rust нет, поэтому очереди с одинаковым размером
//! узла (`PooledQueue<i32>` и `SyncPooledQueue<u32>`) делят пул; он
//! потокобезопасен для любых очередей. Узлы больше 256 байт не
//! поддерживаются (ошибка компиляции): храните `Box<T>`.
//!
//! Устройство: у каждого потока на каждый класс — кэш (список свободных
//! узлов, LIFO, и плита, которую он нарезает) в куче за указателем из
//! `thread_local!`; кэш длиннее
//! `CACHE_MAX` отдаёт `BATCH` узлов в общий стек пачек под спин-блокировкой,
//! пустой — забирает пачку оттуда до нарезки новой плиты. При выходе потока
//! кэш уходит в общий стек (`Drop`). Свободный узел хранит ссылки пула в
//! первых двух словах: слово 0 — следующий узел списка или пачки; в голове
//! пачки слово 1 — следующая пачка; во втором узле пачки слово 1 — число узлов
//! в ней (в пачке из одного узла второго нет).
//!
//! Второй модуль крейта с `unsafe` (после `node`). Инварианты:
//! - узел, выданный [`alloc`], не выдаётся снова до [`free`];
//! - плита освобождается только в [`trim`], когда ни одного живого узла нет;
//! - кэш потока читают другие потоки только через атомарные поля (`stats`) и
//!   пишут только в `trim`, который требует, чтобы пулом никто не пользовался.

#![allow(unsafe_code)]
#![warn(clippy::undocumented_unsafe_blocks)]

use std::alloc::{Layout, alloc as global_alloc, dealloc, handle_alloc_error};
use std::cell::{Cell, UnsafeCell};
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::sync::atomic::{
    AtomicBool, AtomicPtr, AtomicU32, Ordering::Acquire, Ordering::Relaxed, Ordering::Release,
};

use crate::refcount::{RefCount, ThreadSafe, sealed};
use crate::storage::NodeStorage;

const SLAB_BYTES: usize = 65536;
const CACHE_MAX: u32 = 512;
const BATCH: u32 = 256;
/// Выравнивание плиты; узлы в ней — по `SLAB_ALIGN` и размеру узла.
const SLAB_ALIGN: usize = 16;
const MAX_NODE: usize = 256;
const CLASSES: usize = MAX_NODE / 8 + 1;

const SLAB_LAYOUT: Layout = match Layout::from_size_align(SLAB_BYTES, SLAB_ALIGN) {
    Ok(l) => l,
    Err(_) => panic!("размер плиты"),
};

/// Политика узлов: счётчик ссылок `R` ([`Local`](crate::refcount::Local) или
/// [`Atomic`](crate::refcount::Atomic)), узлы — из пула.
pub struct Pooled<R: RefCount>(std::convert::Infallible, PhantomData<R>);

impl<R: RefCount> sealed::Sealed for Pooled<R> {}
// Пул потокобезопасен; поток решает счётчик.
impl<R: RefCount + ThreadSafe> ThreadSafe for Pooled<R> {}

impl<R: RefCount> RefCount for Pooled<R> {
    type Counter = R::Counter;
    type Storage = Pool;

    #[inline]
    fn one() -> R::Counter {
        R::one()
    }
    #[inline]
    fn increment(counter: &R::Counter) {
        R::increment(counter);
    }
    #[inline]
    fn decrement(counter: &R::Counter) -> bool {
        R::decrement(counter)
    }
    #[inline]
    fn is_unique(counter: &R::Counter) -> bool {
        R::is_unique(counter)
    }
    #[inline]
    fn touch(counter: &R::Counter) {
        R::touch(counter);
    }
    fn get(counter: &R::Counter) -> u32 {
        R::get(counter)
    }
}

/// Узел — из пула.
pub enum Pool {}

impl sealed::Sealed for Pool {}

impl NodeStorage for Pool {
    #[inline]
    fn place<N>(node: N) -> NonNull<N> {
        let ptr = alloc::<N>();
        // SAFETY: свежий узел пула под `N`: размер и выравнивание проверены в
        // `class_of`, память ничья.
        unsafe { ptr.as_ptr().write(node) };
        ptr
    }

    #[inline]
    unsafe fn take<N>(ptr: NonNull<N>) -> N {
        // SAFETY: узел живой и наш (контракт `take`); после чтения он больше не
        // используется как `N` и возвращается в пул.
        let node = unsafe { ptr.as_ptr().read() };
        free::<N>(ptr);
        node
    }
}

/// Класс узла `N`: размер / 8. Проверка — на этапе компиляции.
const fn class_of<N>() -> usize {
    let size = size_of::<N>();
    assert!(size <= MAX_NODE, "узел больше 256 байт: храните Box<T>");
    assert!(align_of::<N>() <= SLAB_ALIGN && align_of::<N>() >= 8);
    assert!(size >= 2 * size_of::<*mut u8>() && size.is_multiple_of(8));
    size / 8
}

// ---------------------------------------------------------------------------
// Пул класса и кэш потока.

struct Inner {
    global: usize,  // узлов в пачках
    slabs: *mut u8, // все плиты, через слово 0
    nslabs: usize,
    abandoned: usize, // ненарезанные узлы плит завершившихся потоков
    registry: *const Cache,
    slab_allocs: u64,
    slab_frees: u64,
    spills: u64,
    refills: u64,
}

struct ClassPool {
    lock: AtomicBool,
    batches: AtomicPtr<u8>, // стек пачек (подсматривается без блокировки)
    inner: UnsafeCell<Inner>,
}

// SAFETY: `inner` меняется только под `lock`; `batches` атомарный.
unsafe impl Sync for ClassPool {}

impl ClassPool {
    const fn new() -> Self {
        Self {
            lock: AtomicBool::new(false),
            batches: AtomicPtr::new(ptr::null_mut()),
            inner: UnsafeCell::new(Inner {
                global: 0,
                slabs: ptr::null_mut(),
                nslabs: 0,
                abandoned: 0,
                registry: ptr::null(),
                slab_allocs: 0,
                slab_frees: 0,
                spills: 0,
                refills: 0,
            }),
        }
    }

    /// Спин-блокировка test-and-test-and-set с уступкой процессора
    /// (одинакова в C, Rust и Zig, `docs/pool.md`).
    fn lock(&self) -> Guard<'_> {
        while self.lock.swap(true, Acquire) {
            while self.lock.load(Relaxed) {
                std::thread::yield_now();
            }
        }
        Guard(self)
    }
}

struct Guard<'a>(&'a ClassPool);

impl Guard<'_> {
    fn inner(&mut self) -> &mut Inner {
        // SAFETY: блокировка наша, других ссылок на `inner` нет.
        unsafe { &mut *self.0.inner.get() }
    }

    /// Положить пачку из `n` узлов (связаны словом 0, у последнего — null).
    fn push_batch(&mut self, head: *mut u8, n: u32) {
        // SAFETY: узлы пачки свободны и принадлежат нам; размер ≥ 2 слов.
        unsafe {
            let second = *word(head, 0);
            if !second.is_null() {
                *word(second, 1) = ptr::without_provenance_mut(n as usize);
            }
            *word(head, 1) = self.0.batches.load(Relaxed);
        }
        self.0.batches.store(head, Relaxed);
        self.inner().global += n as usize;
    }

    /// Снять пачку: голова и число узлов.
    fn pop_batch(&mut self) -> Option<(*mut u8, u32)> {
        let head = self.0.batches.load(Relaxed);
        if head.is_null() {
            return None;
        }
        // SAFETY: пачки в стеке — свободные узлы пула.
        let (next, n) = unsafe {
            let second = *word(head, 0);
            // Записано в `push_batch` из `u32`.
            #[allow(clippy::cast_possible_truncation)]
            let n = if second.is_null() {
                1
            } else {
                (*word(second, 1)).addr() as u32
            };
            (*word(head, 1), n)
        };
        self.0.batches.store(next, Relaxed);
        self.inner().global -= n as usize;
        Some((head, n))
    }

    fn new_slab(&mut self) -> *mut u8 {
        // SAFETY: размер плиты ненулевой.
        let slab = unsafe { global_alloc(SLAB_LAYOUT) };
        if slab.is_null() {
            handle_alloc_error(SLAB_LAYOUT);
        }
        let inner = self.inner();
        // SAFETY: в плите есть место под заголовок.
        unsafe { *word(slab, 0) = inner.slabs };
        inner.slabs = slab;
        inner.nslabs += 1;
        inner.slab_allocs += 1;
        slab
    }

    /// Свободные узлы: кэши (список и ненарезанное), общий стек, брошенные.
    fn free_nodes(&mut self, size: usize) -> usize {
        let inner = self.inner();
        let mut n = inner.global + inner.abandoned;
        let mut c = inner.registry;
        while !c.is_null() {
            // SAFETY: кэш в реестре — живой кэш потока (снимается при выходе);
            // ссылки — только на отдельные поля (см. `Cache`).
            unsafe {
                n += (*c).nfree.load(Relaxed) as usize + uncarved(&(*c).cur, &(*c).end, size);
                c = (*c).next.get();
            }
        }
        n
    }
}

impl Drop for Guard<'_> {
    fn drop(&mut self) {
        self.0.lock.store(false, Release);
    }
}

/// Ненарезанные узлы плиты кэша.
fn uncarved(cur: &AtomicPtr<u8>, end: &AtomicPtr<u8>, size: usize) -> usize {
    let cur = cur.load(Relaxed);
    if cur.is_null() {
        0
    } else {
        (end.load(Relaxed).addr() - cur.addr()) / size
    }
}

/// Слово `i` свободного узла.
///
/// # Safety
/// `node` — узел пула размером не меньше `i + 1` слов.
// Узлы выровнены не меньше чем на 8 (`class_of`), плиты — на 16.
#[allow(clippy::cast_ptr_alignment)]
unsafe fn word(node: *mut u8, i: usize) -> *mut *mut u8 {
    // SAFETY: см. контракт.
    unsafe { node.cast::<*mut u8>().add(i) }
}

/// Кэш потока на один класс — в куче, как в C: `thread_local!` хранит только
/// указатели на кэши. Кэш читают и пишут разные потоки (владелец; `stats`,
/// `trim` и выход других потоков — реестр под блокировкой), поэтому все
/// обращения, и владельца тоже, — через сырой указатель по отдельным полям:
/// ссылка на всю структуру (и `&mut` деструктора thread-local) считалась бы
/// доступом и к полям, которые в это время меняет другой поток.
struct Cache {
    free: AtomicPtr<u8>, // список свободных, LIFO
    nfree: AtomicU32,
    cur: AtomicPtr<u8>, // нарезка: следующий узел плиты
    end: AtomicPtr<u8>,
    next: Cell<*const Cache>, // реестр класса; только под блокировкой пула
}

/// Узлов в ненарезанной части плиты кэша.
///
/// # Safety
/// `c` — живой кэш (зарегистрированный или свой).
unsafe fn cache_uncarved(c: *const Cache, size: usize) -> usize {
    // SAFETY: см. контракт; ссылки — на отдельные атомарные поля.
    unsafe { uncarved(&(*c).cur, &(*c).end, size) }
}

/// Весь список кэша — в общий стек пачками.
///
/// # Safety
/// `c` — живой кэш, его список не меняется никем другим (свой поток или
/// спокойное состояние); блокировка пула у вызывающего.
unsafe fn cache_flush(c: *const Cache, g: &mut Guard<'_>) {
    // SAFETY: см. контракт; узлы списка — свободные узлы пула, связаны словом 0.
    unsafe {
        let mut node = (*c).free.load(Relaxed);
        while !node.is_null() {
            let (head, mut last, mut n) = (node, node, 1);
            while n < BATCH && !(*word(last, 0)).is_null() {
                last = *word(last, 0);
                n += 1;
            }
            node = *word(last, 0);
            *word(last, 0) = ptr::null_mut();
            g.push_batch(head, n);
        }
        (*c).free.store(ptr::null_mut(), Relaxed);
        (*c).nfree.store(0, Relaxed);
    }
}

/// Указатели на кэши потока по классам; кэш создаётся при первом обращении.
/// Без деструктора: доступ к `thread_local!` с деструктором проверял бы его
/// состояние на каждой операции; выход потока обслуживает [`ExitGuard`].
struct Caches([Cell<*mut Cache>; CLASSES]);

/// Выход потока: свободные узлы — в общий стек, остаток плиты — до `trim`,
/// кэш — из реестра. Регистрируется при создании первого кэша потока.
struct ExitGuard;

impl Drop for ExitGuard {
    fn drop(&mut self) {
        CACHES.with(release_caches);
    }
}

fn release_caches(caches: &Caches) {
    {
        for (class, slot) in caches.0.iter().enumerate() {
            let cache = slot.replace(ptr::null_mut());
            if cache.is_null() {
                continue;
            }
            let mut g = POOLS[class].lock();
            // SAFETY: кэш свой и живой; блокировка наша; узлы реестра — живые
            // кэши, `next` меняется только под блокировкой.
            unsafe {
                cache_flush(cache, &mut g);
                let uncarved = cache_uncarved(cache, class * 8);
                let inner = g.inner();
                inner.abandoned += uncarved;
                let mut r: *const Cache = ptr::null();
                let mut c = inner.registry;
                while !ptr::eq(c, cache) {
                    r = c;
                    c = (*c).next.get();
                }
                if r.is_null() {
                    inner.registry = (*cache).next.get();
                } else {
                    (*r).next.set((*cache).next.get());
                }
            }
            drop(g);
            // SAFETY: кэш из `Box::into_raw` в `register`, ссылок на него
            // больше нет (из реестра снят).
            drop(unsafe { Box::from_raw(cache) });
        }
    }
}

static POOLS: [ClassPool; CLASSES] = [const { ClassPool::new() }; CLASSES];

thread_local! {
    static CACHES: Caches = const { Caches([const { Cell::new(ptr::null_mut()) }; CLASSES]) };
    static EXIT: ExitGuard = const { ExitGuard };
}

/// Создать кэш потока для класса и внести в реестр; null, если поток уже
/// завершается (деструктор `EXIT` отработал или идёт — кэш не на кого было
/// бы оставить): тогда узлы идут мимо кэша.
fn register(class: usize, slot: &Cell<*mut Cache>) -> *const Cache {
    if EXIT.try_with(|_| ()).is_err() {
        return ptr::null();
    }
    let cache = Box::into_raw(Box::new(Cache {
        free: AtomicPtr::new(ptr::null_mut()),
        nfree: AtomicU32::new(0),
        cur: AtomicPtr::new(ptr::null_mut()),
        end: AtomicPtr::new(ptr::null_mut()),
        next: Cell::new(ptr::null()),
    }));
    let mut g = POOLS[class].lock();
    let inner = g.inner();
    // SAFETY: кэш только что создан, `next` — под блокировкой.
    unsafe { (*cache).next.set(inner.registry) };
    inner.registry = cache;
    slot.set(cache);
    cache
}

// ---------------------------------------------------------------------------
// Выделение и освобождение. Быстрые пути встраиваются (как `static inline`
// в C), медленные — отдельные функции без `#[cold]`, как в C: нарезка плиты
// в `fill` — частый путь.

#[inline]
fn alloc<N>() -> NonNull<N> {
    let class = const { class_of::<N>() };
    let node = CACHES.with(|cs| {
        let slot = &cs.0[class];
        let c = slot.get();
        if !c.is_null() {
            // SAFETY: свой живой кэш; узел списка свободен, слово 0 —
            // следующий.
            unsafe {
                let n = (*c).free.load(Relaxed);
                if !n.is_null() {
                    (*c).free.store(*word(n, 0), Relaxed);
                    (*c).nfree.store((*c).nfree.load(Relaxed) - 1, Relaxed);
                    return n;
                }
            }
        }
        alloc_slow(class, slot)
    });
    // SAFETY: узел пула ненулевой.
    unsafe { NonNull::new_unchecked(node.cast()) }
}

/// Список пуст (или кэша ещё нет): пачка из общего стека, затем плита
/// потока, затем новая.
#[inline(never)]
fn alloc_slow(class: usize, slot: &Cell<*mut Cache>) -> *mut u8 {
    let pool = &POOLS[class];
    let size = class * 8;
    let mut c: *const Cache = slot.get();
    if c.is_null() {
        c = register(class, slot);
        if c.is_null() {
            return alloc_uncached(class);
        }
    }
    if !pool.batches.load(Relaxed).is_null() {
        let mut g = pool.lock();
        if let Some((head, n)) = g.pop_batch() {
            g.inner().refills += 1;
            drop(g);
            // SAFETY: пачка теперь наша, слово 0 головы — остальные узлы; кэш
            // свой и живой.
            unsafe {
                (*c).free.store(*word(head, 0), Relaxed);
                (*c).nfree.store(n - 1, Relaxed);
            }
            return head;
        }
    }
    // SAFETY: свой живой кэш; нарезка в пределах плиты: заголовок и
    // `per_slab` узлов, `cur` < `end`.
    unsafe {
        let mut cur = (*c).cur.load(Relaxed);
        if cur.is_null() || cur == (*c).end.load(Relaxed) {
            let slab = pool.lock().new_slab();
            let per_slab = SLAB_BYTES / size - 1;
            cur = slab.add(size);
            (*c).end.store(cur.add(per_slab * size), Relaxed);
        }
        (*c).cur.store(cur.add(size), Relaxed);
        cur
    }
}

/// Поток завершается (выделение из деструктора thread-local другого
/// значения после `EXIT`): узел из пачки или из новой плиты целиком.
#[cold]
fn alloc_uncached(class: usize) -> *mut u8 {
    let size = class * 8;
    let mut g = POOLS[class].lock();
    if let Some((head, n)) = g.pop_batch() {
        if n > 1 {
            // SAFETY: остаток пачки — свободные узлы.
            let rest = unsafe { *word(head, 0) };
            g.push_batch(rest, n - 1);
        }
        return head;
    }
    let slab = g.new_slab();
    let per_slab = SLAB_BYTES / size - 1;
    // SAFETY: узлы 1..per_slab плиты связываются в список; первый — наш.
    unsafe {
        let first = slab.add(size);
        for i in 1..per_slab {
            let node = first.add(i * size);
            *word(node, 0) = if i + 1 < per_slab {
                node.add(size)
            } else {
                ptr::null_mut()
            };
        }
        if per_slab > 1 {
            // Узлов в плите не больше 4095.
            #[allow(clippy::cast_possible_truncation)]
            g.push_batch(first.add(size), (per_slab - 1) as u32);
        }
        first
    }
}

#[inline]
fn free<N>(node: NonNull<N>) {
    let class = const { class_of::<N>() };
    let p = node.as_ptr().cast::<u8>();
    CACHES.with(|cs| {
        let slot = &cs.0[class];
        let c = slot.get();
        if !c.is_null() {
            // SAFETY: свой живой кэш; узел больше не используется, слово 0
            // — под список.
            unsafe {
                let k = (*c).nfree.load(Relaxed) + 1;
                *word(p, 0) = (*c).free.load(Relaxed);
                (*c).free.store(p, Relaxed);
                (*c).nfree.store(k, Relaxed);
                if k <= CACHE_MAX {
                    return;
                }
            }
        }
        free_slow(class, slot, p);
    });
}

/// Кэша нет — создать и положить узел; иначе список превысил `CACHE_MAX`
/// (узел уже в нём) — `BATCH` узлов с вершины в общий стек.
#[inline(never)]
fn free_slow(class: usize, slot: &Cell<*mut Cache>, p: *mut u8) {
    let c: *const Cache = slot.get();
    if c.is_null() {
        let c = register(class, slot);
        if c.is_null() {
            let mut g = POOLS[class].lock();
            // SAFETY: узел свободен; пачка из одного узла.
            unsafe { *word(p, 0) = ptr::null_mut() };
            g.push_batch(p, 1);
            return;
        }
        // SAFETY: узел свободен; кэш только что создан, список пуст.
        unsafe {
            *word(p, 0) = ptr::null_mut();
            (*c).free.store(p, Relaxed);
            (*c).nfree.store(1, Relaxed);
        }
        return;
    }
    // SAFETY: свой живой кэш; в списке больше `BATCH` узлов, связаны словом 0.
    let head = unsafe {
        let head = (*c).free.load(Relaxed);
        let mut last = head;
        for _ in 1..BATCH {
            last = *word(last, 0);
        }
        (*c).free.store(*word(last, 0), Relaxed);
        *word(last, 0) = ptr::null_mut();
        (*c).nfree.store((*c).nfree.load(Relaxed) - BATCH, Relaxed);
        head
    };
    let mut g = POOLS[class].lock();
    g.push_batch(head, BATCH);
    g.inner().spills += 1;
}

// ---------------------------------------------------------------------------
// Статистика и trim.

/// Статистика пулов (сумма по классам); под конкуренцией — приблизительная.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolStats {
    /// Плит у пулов.
    pub slabs: usize,
    /// Их общий размер, байт.
    pub slab_bytes: usize,
    /// Узлов в них.
    pub capacity: usize,
    /// Из них свободных: в кэшах потоков, ненарезанных, в общем стеке.
    pub free: usize,
    /// Из них живых: `capacity - free`.
    pub live: usize,
    /// Плит выделено за всё время (обращения к глобальному аллокатору).
    pub slab_allocs: u64,
    /// Плит возвращено ([`trim`]).
    pub slab_frees: u64,
    /// Пачек отдано кэшами потоков в общий стек.
    pub spills: u64,
    /// Пачек взято из него.
    pub refills: u64,
}

/// Статистика всех пулов.
#[must_use]
pub fn stats() -> PoolStats {
    let mut s = PoolStats::default();
    for (class, pool) in POOLS.iter().enumerate().skip(2) {
        let size = class * 8;
        let mut g = pool.lock();
        let free = g.free_nodes(size);
        let inner = g.inner();
        let capacity = inner.nslabs * (SLAB_BYTES / size - 1);
        s.slabs += inner.nslabs;
        s.slab_bytes += inner.nslabs * SLAB_BYTES;
        s.capacity += capacity;
        s.free += free;
        s.live += capacity - free;
        s.slab_allocs += inner.slab_allocs;
        s.slab_frees += inner.slab_frees;
        s.spills += inner.spills;
        s.refills += inner.refills;
    }
    s
}

/// Вернуть глобальному аллокатору все плиты тех пулов, в которых нет живых
/// узлов (все версии их очередей уничтожены); возвращает число байт.
///
/// Вызывать в спокойном состоянии: другие потоки в это время пулом не
/// пользуются (их кэши сбрасываются отсюда).
pub fn trim() -> usize {
    let mut bytes = 0;
    for (class, pool) in POOLS.iter().enumerate().skip(2) {
        let size = class * 8;
        let mut g = pool.lock();
        let free = g.free_nodes(size);
        let inner = g.inner();
        if free != inner.nslabs * (SLAB_BYTES / size - 1) || inner.nslabs == 0 {
            continue;
        }
        bytes += inner.nslabs * SLAB_BYTES;
        let mut slab = inner.slabs;
        while !slab.is_null() {
            // SAFETY: плита из `new_slab`, живых узлов в ней нет; заголовок —
            // ссылка на следующую.
            let next = unsafe { *word(slab, 0) };
            // SAFETY: тот же `SLAB_LAYOUT`, что при выделении.
            unsafe { dealloc(slab, SLAB_LAYOUT) };
            inner.slab_frees += 1;
            slab = next;
        }
        inner.slabs = ptr::null_mut();
        inner.nslabs = 0;
        inner.global = 0;
        inner.abandoned = 0;
        let mut c = inner.registry;
        while !c.is_null() {
            // SAFETY: живой кэш реестра; его поток пулом сейчас не пользуется;
            // ссылки — только на отдельные поля (см. `Cache`).
            unsafe {
                (*c).free.store(ptr::null_mut(), Relaxed);
                (*c).nfree.store(0, Relaxed);
                (*c).cur.store(ptr::null_mut(), Relaxed);
                (*c).end.store(ptr::null_mut(), Relaxed);
                c = (*c).next.get();
            }
        }
        pool.batches.store(ptr::null_mut(), Relaxed);
    }
    bytes
}
