//! Пул узлов — альтернативный аллокатор узлов (`docs/pool.md`).
//!
//! [`NodePool`] реализует [`Allocator`]: очередь с пулом — `Queue<T, R,
//! NodePool>`, для краткости [`PooledQueue`](crate::PooledQueue) и
//! [`SyncPooledQueue`](crate::SyncPooledQueue). Семантика, счётчики ссылок и
//! освобождение узла при обнулении счётчика — те же, но узел уходит в пул, а
//! не в глобальный аллокатор. Память берётся плитами по 16 384 байта,
//! выровненными по своему размеру, у глобального аллокатора и не
//! возвращается, пока в пуле есть живой узел: пул держит пик живого множества
//! до [`trim`].
//!
//! Пулы — по классам размера (16, 24, …, 256 байт): обобщённых статических
//! переменных в Rust нет, поэтому очереди с одинаковым размером узла
//! (`PooledQueue<i32>` и `SyncPooledQueue<u32>`) делят пул; он потокобезопасен
//! для любых очередей. Блоки больше 256 байт, с выравниванием больше 16 и
//! нулевого размера пул передаёт глобальному аллокатору.
//!
//! Устройство: у каждого потока на каждый класс — кэш (список свободных
//! узлов, LIFO, и плита, которую он нарезает) в куче за указателем из
//! `thread_local!`. Кэш владеет плитами, которые нарезал: заголовок плиты
//! указывает на него, а плита узла — его адрес, округлённый вниз до размера
//! плиты. Узел, освобождённый владельцем, идёт в его список; освобождённый
//! другим потоком — в стек освобождений владельца (без блокировки: кладёт
//! любой поток, владелец забирает целиком, когда его список пуст). Узлы не
//! уходят от владельца, и строки кэша с узлами не переходят между потоками
//! через пул. Чужие узлы поток копит пачкой до `REMOTE_BATCH` для одного
//! владельца и отдаёт одной атомарной операцией. При выходе потока его кэш
//! со всем, что в нём, становится сиротой, и его забирает следующий поток,
//! которому нужен кэш; кэши освобождаются только вместе с пулом — на них
//! указывают заголовки плит.
//!
//! Свободный узел хранит ссылку пула в слове 0 (следующий узел списка, пачки
//! или стека освобождений). Заголовок плиты — её первый узел: слово 0 —
//! следующая плита пула, слово 1 — кэш-владелец.
//!
//! Второй модуль крейта с `unsafe` (после `node`). Инварианты:
//! - узел, выданный `alloc`, не выдаётся снова до `free`;
//! - плита освобождается только в [`trim`], когда ни одного живого узла нет;
//!   кэш — никогда (на него указывают заголовки плит);
//! - владелец плиты не меняется, пока плита жива;
//! - поля кэша, кроме стека освобождений, пишет только поток, которому кэш
//!   принадлежит, или [`trim`] в спокойном состоянии; другие потоки читают их
//!   только через атомарные поля (`stats`).

#![allow(unsafe_code)]
#![warn(clippy::undocumented_unsafe_blocks)]

use std::alloc::{
    AllocError, Allocator, Global, Layout, alloc as global_alloc, dealloc, handle_alloc_error,
};
use std::cell::{Cell, UnsafeCell};
use std::ptr::{self, NonNull};
use std::sync::atomic::{
    AtomicBool, AtomicPtr, AtomicU32, AtomicU64, Ordering::Acquire, Ordering::Relaxed,
    Ordering::Release,
};

/// Страница Apple M: заголовок плиты узла — на странице узла, и проверка
/// владельца при освобождении не стоит лишней записи TLB (`docs/pool.md`).
const SLAB_BYTES: usize = 16384;
/// Чужих узлов в пачке, которую поток отдаёт владельцу одной операцией.
const REMOTE_BATCH: u32 = 64;
const MAX_NODE: usize = 256;
const CLASSES: usize = MAX_NODE / 8 + 1;

/// Плита выровнена по своему размеру: по адресу узла находится заголовок.
const SLAB_LAYOUT: Layout = match Layout::from_size_align(SLAB_BYTES, SLAB_BYTES) {
    Ok(l) => l,
    Err(_) => panic!("размер плиты"),
};

/// Аллокатор узлов из пула: `Queue<T, R, NodePool>`.
///
/// Без размера: пул один на процесс (по классам размера), а `NodePool` — лишь
/// способ к нему обратиться; узел и версия очереди с ним не растут.
///
/// Выделение не возвращает [`AllocError`]: при нехватке памяти под новую плиту
/// вызывается [`handle_alloc_error`], как у `Box::new`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NodePool;

// SAFETY: блок класса выдаётся одному владельцу до `deallocate` с той же
// раскладкой (тот же класс) и остаётся действительным до него: плиты
// освобождает только `trim`, когда в пуле нет ни одного выданного блока.
// Блоки не из пула — у `Global`, а `class_of` направляет `deallocate` туда же,
// куда `allocate`. Все экземпляры `NodePool` — один и тот же пул.
unsafe impl Allocator for NodePool {
    #[inline]
    fn allocate(&self, layout: Layout) -> Result<NonNull<[u8]>, AllocError> {
        let Some(class) = class_of(layout) else {
            return Global.allocate(layout);
        };
        Ok(NonNull::slice_from_raw_parts(alloc(class), class * 8))
    }

    #[inline]
    unsafe fn deallocate(&self, ptr: NonNull<u8>, layout: Layout) {
        match class_of(layout) {
            Some(class) => free(class, ptr),
            // SAFETY: блок выделен `Global` (тот же `class_of` в `allocate`)
            // с этой раскладкой.
            None => unsafe { Global.deallocate(ptr, layout) },
        }
    }
}

/// Класс блока: его размер в пуле / 8, или `None` — блок не из пула.
///
/// Узлы класса нарезаются с шагом в размер класса от начала плиты (оно
/// выровнено по размеру плиты), поэтому при выравнивании 16 размер класса
/// кратен 16. Свободный узел хранит два слова (заголовок плиты — тоже) —
/// отсюда минимум 16 байт. Раскладка узла известна при компиляции, и после
/// встраивания класс — константа. Любой размер, допустимый для `deallocate`
/// блока (между запрошенным и выданным), попадает в тот же класс.
#[inline]
const fn class_of(layout: Layout) -> Option<usize> {
    let (size, align) = (layout.size(), layout.align());
    if size == 0 || size > MAX_NODE || align > 16 {
        return None;
    }
    let size = size.next_multiple_of(if align > 8 { 16 } else { 8 });
    Some(if size < 16 { 2 } else { size / 8 })
}

// ---------------------------------------------------------------------------
// Пул класса и кэш потока.

struct Inner {
    slabs: *mut u8, // все плиты, через слово 0
    nslabs: usize,
    registry: *const Cache, // все кэши, сироты тоже
    orphans: *const Cache,  // кэши завершившихся потоков
    shared: *const Cache,   // кэш потоков без своего, под `shared_lock`
    slab_allocs: u64,
    slab_frees: u64,
    adoptions: u64,
}

struct ClassPool {
    lock: AtomicBool,
    shared_lock: AtomicBool,
    inner: UnsafeCell<Inner>,
}

// SAFETY: `inner` меняется только под `lock`, общий кэш — под `shared_lock`.
unsafe impl Sync for ClassPool {}

impl ClassPool {
    const fn new() -> Self {
        Self {
            lock: AtomicBool::new(false),
            shared_lock: AtomicBool::new(false),
            inner: UnsafeCell::new(Inner {
                slabs: ptr::null_mut(),
                nslabs: 0,
                registry: ptr::null(),
                orphans: ptr::null(),
                shared: ptr::null(),
                slab_allocs: 0,
                slab_frees: 0,
                adoptions: 0,
            }),
        }
    }

    fn lock(&self) -> Guard<'_> {
        Guard {
            pool: self,
            _spin: Spin::lock(&self.lock),
        }
    }
}

/// Спин-блокировка test-and-test-and-set с уступкой процессора
/// (как в C и Zig, `docs/pool.md`).
struct Spin<'a>(&'a AtomicBool);

impl<'a> Spin<'a> {
    fn lock(flag: &'a AtomicBool) -> Self {
        while flag.swap(true, Acquire) {
            while flag.load(Relaxed) {
                std::thread::yield_now();
            }
        }
        Spin(flag)
    }
}

impl Drop for Spin<'_> {
    fn drop(&mut self) {
        self.0.store(false, Release);
    }
}

/// Блокировка пула класса: отпускается вместе с охранником.
struct Guard<'a> {
    pool: &'a ClassPool,
    _spin: Spin<'a>,
}

impl Guard<'_> {
    fn inner(&mut self) -> &mut Inner {
        // SAFETY: блокировка наша, других ссылок на `inner` нет.
        unsafe { self.pool.inner.as_mut_unchecked() }
    }

    /// Новая плита во владении `c`; заголовок записан.
    fn new_slab(&mut self, c: *const Cache) -> *mut u8 {
        // SAFETY: размер плиты ненулевой.
        let slab = unsafe { global_alloc(SLAB_LAYOUT) };
        if slab.is_null() {
            handle_alloc_error(SLAB_LAYOUT);
        }
        let inner = self.inner();
        // SAFETY: в плите есть место под заголовок из двух слов.
        unsafe {
            *word(slab, 0) = inner.slabs;
            *word(slab, 1) = c.cast_mut().cast();
        }
        inner.slabs = slab;
        inner.nslabs += 1;
        inner.slab_allocs += 1;
        slab
    }

    /// Свободные узлы: кэши (список, пачка, стек освобождений, ненарезанное).
    fn free_nodes(&mut self, size: usize) -> usize {
        let inner = self.inner();
        let mut n = 0;
        let mut c = inner.registry;
        while !c.is_null() {
            // SAFETY: кэши не освобождаются; ссылки — только на отдельные
            // атомарные поля, `next` — под блокировкой (см. `Cache`).
            unsafe {
                n += (*c).nfree.load(Relaxed) as usize
                    + (*c).nout.load(Relaxed) as usize
                    + remote_len(c)
                    + uncarved(&(*c).cur, &(*c).end, size);
                c = (*c).next.get();
            }
        }
        n
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

/// Слово `i` свободного узла (или заголовка плиты).
///
/// # Safety
/// `node` — узел пула размером не меньше `i + 1` слов.
// Плиты выровнены по своему размеру, размеры классов кратны 8 (`class_of`):
// узлы — на 8.
#[allow(clippy::cast_ptr_alignment)]
unsafe fn word(node: *mut u8, i: usize) -> *mut *mut u8 {
    // SAFETY: см. контракт.
    unsafe { node.cast::<*mut u8>().add(i) }
}

/// Кэш-владелец плиты узла.
///
/// # Safety
/// `node` — узел живой плиты пула.
#[inline]
unsafe fn owner(node: *mut u8) -> *const Cache {
    let slab = node.map_addr(|a| a & !(SLAB_BYTES - 1));
    // SAFETY: см. контракт; слово 1 заголовка записано в `new_slab` и не
    // меняется, пока плита жива.
    unsafe { (*word(slab, 1)).cast_const().cast() }
}

/// Стек освобождений кэша — своя строка: его пишут другие потоки.
#[repr(align(128))]
struct Remote(AtomicPtr<u8>);

/// Кэш потока на один класс — в куче, как в C: `thread_local!` хранит только
/// указатели на кэши. Кэш читают и пишут разные потоки (владелец; чужие
/// освобождения; `stats`, `trim` и выход потоков — под блокировкой), поэтому
/// все обращения, и владельца тоже, — через сырой указатель по отдельным
/// полям: ссылка на всю структуру считалась бы доступом и к полям, которые в
/// это время меняет другой поток.
/// Своя строка кэша (128 байт на Apple M): кэши разных потоков меняются на
/// каждой операции, и два в одной строке гоняли бы её между ядрами.
#[repr(align(128))]
struct Cache {
    free: AtomicPtr<u8>, // список свободных, LIFO
    nfree: AtomicU32,
    cur: AtomicPtr<u8>, // нарезка: следующий узел плиты
    end: AtomicPtr<u8>,
    // Чужие узлы, освобождённые здесь, — пачкой одному владельцу.
    out_owner: Cell<*const Cache>,
    out_head: Cell<*mut u8>,
    out_tail: Cell<*mut u8>,
    nout: AtomicU32,
    collects: AtomicU64,        // сколько раз забран стек освобождений
    next: Cell<*const Cache>,   // реестр класса; только под блокировкой пула
    orphan: Cell<*const Cache>, // следующая сирота; только под блокировкой
    remote: Remote,             // узлы плит кэша, освобождённые другими потоками
}

impl Cache {
    fn new() -> *const Cache {
        Box::into_raw(Box::new(Cache {
            free: AtomicPtr::new(ptr::null_mut()),
            nfree: AtomicU32::new(0),
            cur: AtomicPtr::new(ptr::null_mut()),
            end: AtomicPtr::new(ptr::null_mut()),
            out_owner: Cell::new(ptr::null()),
            out_head: Cell::new(ptr::null_mut()),
            out_tail: Cell::new(ptr::null_mut()),
            nout: AtomicU32::new(0),
            collects: AtomicU64::new(0),
            next: Cell::new(ptr::null()),
            orphan: Cell::new(ptr::null()),
            remote: Remote(AtomicPtr::new(ptr::null_mut())),
        }))
    }
}

/// Положить цепочку `head..=last` (связана словом 0) в стек освобождений
/// владельца.
///
/// # Safety
/// `owner` — кэш пула; узлы цепочки свободны, принадлежат плитам `owner` и
/// больше никем не используются.
unsafe fn push_remote(owner: *const Cache, head: *mut u8, last: *mut u8) {
    // SAFETY: см. контракт; кэши не освобождаются.
    let remote = unsafe { &(*owner).remote.0 };
    let mut top = remote.load(Relaxed);
    loop {
        // SAFETY: `last` — свободный узел, слово 0 — под стек.
        unsafe { *word(last, 0) = top };
        match remote.compare_exchange_weak(top, head, Release, Relaxed) {
            Ok(_) => return,
            Err(t) => top = t,
        }
    }
}

/// Отдать накопленную пачку чужих узлов владельцу, если она есть.
///
/// # Safety
/// `c` — свой живой кэш (или кэш, с которым больше никто не работает).
unsafe fn flush_out(c: *const Cache) {
    // SAFETY: см. контракт; поля пачки пишет только поток кэша.
    unsafe {
        let head = (*c).out_head.get();
        if head.is_null() {
            return;
        }
        push_remote((*c).out_owner.get(), head, (*c).out_tail.get());
        (*c).out_head.set(ptr::null_mut());
        (*c).out_tail.set(ptr::null_mut());
        (*c).nout.store(0, Relaxed);
    }
}

/// Забрать стек освобождений кэша в начало его списка; `false`, если он пуст.
///
/// # Safety
/// `c` — свой живой кэш (или общий под блокировкой).
unsafe fn collect(c: *const Cache) -> bool {
    // SAFETY: см. контракт; узлы стека освобождены другими потоками и
    // переданы нам `Release`-записью, которую видит `Acquire`.
    unsafe {
        let remote = &(*c).remote.0;
        if remote.load(Relaxed).is_null() {
            return false;
        }
        let head = remote.swap(ptr::null_mut(), Acquire);
        let (mut last, mut n) = (head, 1);
        while !(*word(last, 0)).is_null() {
            last = *word(last, 0);
            n += 1;
        }
        *word(last, 0) = (*c).free.load(Relaxed);
        (*c).free.store(head, Relaxed);
        (*c).nfree.store((*c).nfree.load(Relaxed) + n, Relaxed);
        (*c).collects
            .store((*c).collects.load(Relaxed) + 1, Relaxed);
    }
    true
}

/// Длина стека освобождений кэша: забрать целиком, посчитать, вернуть.
///
/// # Safety
/// `c` — кэш пула.
unsafe fn remote_len(c: *const Cache) -> usize {
    // SAFETY: см. контракт; забранная цепочка на время подсчёта наша.
    unsafe {
        let head = (*c).remote.0.swap(ptr::null_mut(), Acquire);
        if head.is_null() {
            return 0;
        }
        let (mut last, mut n) = (head, 1);
        while !(*word(last, 0)).is_null() {
            last = *word(last, 0);
            n += 1;
        }
        push_remote(c, head, last);
        n
    }
}

/// Указатели на кэши потока по классам; кэш создаётся при первом обращении.
/// Без деструктора: доступ к `thread_local!` с деструктором проверял бы его
/// состояние на каждой операции; выход потока обслуживает [`ExitGuard`].
struct Caches([Cell<*mut Cache>; CLASSES]);

/// Выход потока: пачки чужих узлов — владельцам, кэши — в сироты.
/// Регистрируется при создании первого кэша потока.
struct ExitGuard;

impl Drop for ExitGuard {
    fn drop(&mut self) {
        CACHES.with(release_caches);
    }
}

fn release_caches(caches: &Caches) {
    for (class, slot) in caches.0.iter().enumerate() {
        let cache: *const Cache = slot.replace(ptr::null_mut());
        if cache.is_null() {
            continue;
        }
        // SAFETY: кэш свой и живой.
        unsafe { flush_out(cache) };
        let mut g = POOLS[class].lock();
        let inner = g.inner();
        // SAFETY: `orphan` — под блокировкой.
        unsafe { (*cache).orphan.set(inner.orphans) };
        inner.orphans = cache;
    }
}

static POOLS: [ClassPool; CLASSES] = [const { ClassPool::new() }; CLASSES];

thread_local! {
    static CACHES: Caches = const { Caches([const { Cell::new(ptr::null_mut()) }; CLASSES]) };
    static EXIT: ExitGuard = const { ExitGuard };
}

/// Кэш потока для класса: сирота, если есть, иначе новый (в реестр); null,
/// если поток уже завершается (деструктор `EXIT` отработал или идёт — кэш не
/// на кого было бы оставить): тогда узлы идут мимо кэша.
fn register(class: usize, slot: &Cell<*mut Cache>) -> *const Cache {
    if EXIT.try_with(|_| ()).is_err() {
        return ptr::null();
    }
    let mut g = POOLS[class].lock();
    let inner = g.inner();
    let mut cache = inner.orphans;
    if cache.is_null() {
        cache = Cache::new();
        // SAFETY: кэш только что создан, `next` — под блокировкой.
        unsafe { (*cache).next.set(inner.registry) };
        inner.registry = cache;
    } else {
        // SAFETY: сирота — кэш пула, `orphan` — под блокировкой.
        inner.orphans = unsafe { (*cache).orphan.get() };
        inner.adoptions += 1;
    }
    slot.set(cache.cast_mut());
    cache
}

// ---------------------------------------------------------------------------
// Выделение и освобождение. Быстрые пути встраиваются (как `static inline`
// в C), медленные — отдельные функции без `#[cold]`, как в C: нарезка плиты
// в `fill` — частый путь.

#[inline]
fn alloc(class: usize) -> NonNull<u8> {
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
    unsafe { NonNull::new_unchecked(node) }
}

/// Узел из кэша, у которого список пуст: стек освобождений, затем плита
/// кэша, затем новая.
///
/// # Safety
/// `c` — свой живой кэш (или общий под `shared_lock`).
unsafe fn alloc_from(class: usize, c: *const Cache) -> *mut u8 {
    let size = class * 8;
    // SAFETY: см. контракт; у усыновлённой сироты список может быть не пуст.
    unsafe {
        let n = (*c).free.load(Relaxed);
        if !n.is_null() || collect(c) {
            let n = (*c).free.load(Relaxed);
            (*c).free.store(*word(n, 0), Relaxed);
            (*c).nfree.store((*c).nfree.load(Relaxed) - 1, Relaxed);
            return n;
        }
        // Нарезка в пределах плиты: заголовок и `per_slab` узлов, `cur` < `end`.
        let mut cur = (*c).cur.load(Relaxed);
        if cur.is_null() || cur == (*c).end.load(Relaxed) {
            let slab = POOLS[class].lock().new_slab(c);
            let per_slab = SLAB_BYTES / size - 1;
            cur = slab.add(size);
            (*c).end.store(cur.add(per_slab * size), Relaxed);
        }
        (*c).cur.store(cur.add(size), Relaxed);
        cur
    }
}

/// Список пуст (или кэша ещё нет).
#[inline(never)]
fn alloc_slow(class: usize, slot: &Cell<*mut Cache>) -> *mut u8 {
    let mut c: *const Cache = slot.get();
    if c.is_null() {
        c = register(class, slot);
        if c.is_null() {
            return alloc_uncached(class);
        }
    }
    // SAFETY: свой живой кэш.
    unsafe { alloc_from(class, c) }
}

/// Поток завершается (выделение из деструктора thread-local другого
/// значения после `EXIT`): узел из общего кэша класса под блокировкой.
#[cold]
fn alloc_uncached(class: usize) -> *mut u8 {
    let pool = &POOLS[class];
    let c = {
        let mut g = pool.lock();
        let inner = g.inner();
        if inner.shared.is_null() {
            let c = Cache::new();
            // SAFETY: кэш только что создан, `next` — под блокировкой.
            unsafe { (*c).next.set(inner.registry) };
            inner.registry = c;
            inner.shared = c;
        }
        inner.shared
    };
    // Блокировка пула отпущена: новую плиту `alloc_from` берёт под ней сам.
    let _s = Spin::lock(&pool.shared_lock);
    // SAFETY: общий кэш — только под `shared_lock`.
    unsafe { alloc_from(class, c) }
}

#[inline]
fn free(class: usize, node: NonNull<u8>) {
    let p = node.as_ptr();
    // SAFETY: узел выдан пулом этого класса, его плита жива.
    let owner = unsafe { owner(p) };
    CACHES.with(|cs| {
        let slot = &cs.0[class];
        let c = slot.get();
        if ptr::eq(owner, c) {
            // SAFETY: свой живой кэш; узел больше не используется, слово 0
            // — под список.
            unsafe {
                let k = (*c).nfree.load(Relaxed) + 1;
                *word(p, 0) = (*c).free.load(Relaxed);
                (*c).free.store(p, Relaxed);
                (*c).nfree.store(k, Relaxed);
            }
            return;
        }
        free_remote(class, slot, owner, p);
    });
}

/// Узел чужой плиты: в пачку потока для его владельца (пачка другому
/// владельцу уходит первой). Без кэша — владельцу сразу.
#[inline(never)]
fn free_remote(class: usize, slot: &Cell<*mut Cache>, owner: *const Cache, p: *mut u8) {
    let mut c: *const Cache = slot.get();
    if c.is_null() {
        c = register(class, slot);
        if c.is_null() {
            // SAFETY: узел свободен и принадлежит плите `owner`.
            unsafe { push_remote(owner, p, p) };
            return;
        }
    }
    // SAFETY: свой живой кэш; узел свободен, слово 0 — под пачку.
    unsafe {
        if !ptr::eq((*c).out_owner.get(), owner) {
            flush_out(c);
            (*c).out_owner.set(owner);
        }
        let head = (*c).out_head.get();
        *word(p, 0) = head;
        if head.is_null() {
            (*c).out_tail.set(p);
        }
        (*c).out_head.set(p);
        let k = (*c).nout.load(Relaxed) + 1;
        (*c).nout.store(k, Relaxed);
        if k == REMOTE_BATCH {
            flush_out(c);
        }
    }
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
    /// Из них свободных: в кэшах потоков и ненарезанных.
    pub free: usize,
    /// Из них живых: `capacity - free`.
    pub live: usize,
    /// Плит выделено за всё время (обращения к глобальному аллокатору).
    pub slab_allocs: u64,
    /// Плит возвращено ([`trim`]).
    pub slab_frees: u64,
    /// Сколько раз владельцы забрали узлы, освобождённые другими потоками.
    pub collects: u64,
    /// Кэшей завершившихся потоков, взятых новыми потоками.
    pub adoptions: u64,
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
        s.adoptions += inner.adoptions;
        let mut c = inner.registry;
        while !c.is_null() {
            // SAFETY: кэши не освобождаются; `next` — под блокировкой.
            unsafe {
                s.collects += (*c).collects.load(Relaxed);
                c = (*c).next.get();
            }
        }
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
        let mut c = inner.registry;
        while !c.is_null() {
            // SAFETY: кэш реестра; его поток пулом сейчас не пользуется;
            // ссылки — только на отдельные поля (см. `Cache`).
            unsafe {
                (*c).free.store(ptr::null_mut(), Relaxed);
                (*c).nfree.store(0, Relaxed);
                (*c).cur.store(ptr::null_mut(), Relaxed);
                (*c).end.store(ptr::null_mut(), Relaxed);
                (*c).out_owner.set(ptr::null());
                (*c).out_head.set(ptr::null_mut());
                (*c).out_tail.set(ptr::null_mut());
                (*c).nout.store(0, Relaxed);
                (*c).remote.0.store(ptr::null_mut(), Relaxed);
                c = (*c).next.get();
            }
        }
    }
    bytes
}
