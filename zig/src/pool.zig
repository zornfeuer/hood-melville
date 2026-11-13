//! Пул узлов (`docs/pool.md`). `NodePool(Node)` — один пул на тип узла, то
//! есть на тип очереди: состояние пула — переменные этого типа, кэш потока —
//! в куче за `threadlocal`-указателем.
//!
//! Плиты по 16 КиБ, выровненные по своему размеру, берутся у аллокатора,
//! переданного в операции очереди: пул запоминает его при первой плите и в
//! Debug/ReleaseSafe проверяет, что передаётся тот же (сменить его можно,
//! когда пул пуст целиком: после `flushThreadCache` во всех потоках и `trim`);
//! плиты не возвращаются, пока в пуле есть живой узел, — пул держит пик
//! живого множества до `trim`. У каждого потока — список свободных узлов
//! (LIFO) и плита, которую он нарезает. Кэш владеет плитами, которые нарезал:
//! заголовок плиты указывает на него, а плита узла — его адрес, округлённый
//! вниз до размера плиты. Узел, освобождённый владельцем, идёт в его список;
//! освобождённый другим потоком — в стек освобождений владельца (без
//! блокировки: кладёт любой поток, владелец забирает целиком, когда его список
//! пуст). Чужие узлы поток копит пачкой до `remote_batch` для одного владельца
//! и отдаёт одной атомарной операцией.
//!
//! Деструкторов у `threadlocal` в Zig нет: перед выходом потока, который
//! пользовался пулом, вызывайте `flushThreadCache` — пачка чужих узлов уходит
//! владельцу, а кэш становится сиротой, которую возьмёт следующий поток;
//! иначе кэш (его свободные узлы и остаток плиты) удерживается до `trim`.
//! Кэши сирот освобождает `trim`, кэши живых потоков — нет: на них указывают
//! заголовки плит и `threadlocal` их потоков.
//!
//! Свободный узел хранит ссылку пула в слове 0 (следующий узел списка, пачки
//! или стека освобождений). Заголовок плиты — её первый узел: слово 0 —
//! следующая плита пула, слово 1 — кэш-владелец.

const std = @import("std");
const Allocator = std.mem.Allocator;

/// Страница Apple M: заголовок плиты узла — на странице узла, и проверка
/// владельца при освобождении не стоит лишней записи TLB (`docs/pool.md`).
pub const slab_bytes = 16384;
/// Чужих узлов в пачке, которую поток отдаёт владельцу одной операцией.
pub const remote_batch = 64;

const slab_alignment: std.mem.Alignment = .fromByteUnits(slab_bytes);

/// Статистика пула; под конкуренцией — приблизительная.
pub const Stats = struct {
    /// Плит у пула.
    slabs: usize = 0,
    /// Их общий размер, байт.
    slab_bytes: usize = 0,
    /// Узлов в них.
    capacity: usize = 0,
    /// Из них свободных: в кэшах потоков и ненарезанных.
    free: usize = 0,
    /// Из них живых: capacity - free.
    live: usize = 0,
    /// Плит выделено за всё время (обращения к аллокатору).
    slab_allocs: u64 = 0,
    /// Плит возвращено (`trim`).
    slab_frees: u64 = 0,
    /// Сколько раз владельцы забрали узлы, освобождённые другими потоками.
    collects: u64 = 0,
    /// Кэшей завершившихся потоков, взятых новыми потоками.
    adoptions: u64 = 0,
};

pub fn NodePool(comptime Node: type) type {
    const size = @sizeOf(Node);
    if (size < 2 * @sizeOf(usize) or @alignOf(Node) > 16)
        @compileError("узел пула: не меньше двух слов, выравнивание не больше 16");
    // Узлы нарезаются с шагом size от выровненной плиты.
    const stride = std.mem.alignForward(usize, size, @alignOf(Node));
    const per_slab = slab_bytes / stride - 1; // первый — заголовок

    return struct {
        const Word = ?*anyopaque;

        // Своя строка кэша (128 байт на Apple M): кэши разных потоков
        // меняются на каждой операции, и два в одной строке гоняли бы её
        // между ядрами. Стек освобождений пишут другие потоки — у него тоже
        // своя строка. `extern`: порядок полей как написан — иначе Zig
        // переставлял их, и `nfree` уходил в строку стека освобождений
        // (каждое освобождение трогало две строки, `mixed_mut` +7%).
        const Cache = extern struct {
            free: std.atomic.Value(?[*]u8) align(128) = .init(null), // список, LIFO
            nfree: std.atomic.Value(u32) = .init(0),
            cur: std.atomic.Value(?[*]u8) = .init(null), // нарезка
            end: std.atomic.Value(?[*]u8) = .init(null),
            // Чужие узлы, освобождённые здесь, — пачкой одному владельцу.
            out_owner: ?*Cache = null,
            out_head: ?[*]u8 = null,
            out_tail: ?[*]u8 = null,
            nout: std.atomic.Value(u32) = .init(0),
            collects: std.atomic.Value(u64) = .init(0),
            next: ?*Cache = null, // реестр; под блокировкой
            orphan: ?*Cache = null, // следующая сирота; под блокировкой
            // Узлы плит кэша, освобождённые другими потоками.
            remote: std.atomic.Value(?[*]u8) align(128) = .init(null),
        };

        var lock: std.atomic.Value(bool) = .init(false);
        // Под блокировкой:
        var slabs: ?[*]u8 = null;
        var nslabs: usize = 0;
        var abandoned: usize = 0; // ненарезанные узлы плит, оставленных reserve
        var registry: ?*Cache = null; // все кэши, сироты тоже
        var orphans: ?*Cache = null;
        var counters: Stats = .{};
        var backing: ?Allocator = null;

        threadlocal var cache: ?*Cache = null;

        fn word(node: [*]u8, i: usize) *Word {
            const words: [*]Word = @ptrCast(@alignCast(node));
            return &words[i];
        }

        fn link(node: [*]u8) ?[*]u8 {
            return @ptrCast(word(node, 0).*);
        }

        /// Кэш-владелец плиты узла (не меняется, пока плита жива).
        inline fn owner(node: [*]u8) *Cache {
            const slab = node - (@intFromPtr(node) & (slab_bytes - 1));
            return @ptrCast(@alignCast(word(slab, 1).*));
        }

        /// Спин-блокировка test-and-test-and-set с уступкой процессора
        /// (как в C и Rust).
        fn acquire() void {
            while (lock.swap(true, .acquire)) {
                while (lock.load(.monotonic)) std.Thread.yield() catch {};
            }
        }

        fn release() void {
            lock.store(false, .release);
        }

        fn checkBacking(allocator: Allocator) void {
            if (std.debug.runtime_safety) {
                if (backing) |b| std.debug.assert(b.ptr == allocator.ptr and b.vtable == allocator.vtable);
            }
        }

        /// Новая плита во владении c; под блокировкой.
        fn newSlab(allocator: Allocator, c: *Cache) Allocator.Error![*]u8 {
            checkBacking(allocator);
            const slab = try allocator.alignedAlloc(u8, slab_alignment, slab_bytes);
            backing = allocator;
            word(slab.ptr, 0).* = @ptrCast(slabs);
            word(slab.ptr, 1).* = c;
            slabs = slab.ptr;
            nslabs += 1;
            counters.slab_allocs += 1;
            return slab.ptr;
        }

        fn uncarved(c: *Cache) usize {
            const cur = c.cur.load(.monotonic) orelse return 0;
            return (@intFromPtr(c.end.load(.monotonic).?) - @intFromPtr(cur)) / stride;
        }

        /// Цепочку head..last (связана словом 0) — в стек освобождений
        /// владельца.
        fn pushRemote(o: *Cache, head: [*]u8, last: [*]u8) void {
            var top = o.remote.load(.monotonic);
            while (true) {
                word(last, 0).* = @ptrCast(top);
                top = o.remote.cmpxchgWeak(top, head, .release, .monotonic) orelse return;
            }
        }

        /// Отдать накопленную пачку чужих узлов владельцу, если она есть.
        fn flushOut(c: *Cache) void {
            const head = c.out_head orelse return;
            pushRemote(c.out_owner.?, head, c.out_tail.?);
            c.out_head = null;
            c.out_tail = null;
            c.nout.store(0, .monotonic);
        }

        /// Стек освобождений кэша — в начало его списка; false, если пуст.
        fn collect(c: *Cache) bool {
            if (c.remote.load(.monotonic) == null) return false;
            const head = c.remote.swap(null, .acquire).?;
            var last = head;
            var n: u32 = 1;
            while (link(last)) |next| : (n += 1) last = next;
            word(last, 0).* = @ptrCast(c.free.load(.monotonic));
            c.free.store(head, .monotonic);
            c.nfree.store(c.nfree.load(.monotonic) + n, .monotonic);
            c.collects.store(c.collects.load(.monotonic) + 1, .monotonic);
            return true;
        }

        /// Длина стека освобождений: забрать целиком, посчитать, вернуть.
        fn remoteLen(c: *Cache) usize {
            const head = c.remote.swap(null, .acquire) orelse return 0;
            var last = head;
            var n: usize = 1;
            while (link(last)) |next| : (n += 1) last = next;
            pushRemote(c, head, last);
            return n;
        }

        /// Кэш потока: сирота, если есть, иначе новый.
        fn register(allocator: Allocator) ?*Cache {
            acquire();
            if (orphans) |c| {
                orphans = c.orphan;
                counters.adoptions += 1;
                release();
                cache = c;
                return c;
            }
            release();
            const c = allocator.create(Cache) catch return null;
            c.* = .{};
            acquire();
            defer release();
            c.next = registry;
            registry = c;
            cache = c;
            return c;
        }

        fn pop(c: *Cache) [*]u8 {
            const n = c.free.load(.monotonic).?;
            c.free.store(link(n), .monotonic);
            c.nfree.store(c.nfree.load(.monotonic) - 1, .monotonic);
            return n;
        }

        pub inline fn alloc(allocator: Allocator) Allocator.Error!*Node {
            if (cache) |c| {
                if (c.free.load(.monotonic) != null) return @ptrCast(@alignCast(pop(c)));
            }
            return @ptrCast(@alignCast(try allocSlow(allocator)));
        }

        /// Список пуст (или кэша ещё нет): стек освобождений, затем плита
        /// потока, затем новая.
        noinline fn allocSlow(allocator: Allocator) Allocator.Error![*]u8 {
            const c = cache orelse register(allocator) orelse return error.OutOfMemory;
            // У усыновлённой сироты список может быть не пуст.
            if (c.free.load(.monotonic) != null or collect(c)) return pop(c);
            var cur = c.cur.load(.monotonic);
            if (cur == null or cur == c.end.load(.monotonic)) {
                acquire();
                defer release();
                const slab = try newSlab(allocator, c);
                cur = slab + stride;
                c.end.store(cur.? + per_slab * stride, .monotonic);
            }
            c.cur.store(cur.? + stride, .monotonic);
            return cur.?;
        }

        /// Убедиться, что кэш потока выдаст ещё n узлов без отказа: в его
        /// списке и ненарезанном остатке плиты их не меньше n, иначе забрать
        /// стек освобождений или новую плиту сейчас. Операция на месте
        /// вызывает это до изменения очереди (отказать может только это), а
        /// узлы выделяет сам шаг ротации — после своих освобождений, как
        /// персистентные операции и Rust: выделение заранее меняло бы порядок
        /// узлов, а при LIFO-переиспользовании порядок задаёт раскладку
        /// списков.
        pub inline fn reserve(allocator: Allocator, n: u32) Allocator.Error!void {
            if (cache) |c| {
                if (c.nfree.load(.monotonic) >= n) return;
            }
            return reserveSlow(allocator, n);
        }

        noinline fn reserveSlow(allocator: Allocator, n: u32) Allocator.Error!void {
            const c = cache orelse register(allocator) orelse return error.OutOfMemory;
            const left: u32 = @intCast(uncarved(c));
            if (c.nfree.load(.monotonic) + left >= n) return;
            if (collect(c) and c.nfree.load(.monotonic) + left >= n) return;
            acquire();
            defer release();
            const slab = try newSlab(allocator, c);
            abandoned += left; // остаток прежней плиты — до trim
            const cur = slab + stride;
            c.end.store(cur + per_slab * stride, .monotonic);
            c.cur.store(cur, .monotonic);
        }

        pub inline fn free(allocator: Allocator, node: *Node) void {
            const p: [*]u8 = @ptrCast(node);
            const o = owner(p);
            if (cache) |c| {
                if (c == o) {
                    const k = c.nfree.load(.monotonic) + 1;
                    word(p, 0).* = @ptrCast(c.free.load(.monotonic));
                    c.free.store(p, .monotonic);
                    c.nfree.store(k, .monotonic);
                    return;
                }
            }
            freeRemote(allocator, o, p);
        }

        /// То же вызовом — для очередей с атомарными счётчиками: встроенное,
        /// оно меняло встраивание путей освобождения и замедляло их операции
        /// на месте (`mixed_mut 10^6` +19% против +11% у v1, `docs/pool.md`).
        pub noinline fn freeOutlined(allocator: Allocator, node: *Node) void {
            free(allocator, node);
        }

        /// Узел чужой плиты: в пачку потока для его владельца (пачка другому
        /// владельцу уходит первой). Без памяти на кэш — владельцу сразу.
        noinline fn freeRemote(allocator: Allocator, o: *Cache, p: [*]u8) void {
            const c = cache orelse register(allocator) orelse return pushRemote(o, p, p);
            if (c.out_owner != o) {
                flushOut(c);
                c.out_owner = o;
            }
            word(p, 0).* = @ptrCast(c.out_head);
            if (c.out_head == null) c.out_tail = p;
            c.out_head = p;
            const k = c.nout.load(.monotonic) + 1;
            c.nout.store(k, .monotonic);
            if (k == remote_batch) flushOut(c);
        }

        /// Перед выходом потока: пачка чужих узлов — владельцу, кэш — в
        /// сироты (его возьмёт следующий поток; освободит `trim`).
        pub fn flushThreadCache() void {
            const c = cache orelse return;
            flushOut(c);
            acquire();
            c.orphan = orphans;
            orphans = c;
            release();
            cache = null;
        }

        fn freeNodes() usize {
            var n = abandoned;
            var c = registry;
            while (c) |p| : (c = p.next)
                n += p.nfree.load(.monotonic) + p.nout.load(.monotonic) + remoteLen(p) + uncarved(p);
            return n;
        }

        pub fn stats() Stats {
            acquire();
            defer release();
            var s = counters;
            s.slabs = nslabs;
            s.slab_bytes = nslabs * slab_bytes;
            s.capacity = nslabs * per_slab;
            s.free = freeNodes();
            s.live = s.capacity - s.free;
            var c = registry;
            while (c) |p| : (c = p.next) s.collects += p.collects.load(.monotonic);
            return s;
        }

        /// Вернуть все плиты и кэши сирот аллокатору, если живых узлов нет
        /// (все версии этого типа уничтожены); иначе ничего. Возвращает число
        /// байт плит. Другие потоки в это время пулом не пользуются (их кэши
        /// сбрасываются отсюда).
        pub fn trim(allocator: Allocator) usize {
            acquire();
            defer release();
            if (freeNodes() != nslabs * per_slab) return 0;
            if (nslabs == 0 and orphans == null) return 0;
            checkBacking(allocator);
            const bytes = nslabs * slab_bytes;
            var s = slabs;
            while (s) |slab| {
                s = link(slab);
                const memory: []align(slab_bytes) u8 = @alignCast(slab[0..slab_bytes]);
                allocator.free(memory);
                counters.slab_frees += 1;
            }
            slabs = null;
            nslabs = 0;
            abandoned = 0;
            // Сироты больше никому не нужны: плит, которые на них указывают,
            // нет.
            var r: *?*Cache = &registry;
            while (r.*) |p| {
                if (isOrphan(p)) {
                    r.* = p.next;
                    allocator.destroy(p);
                } else {
                    r = &p.next;
                }
            }
            orphans = null;
            // Пул пуст целиком (ни плит, ни кэшей, выделенных у аллокатора) —
            // следующая плита может прийти от другого аллокатора.
            if (registry == null) backing = null;
            var c = registry;
            while (c) |p| : (c = p.next) {
                p.free.store(null, .monotonic);
                p.nfree.store(0, .monotonic);
                p.cur.store(null, .monotonic);
                p.end.store(null, .monotonic);
                p.out_owner = null;
                p.out_head = null;
                p.out_tail = null;
                p.nout.store(0, .monotonic);
                p.remote.store(null, .monotonic);
            }
            return bytes;
        }

        fn isOrphan(c: *Cache) bool {
            var o = orphans;
            while (o) |p| : (o = p.orphan) if (p == c) return true;
            return false;
        }
    };
}
