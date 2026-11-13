const std = @import("std");
const Allocator = std.mem.Allocator;

/// Значения T копируются присваиванием, как в C-шаблоне. Ресурсами внутри T
/// владеет вызывающий. Очередь не потокобезопасна; все версии одного графа
/// должны использовать один и тот же аллокатор. Копия по значению — перенос
/// владения; для независимой версии нужен clone.
pub fn Queue(comptime T: type) type {
    return QueueWithOptions(T, .{});
}

/// Потокобезопасные узлы: неизменяемую структуру версии можно читать и
/// порождать из неё версии в разных потоках. pushInPlace/popInPlace/deinit
/// требуют единоличного владения самой структурой; узлы могут разделяться.
/// T должен допускать параллельное чтение. Копия значения переносит владение;
/// для независимого владения нужен clone; аллокатор общий и потокобезопасный.
/// Предел ссылок — половина диапазона u32, как у Arc в Rust: запас 2³¹ до
/// переполнения перекрывает любое число одновременно работающих потоков.
pub fn SyncQueue(comptime T: type) type {
    return QueueWithOptions(T, .{ .atomic = true, .refs_limit = std.math.maxInt(u32) / 2 });
}

/// Очередь с узлами из пула (`docs/pool.md`): тот же API, узлы берутся из
/// пула этого типа (плиты по 16 КиБ у переданного аллокатора, кэш у каждого
/// потока) и возвращаются туда при обнулении счётчика. Аллокатор операций —
/// источник плит: во все операции передавайте один и тот же. Пул держит
/// память на пике живого множества до `node_pool.trim`; перед выходом потока —
/// `node_pool.flushThreadCache()`.
pub fn PooledQueue(comptime T: type) type {
    return QueueWithOptions(T, .{ .nodes = .pool });
}

/// SyncQueue с узлами из пула.
pub fn SyncPooledQueue(comptime T: type) type {
    return QueueWithOptions(T, .{ .atomic = true, .refs_limit = std.math.maxInt(u32) / 2, .nodes = .pool });
}

/// Статистика пула узлов (`Q.node_pool.stats()`).
pub const PoolStats = @import("pool.zig").Stats;

/// Настройки очереди.
pub const Options = struct {
    atomic: bool = false,
    /// Предел владеющих ссылок на узел. Превысить его — недопустимое
    /// поведение (паника в Debug и ReleaseSafe): это миллиарды ссылок на один
    /// узел, т.е. утечка версий. Меньший предел — для проверки этой паники.
    refs_limit: u32 = std.math.maxInt(u32),
    /// Где живут узлы: по одному у аллокатора или в пуле (`PooledQueue`).
    nodes: enum { heap, pool } = .heap,
};

/// Очередь с заданными настройками.
pub fn QueueWithOptions(comptime T: type, comptime options: Options) type {
    return struct {
        const Self = @This();
        const Node = struct {
            value: T,
            refs: if (options.atomic) std.atomic.Value(u32) else u32 = if (options.atomic) .init(1) else 1,
            next: ?*@This(),
        };

        /// Ошибки выделения памяти и переполнения длины (`push`).
        pub const Error = Allocator.Error || error{Overflow};

        const NodePool = @import("pool.zig").NodePool(Node);

        /// Пул узлов этого типа (только `.nodes = .pool`): статистика, `trim`
        /// (вернуть плиты аллокатору, если живых узлов нет) и сброс кэша
        /// потока перед его выходом.
        pub const node_pool = if (options.nodes == .pool) struct {
            pub const stats = NodePool.stats;
            pub const trim = NodePool.trim;
            pub const flushThreadCache = NodePool.flushThreadCache;
        } else struct {};

        inline fn newNode(allocator: Allocator) Allocator.Error!*Node {
            return if (options.nodes == .pool) NodePool.alloc(allocator) else allocator.create(Node);
        }

        inline fn freeNode(allocator: Allocator, n: *Node) void {
            if (options.nodes != .pool) return allocator.destroy(n);
            // С атомарными счётчиками — вызовом: встроенное освобождение
            // пула замедляет их операции (pool.zig, freeOutlined).
            if (options.atomic) NodePool.freeOutlined(allocator, n) else NodePool.free(allocator, n);
        }
        // Фаза ротации кодируется самими указателями, без тега (как ниша в
        // Rust): разворот идёт, пока непуст old_rear; дописывание — old_rear
        // исчерпан, acc непуст (в нём уже есть тыл); покой — все пусты. Вне
        // своей фазы поля пусты, поэтому deinit/clone обходят их без разбора.
        // Версия — 64 байта, одна строка кэша (с тегом было 72).
        const Rotation = struct {
            old_front: ?*Node = null,
            old_rear: ?*Node = null,
            rfront: ?*Node = null,
            acc: ?*Node = null,

            const Phase = enum { idle, reversing, appending };

            fn phase(r: Rotation) Phase {
                if (r.old_rear != null) return .reversing;
                return if (r.acc != null) .appending else .idle;
            }
        };
        size: u32 = 0,
        front_len: u32 = 0,
        rear_len: u32 = 0,
        ok: u32 = 0,
        front_stack: ?*Node = null,
        rear: ?*Node = null,
        rot: Rotation = .{},

        /// Создать пустую очередь без выделения памяти.
        pub fn init() Self {
            return .{};
        }

        /// Число элементов очереди.
        pub fn len(self: Self) u32 {
            return self.size;
        }

        /// Проверить, пуста ли очередь.
        pub fn isEmpty(self: Self) bool {
            return self.size == 0;
        }

        /// Первое значение без изменения версии.
        pub fn front(self: Self) ?T {
            return if (self.front_stack) |n| n.value else null;
        }

        fn refCount(n: *Node, comptime order: std.builtin.AtomicOrder) u32 {
            return if (options.atomic) n.refs.load(order) else n.refs;
        }

        // Предел ссылок (Options.refs_limit): превышение — недопустимое
        // поведение. Atomic проверяет уже сделанный инкремент, как Arc: под
        // гонками счётчик может уйти выше, но до переполнения u32 ещё 2³¹.
        fn retain(n: ?*Node) void {
            if (n) |p| {
                if (options.atomic) {
                    // Как Arc: существующая ссылка обеспечивает жизнь узла,
                    // поэтому приобретению новой достаточно relaxed.
                    const old = p.refs.fetchAdd(1, .monotonic);
                    std.debug.assert(old < options.refs_limit);
                } else {
                    std.debug.assert(p.refs < options.refs_limit);
                    p.refs += 1;
                }
            }
        }

        fn touch(n: ?*Node) void {
            if (n) |p| {
                if (options.atomic) {
                    _ = p.refs.load(.monotonic);
                } else {
                    const refs: *volatile u32 = &p.refs;
                    _ = refs.*;
                }
            }
        }

        fn dropRef(n: *Node) bool {
            if (options.atomic) {
                if (n.refs.fetchSub(1, .release) != 1) return false;
                // Как Arc: release публикует предшествующие обращения, а
                // acquire перед чтением полей/освобождением видит все drop.
                _ = n.refs.load(.acquire);
                return true;
            }
            n.refs -= 1;
            return n.refs == 0;
        }

        fn release(allocator: Allocator, head: ?*Node) void {
            var cur = head;
            while (cur) |n| {
                if (!dropRef(n)) break;
                cur = n.next;
                touch(cur);
                freeNode(allocator, n);
            }
        }

        // cons забирает ссылку на хвост лишь после успешного выделения.
        fn prepend(allocator: Allocator, dst: *?*Node, value: T, pool: ?*Pool) Allocator.Error!void {
            const n = if (pool) |p| p.take() else try newNode(allocator);
            n.* = .{ .value = value, .next = dst.* };
            dst.* = n;
        }

        fn advance(allocator: Allocator, dst: *?*Node) void {
            const n = dst.* orelse return;
            // Acquire наблюдает предыдущих владельцев перед чтением полей.
            const unique = refCount(n, .acquire) == 1;
            const next = n.next;
            if (unique) {
                touch(next);
                freeNode(allocator, n);
            } else {
                retain(next);
                if (options.atomic) release(allocator, n) else n.refs -= 1;
            }
            dst.* = next;
        }

        /// Освободить ссылки версии; после вызова значение не определено.
        pub fn deinit(self: *Self, allocator: Allocator) void {
            release(allocator, self.front_stack);
            release(allocator, self.rear);
            release(allocator, self.rot.old_front);
            release(allocator, self.rot.old_rear);
            release(allocator, self.rot.rfront);
            release(allocator, self.rot.acc);
            self.* = undefined;
        }

        // inline: без него push/pop материализуют на стеке копии аргумента и
        // результата clone (кадр 288 байт против 160); persistent −9%.
        /// Приобрести ссылки на узлы для независимой версии за O(1).
        pub inline fn clone(self: Self) Self {
            const r = self.rot;
            for ([6]?*Node{ self.front_stack, self.rear, r.old_front, r.old_rear, r.rfront, r.acc }) |n| retain(n);
            return self;
        }

        fn start(self: *Self) void {
            retain(self.front_stack);
            std.debug.assert(self.rear != null);
            self.rot = .{ .old_front = self.front_stack, .old_rear = self.rear };
            self.ok = self.front_len;
            self.rear = null;
            self.rear_len = 0;
        }

        fn finish(self: *Self, allocator: Allocator) void {
            const s = self.rot;
            std.debug.assert(s.phase() == .appending);
            release(allocator, self.front_stack);
            release(allocator, s.rfront);
            self.front_stack = s.acc;
            self.front_len = self.size - self.rear_len;
            self.rot = .{};
            self.ok = 0;
        }

        inline fn step(self: *Self, allocator: Allocator, pool: ?*Pool) Allocator.Error!void {
            const s = &self.rot;
            switch (s.phase()) {
                .idle => {},
                .reversing => {
                    try prepend(allocator, &s.acc, s.old_rear.?.value, pool);
                    advance(allocator, &s.old_rear);
                    if (s.old_front) |n| {
                        try prepend(allocator, &s.rfront, n.value, pool);
                        advance(allocator, &s.old_front);
                    }
                    // Исчерпанный old_rear сам переводит фазу в дописывание.
                    if (s.old_rear == null) {
                        std.debug.assert(s.old_front == null);
                        if (self.ok == 0) self.finish(allocator);
                    }
                },
                .appending => {
                    try prepend(allocator, &s.acc, s.rfront.?.value, pool);
                    advance(allocator, &s.rfront);
                    self.ok -= 1;
                    if (self.ok == 0) self.finish(allocator);
                },
            }
        }

        /// Новая версия с элементом в конце; исходная версия цела и при ошибке.
        pub fn push(self: Self, allocator: Allocator, value: T) Error!Self {
            if (self.size == std.math.maxInt(u32)) return error.Overflow;
            var q = self.clone();
            errdefer q.deinit(allocator);
            try prepend(allocator, &q.rear, value, null);
            q.size += 1;
            q.rear_len += 1;
            if (q.rot.phase() == .idle and q.rear_len > q.front_len) q.start();
            try q.step(allocator, null);
            return q;
        }

        /// Новая версия без головы, либо null; исходная версия не меняется.
        pub fn pop(self: Self, allocator: Allocator) Allocator.Error!?Self {
            if (self.isEmpty()) return null;
            var q = self.clone();
            errdefer q.deinit(allocator);
            advance(allocator, &q.front_stack);
            q.size -= 1;
            q.front_len -= 1;
            const phase = q.rot.phase();
            if (phase != .idle) {
                q.ok -= 1;
                if (phase == .appending and q.ok == 0) q.finish(allocator);
            } else if (q.rear_len > q.front_len) q.start();
            try q.step(allocator, null);
            return q;
        }

        // Пул хранит сырую память: ссылками на хвосты он ещё не владеет.
        const Pool = struct {
            nodes: [3]*Node = undefined,
            count: usize = 0,

            fn init(allocator: Allocator, count: usize) Allocator.Error!Pool {
                var pool: Pool = .{};
                errdefer pool.deinit(allocator);
                while (pool.count < count) : (pool.count += 1)
                    pool.nodes[pool.count] = try newNode(allocator);
                return pool;
            }

            fn take(self: *Pool) *Node {
                std.debug.assert(self.count > 0);
                self.count -= 1;
                return self.nodes[self.count];
            }

            fn deinit(self: *Pool, allocator: Allocator) void {
                while (self.count > 0) freeNode(allocator, self.take());
                self.* = undefined;
            }
        };

        // Ровно один шаг модели Rust: переход Reversing не переносит rfront.
        // phase передаётся явно: при начале ротации old_rear ещё не известен.
        fn stepAllocs(phase: Rotation.Phase, old_front: ?*Node, ok: u32) usize {
            return switch (phase) {
                .idle => 0,
                .reversing => 1 + @as(usize, @intFromBool(old_front != null)),
                .appending => @intFromBool(ok != 0),
            };
        }

        /// Добавить на месте. Все возможные ошибки проверяются до изменения;
        /// другие версии сохраняют свои узлы и содержимое.
        pub fn pushInPlace(self: *Self, allocator: Allocator, value: T) Error!void {
            if (self.size == std.math.maxInt(u32)) return error.Overflow;
            const phase = self.rot.phase();
            const starting = phase == .idle and self.rear_len + 1 > self.front_len;
            // Как в C: тыл выделяется первым, пул — только шаг ротации. Порядок
            // выделений задаёт порядок адресов узлов в списках: с тылом из
            // пула (последним выделенным) mixed_mut 10^7 был вдвое медленнее.
            // При отказе пула ссылка на прежний тыл остаётся у очереди.
            const rear = try newNode(allocator);
            errdefer freeNode(allocator, rear);
            const old_front = if (starting) self.front_stack else self.rot.old_front;
            const allocs = stepAllocs(if (starting) .reversing else phase, old_front, self.ok);
            // С пулом узлов — резерв без выделения: узлы выделит сам шаг,
            // после своих освобождений (pool.zig, reserve).
            if (options.nodes == .pool) try NodePool.reserve(allocator, @intCast(allocs));
            var pool = try Pool.init(allocator, if (options.nodes == .pool) 0 else allocs);
            defer pool.deinit(allocator);

            // После подготовки пула ошибок нет.
            rear.* = .{ .value = value, .next = self.rear };
            self.rear = rear;
            self.size += 1;
            self.rear_len += 1;
            if (starting) self.start();
            self.step(allocator, if (options.nodes == .pool) null else &pool) catch unreachable;
            std.debug.assert(pool.count == 0);
        }

        /// Снять и вернуть голову на месте; пустая очередь даёт null.
        /// При ошибке версия и её ссылки остаются нетронутыми.
        pub fn popInPlace(self: *Self, allocator: Allocator) Allocator.Error!?T {
            const value = self.front() orelse return null;
            const phase = self.rot.phase();
            const starting = phase == .idle and self.rear_len > self.front_len - 1;
            const old_front = if (starting) self.front_stack.?.next else self.rot.old_front;
            const ok = if (phase == .idle) self.front_len - 1 else self.ok - 1;
            const allocs = stepAllocs(if (starting) .reversing else phase, old_front, ok);
            if (options.nodes == .pool) try NodePool.reserve(allocator, @intCast(allocs));
            var pool = try Pool.init(allocator, if (options.nodes == .pool) 0 else allocs);
            defer pool.deinit(allocator);

            advance(allocator, &self.front_stack);
            self.size -= 1;
            self.front_len -= 1;
            if (phase != .idle) {
                self.ok -= 1;
                if (phase == .appending and self.ok == 0) self.finish(allocator);
            } else if (starting) self.start();
            self.step(allocator, if (options.nodes == .pool) null else &pool) catch unreachable;
            std.debug.assert(pool.count == 0);
            return value;
        }

        /// Итератор владеет отдельной версией и требует deinit.
        pub const Iterator = struct {
            rest: Self,

            /// Выдать следующий элемент; при ошибке позиция не меняется.
            pub fn next(it: *@This(), allocator: Allocator) Allocator.Error!?T {
                return it.rest.popInPlace(allocator);
            }

            /// Освободить ссылки версии; после вызова значение не определено.
            pub fn deinit(it: *@This(), allocator: Allocator) void {
                it.rest.deinit(allocator);
            }
        };

        /// Создать итератор по отдельной O(1)-копии очереди.
        pub fn iterator(self: Self) Iterator {
            return .{ .rest = self.clone() };
        }
    };
}

test {
    _ = @import("tests.zig");
}

fn InternalSuite(comptime atomic: bool) type {
    return struct {
        // размер узла, перенос хвоста и переполнение длины; переполнение
        // счётчика ссылок — недопустимое поведение, его ловит
        // tests/refs_overflow.zig (`zig build test`)
        fn nodeLayoutAndOverflow() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic });
            const a = std.testing.allocator;
            try std.testing.expectEqual(@as(usize, 16), @sizeOf(Q.Node));
            try std.testing.expectEqual(@as(usize, 64), @sizeOf(Q));
            var head: ?*Q.Node = null;
            defer Q.release(a, head);
            try Q.prepend(a, &head, 1, null);
            const tail = head.?;
            try Q.prepend(a, &head, 2, null);
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(tail, .monotonic));
            Q.advance(a, &head);
            try std.testing.expect(head == tail);
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(tail, .monotonic));
            var q = try Q.init().push(a, 7);
            defer q.deinit(a);
            q.size = std.math.maxInt(u32);
            try std.testing.expectError(error.Overflow, q.push(a, 8));
            try std.testing.expectError(error.Overflow, q.pushInPlace(a, 8));
            q.size = 1;
        }

        // ровно до предела ссылок всё работает: малый предел, все копии живы
        fn upToRefsLimit() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = 8 });
            const a = std.testing.allocator;
            var q = try Q.init().push(a, 10);
            defer q.deinit(a);
            var copies: [7]Q = undefined;
            for (&copies) |*copy| copy.* = q.clone();
            defer for (&copies) |*copy| copy.deinit(a);
            try std.testing.expectEqual(@as(u32, 8), Q.refCount(q.front_stack.?, .monotonic));
            // Узлы общие: операции копируют, а не берут новые ссылки на голову.
            try q.pushInPlace(a, 20);
            try std.testing.expectEqual(@as(?i32, 10), try q.popInPlace(a));
            try std.testing.expectEqual(@as(?i32, 20), q.front());
            for (copies) |copy| try std.testing.expectEqual(@as(?i32, 10), copy.front());
        }

        // быстрый pop не клонирует версию и не выделяет узлы без ротации
        fn allocationFreePop() !void {
            const a = std.testing.allocator;
            var q = QueueWithOptions(i32, .{ .atomic = atomic }).init();
            defer q.deinit(a);
            try q.pushInPlace(a, 1);
            var failing = std.testing.FailingAllocator.init(a, .{ .fail_index = 0 });
            try std.testing.expectEqual(@as(?i32, 1), try q.popInPlace(failing.allocator()));
            try std.testing.expectEqual(@as(usize, 0), failing.alloc_index);
            try std.testing.expect(q.isEmpty());
        }
    };
}
test "обе реализации" {
    inline for (.{ false, true }) |atomic| {
        try InternalSuite(atomic).nodeLayoutAndOverflow();
        try InternalSuite(atomic).upToRefsLimit();
        try InternalSuite(atomic).allocationFreePop();
    }
}
