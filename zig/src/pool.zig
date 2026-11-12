//! Пул узлов (`docs/pool.md`; C и Rust следуют тому же алгоритму с теми же
//! константами). `NodePool(Node)` — один пул на тип узла, то есть на тип
//! очереди: состояние пула — переменные этого типа, кэш потока — в куче за
//! `threadlocal`-указателем.
//!
//! Плиты по 64 КиБ берутся у аллокатора, переданного в операции очереди:
//! пул запоминает его при первой плите и в Debug/ReleaseSafe проверяет, что
//! передаётся тот же (сменить его можно, когда пул пуст целиком: после
//! `flushThreadCache` во всех потоках и `trim`); плиты не возвращаются, пока в пуле есть живой узел, —
//! пул держит пик живого множества до `trim`. У каждого потока — список
//! свободных узлов (LIFO) и плита, которую он нарезает; список длиннее
//! `cache_max` отдаёт `batch` узлов в общий стек пачек под спин-блокировкой,
//! пустой — забирает пачку оттуда до нарезки новой плиты.
//!
//! Деструкторов у `threadlocal` в Zig нет: перед выходом потока, который
//! пользовался пулом, вызывайте `flushThreadCache`; иначе его кэш (до
//! `cache_max` узлов и остаток плиты) удерживается до `trim`.
//!
//! Свободный узел хранит ссылки пула в первых двух словах: слово 0 —
//! следующий узел списка или пачки; в голове пачки слово 1 — следующая
//! пачка; во втором узле пачки слово 1 — число узлов в ней.

const std = @import("std");
const Allocator = std.mem.Allocator;

pub const slab_bytes = 65536;
pub const cache_max = 4096;
pub const batch = 2048;

/// Статистика пула; под конкуренцией — приблизительная.
pub const Stats = struct {
    /// Плит у пула.
    slabs: usize = 0,
    /// Их общий размер, байт.
    slab_bytes: usize = 0,
    /// Узлов в них.
    capacity: usize = 0,
    /// Из них свободных: в кэшах потоков, ненарезанных, в общем стеке.
    free: usize = 0,
    /// Из них живых: capacity - free.
    live: usize = 0,
    /// Плит выделено за всё время (обращения к аллокатору).
    slab_allocs: u64 = 0,
    /// Плит возвращено (`trim`).
    slab_frees: u64 = 0,
    /// Пачек отдано кэшами потоков в общий стек.
    spills: u64 = 0,
    /// Пачек взято из него.
    refills: u64 = 0,
};

pub fn NodePool(comptime Node: type) type {
    const size = @sizeOf(Node);
    if (size < 2 * @sizeOf(usize) or @alignOf(Node) > 16)
        @compileError("узел пула: не меньше двух слов, выравнивание не больше 16");
    // Узлы нарезаются с шагом size от выравненной на 16 плиты.
    const stride = std.mem.alignForward(usize, size, @alignOf(Node));
    const per_slab = slab_bytes / stride - 1; // первый — заголовок

    return struct {
        const Word = ?*anyopaque;

        // Своя строка кэша (128 байт на Apple M): кэши разных потоков
        // меняются на каждой операции, и два в одной строке гоняли бы её
        // между ядрами.
        const Cache = struct {
            free: std.atomic.Value(?[*]u8) align(128) = .init(null), // список, LIFO
            nfree: std.atomic.Value(u32) = .init(0),
            cur: std.atomic.Value(?[*]u8) = .init(null), // нарезка
            end: std.atomic.Value(?[*]u8) = .init(null),
            next: ?*Cache = null, // реестр; под блокировкой
        };

        var lock: std.atomic.Value(bool) = .init(false);
        var batches: std.atomic.Value(?[*]u8) = .init(null); // подсматривается без блокировки
        // Под блокировкой:
        var global: usize = 0;
        var slabs: ?[*]u8 = null;
        var nslabs: usize = 0;
        var abandoned: usize = 0;
        var registry: ?*Cache = null;
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

        /// Спин-блокировка test-and-test-and-set с уступкой процессора
        /// (одинакова в C, Rust и Zig).
        fn acquire() void {
            while (lock.swap(true, .acquire)) {
                while (lock.load(.monotonic)) std.Thread.yield() catch {};
            }
        }

        fn release() void {
            lock.store(false, .release);
        }

        fn pushBatch(head: [*]u8, n: u32) void {
            if (link(head)) |second| word(second, 1).* = @ptrFromInt(n);
            word(head, 1).* = @ptrCast(batches.load(.monotonic));
            batches.store(head, .monotonic);
            global += n;
        }

        fn popBatch() ?struct { head: [*]u8, n: u32 } {
            const head = batches.load(.monotonic) orelse return null;
            const n: u32 = if (link(head)) |second| @intCast(@intFromPtr(word(second, 1).*)) else 1;
            batches.store(@ptrCast(word(head, 1).*), .monotonic);
            global -= n;
            return .{ .head = head, .n = n };
        }

        fn checkBacking(allocator: Allocator) void {
            if (std.debug.runtime_safety) {
                if (backing) |b| std.debug.assert(b.ptr == allocator.ptr and b.vtable == allocator.vtable);
            }
        }

        fn newSlab(allocator: Allocator) Allocator.Error![*]u8 {
            checkBacking(allocator);
            const slab = try allocator.alignedAlloc(u8, .@"16", slab_bytes);
            backing = allocator;
            word(slab.ptr, 0).* = @ptrCast(slabs);
            slabs = slab.ptr;
            nslabs += 1;
            counters.slab_allocs += 1;
            return slab.ptr;
        }

        fn uncarved(c: *Cache) usize {
            const cur = c.cur.load(.monotonic) orelse return 0;
            return (@intFromPtr(c.end.load(.monotonic).?) - @intFromPtr(cur)) / stride;
        }

        /// Весь список кэша — в общий стек пачками; под блокировкой.
        fn flushLocked(c: *Cache) void {
            var node = c.free.load(.monotonic);
            while (node) |head| {
                var last = head;
                var n: u32 = 1;
                while (n < batch) : (n += 1) last = link(last) orelse break;
                node = link(last);
                word(last, 0).* = null;
                pushBatch(head, n);
            }
            c.free.store(null, .monotonic);
            c.nfree.store(0, .monotonic);
        }

        fn register(allocator: Allocator) ?*Cache {
            const c = allocator.create(Cache) catch return null;
            c.* = .{};
            acquire();
            defer release();
            c.next = registry;
            registry = c;
            cache = c;
            return c;
        }

        pub inline fn alloc(allocator: Allocator) Allocator.Error!*Node {
            if (cache) |c| {
                if (c.free.load(.monotonic)) |n| {
                    c.free.store(link(n), .monotonic);
                    c.nfree.store(c.nfree.load(.monotonic) - 1, .monotonic);
                    return @ptrCast(@alignCast(n));
                }
            }
            return @ptrCast(@alignCast(try allocSlow(allocator)));
        }

        /// Список пуст (или кэша ещё нет): пачка из общего стека, затем
        /// плита потока, затем новая.
        noinline fn allocSlow(allocator: Allocator) Allocator.Error![*]u8 {
            const c = cache orelse register(allocator) orelse return error.OutOfMemory;
            if (batches.load(.monotonic) != null) {
                acquire();
                if (popBatch()) |b| {
                    counters.refills += 1;
                    release();
                    c.free.store(link(b.head), .monotonic);
                    c.nfree.store(b.n - 1, .monotonic);
                    return b.head;
                }
                release();
            }
            var cur = c.cur.load(.monotonic);
            if (cur == null or cur == c.end.load(.monotonic)) {
                acquire();
                defer release();
                const slab = try newSlab(allocator);
                cur = slab + stride;
                c.end.store(cur.? + per_slab * stride, .monotonic);
            }
            c.cur.store(cur.? + stride, .monotonic);
            return cur.?;
        }

        pub inline fn free(allocator: Allocator, node: *Node) void {
            const p: [*]u8 = @ptrCast(node);
            if (cache) |c| {
                const k = c.nfree.load(.monotonic) + 1;
                word(p, 0).* = @ptrCast(c.free.load(.monotonic));
                c.free.store(p, .monotonic);
                c.nfree.store(k, .monotonic);
                if (k <= cache_max) return;
            }
            freeSlow(allocator, p);
        }

        /// Кэша нет — создать и положить узел (без памяти на кэш — узел в
        /// общий стек один); иначе список превысил cache_max (узел уже в
        /// нём) — batch узлов с вершины в общий стек.
        noinline fn freeSlow(allocator: Allocator, p: [*]u8) void {
            const c = cache orelse {
                word(p, 0).* = null;
                const c = register(allocator) orelse {
                    acquire();
                    defer release();
                    pushBatch(p, 1);
                    return;
                };
                c.free.store(p, .monotonic);
                c.nfree.store(1, .monotonic);
                return;
            };
            const head = c.free.load(.monotonic).?;
            var last = head;
            for (1..batch) |_| last = link(last).?;
            c.free.store(link(last), .monotonic);
            word(last, 0).* = null;
            c.nfree.store(c.nfree.load(.monotonic) - batch, .monotonic);
            acquire();
            defer release();
            pushBatch(head, batch);
            counters.spills += 1;
        }

        /// Сбросить кэш этого потока в пул (перед выходом потока): свободные
        /// узлы — в общий стек, остаток плиты — до `trim`.
        pub fn flushThreadCache() void {
            const c = cache orelse return;
            acquire();
            flushLocked(c);
            abandoned += uncarved(c);
            var r: *?*Cache = &registry;
            while (r.*.? != c) r = &r.*.?.next;
            r.* = c.next;
            const b = backing.?;
            release();
            cache = null;
            b.destroy(c);
        }

        fn freeNodes() usize {
            var n = global + abandoned;
            var c = registry;
            while (c) |p| : (c = p.next) n += p.nfree.load(.monotonic) + uncarved(p);
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
            return s;
        }

        /// Вернуть все плиты аллокатору, если живых узлов нет (все версии
        /// этого типа уничтожены); иначе ничего. Возвращает число байт. Другие
        /// потоки в это время пулом не пользуются (их кэши сбрасываются
        /// отсюда).
        pub fn trim(allocator: Allocator) usize {
            acquire();
            defer release();
            if (nslabs == 0 or freeNodes() != nslabs * per_slab) return 0;
            checkBacking(allocator);
            const bytes = nslabs * slab_bytes;
            var s = slabs;
            while (s) |slab| {
                s = link(slab);
                const memory: []align(16) u8 = @alignCast(slab[0..slab_bytes]);
                allocator.free(memory);
                counters.slab_frees += 1;
            }
            slabs = null;
            nslabs = 0;
            global = 0;
            abandoned = 0;
            batches.store(null, .monotonic);
            // Пул пуст целиком (ни плит, ни кэшей, выделенных у аллокатора) —
            // следующая плита может прийти от другого аллокатора.
            if (registry == null) backing = null;
            var c = registry;
            while (c) |p| : (c = p.next) {
                p.free.store(null, .monotonic);
                p.nfree.store(0, .monotonic);
                p.cur.store(null, .monotonic);
                p.end.store(null, .monotonic);
            }
            return bytes;
        }
    };
}
