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
/// Предел ссылок мягкий: clone и персистентные операции возвращают Overflow
/// и откатывают приобретённые ссылки. Быстрые операции на месте могут немного
/// превысить предел под гонками; запас 2³¹ до переполнения достаточен для
/// любого реального числа одновременно работающих потоков.
pub fn SyncQueue(comptime T: type) type {
    return QueueWithOptions(T, .{ .atomic = true, .refs_limit = std.math.maxInt(u32) / 2 });
}

/// Очередь с узлами из пула (`docs/pool.md`): тот же API, узлы берутся из
/// пула этого типа (плиты по 64 КиБ у переданного аллокатора, кэш у каждого
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

/// Настройки предела ссылок, в том числе для проверки переполнения.
pub const Options = struct {
    atomic: bool = false,
    /// Предел владеющих ссылок; для atomic мягкий (см. SyncQueue).
    refs_limit: u32 = std.math.maxInt(u32),
    /// Где живут узлы: по одному у аллокатора или в пуле (`PooledQueue`).
    nodes: enum { heap, pool } = .heap,
};

/// Очередь с заданным пределом счётчика ссылок (не меньше трёх).
pub fn QueueWithOptions(comptime T: type, comptime options: Options) type {
    if (options.refs_limit < 3) @compileError("refs_limit must be at least 3");
    return struct {
        const Self = @This();
        const Node = struct {
            value: T,
            refs: if (options.atomic) std.atomic.Value(u32) else u32 = if (options.atomic) .init(1) else 1,
            next: ?*@This(),
        };

        /// Ошибки выделения памяти и переполнения длины или ссылок.
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
            if (options.nodes == .pool) NodePool.free(allocator, n) else allocator.destroy(n);
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

        fn setRefs(n: *Node, refs: u32) void {
            if (options.atomic) n.refs.store(refs, .monotonic) else n.refs = refs;
        }

        fn retainMode(n: ?*Node, comptime unchecked: bool) Error!void {
            if (n) |p| {
                if (options.atomic) {
                    // Как Arc: существующая ссылка обеспечивает жизнь узла,
                    // поэтому приобретению новой достаточно relaxed.
                    const old = p.refs.fetchAdd(1, .monotonic);
                    if (!unchecked and old >= options.refs_limit) {
                        _ = dropRef(p);
                        return error.Overflow;
                    }
                } else {
                    if (p.refs == options.refs_limit) return error.Overflow;
                    p.refs += 1;
                }
            }
        }

        fn retain(n: ?*Node) Error!void {
            return retainMode(n, false);
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
        fn prepend(allocator: Allocator, dst: *?*Node, value: T, pool: ?*Pool) Error!void {
            const n = if (pool) |p| p.take() else try newNode(allocator);
            n.* = .{ .value = value, .next = dst.* };
            dst.* = n;
        }

        fn advanceMode(allocator: Allocator, dst: *?*Node, comptime unchecked: bool) Error!void {
            const n = dst.* orelse return;
            // Acquire наблюдает предыдущих владельцев перед чтением полей.
            const unique = refCount(n, .acquire) == 1;
            const next = n.next;
            if (unique) {
                touch(next);
                freeNode(allocator, n);
            } else {
                // При переполнении ссылка остаётся у исходного поля.
                try retainMode(next, unchecked);
                if (options.atomic) release(allocator, n) else n.refs -= 1;
            }
            dst.* = next;
        }

        fn advance(allocator: Allocator, dst: *?*Node) Error!void {
            return advanceMode(allocator, dst, false);
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
        pub inline fn clone(self: Self) Error!Self {
            // Поэтапное приобретение ссылок позволяет откатить даже алиасы
            // между полями, не оставляя временного переполнения счётчика.
            const r = self.rot;
            const nodes = [6]?*Node{ self.front_stack, self.rear, r.old_front, r.old_rear, r.rfront, r.acc };
            var acquired: usize = 0;
            errdefer for (nodes[0..acquired]) |n| {
                if (n) |p| {
                    _ = dropRef(p); // Исходная версия всё ещё владеет ссылкой.
                }
            };
            for (nodes) |n| {
                try retain(n);
                acquired += 1;
            }
            return self;
        }

        fn start(self: *Self, comptime unchecked: bool) Error!void {
            try retainMode(self.front_stack, unchecked);
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

        inline fn step(self: *Self, allocator: Allocator, pool: ?*Pool, comptime unchecked: bool) Error!void {
            const s = &self.rot;
            switch (s.phase()) {
                .idle => {},
                .reversing => {
                    try prepend(allocator, &s.acc, s.old_rear.?.value, pool);
                    try advanceMode(allocator, &s.old_rear, unchecked);
                    if (s.old_front) |n| {
                        try prepend(allocator, &s.rfront, n.value, pool);
                        try advanceMode(allocator, &s.old_front, unchecked);
                    }
                    // Исчерпанный old_rear сам переводит фазу в дописывание.
                    if (s.old_rear == null) {
                        std.debug.assert(s.old_front == null);
                        if (self.ok == 0) self.finish(allocator);
                    }
                },
                .appending => {
                    try prepend(allocator, &s.acc, s.rfront.?.value, pool);
                    try advanceMode(allocator, &s.rfront, unchecked);
                    self.ok -= 1;
                    if (self.ok == 0) self.finish(allocator);
                },
            }
        }

        /// Новая версия с элементом в конце; исходная версия цела и при ошибке.
        pub fn push(self: Self, allocator: Allocator, value: T) Error!Self {
            if (self.size == std.math.maxInt(u32)) return error.Overflow;
            var q = try self.clone();
            errdefer q.deinit(allocator);
            try prepend(allocator, &q.rear, value, null);
            q.size += 1;
            q.rear_len += 1;
            if (q.rot.phase() == .idle and q.rear_len > q.front_len) try q.start(false);
            try q.step(allocator, null, false);
            return q;
        }

        /// Новая версия без головы, либо null; исходная версия не меняется.
        pub fn pop(self: Self, allocator: Allocator) Error!?Self {
            if (self.isEmpty()) return null;
            var q = try self.clone();
            errdefer q.deinit(allocator);
            try advance(allocator, &q.front_stack);
            q.size -= 1;
            q.front_len -= 1;
            const phase = q.rot.phase();
            if (phase != .idle) {
                q.ok -= 1;
                if (phase == .appending and q.ok == 0) q.finish(allocator);
            } else if (q.rear_len > q.front_len) try q.start(false);
            try q.step(allocator, null, false);
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

        fn nextNearLimit(node: ?*Node) bool {
            const n = node orelse return false;
            const next = n.next orelse return false;
            return refCount(next, .monotonic) > options.refs_limit - 2;
        }

        // За операцию один узел получает не больше двух новых ссылок:
        // advance живого фронта, затем start или advance снимка фронта.
        // Проверка консервативна; вблизи предела точный откат даёт slow path.
        fn nearLimit(self: Self) bool {
            if (self.front_stack) |n| {
                if (refCount(n, .monotonic) > options.refs_limit - 2 or nextNearLimit(n) or nextNearLimit(n.next)) return true;
            }
            if (nextNearLimit(self.rear)) return true;
            // Вне своей фазы поля пусты: проверка не зависит от фазы.
            const s = self.rot;
            return nextNearLimit(s.old_front) or nextNearLimit(s.old_rear) or nextNearLimit(s.rfront);
        }

        /// Добавить на месте. Все возможные ошибки проверяются до изменения;
        /// другие версии сохраняют свои узлы и содержимое.
        pub fn pushInPlace(self: *Self, allocator: Allocator, value: T) Error!void {
            if (self.size == std.math.maxInt(u32)) return error.Overflow;
            if (self.nearLimit()) {
                const next = try self.push(allocator, value);
                self.deinit(allocator);
                self.* = next;
                return;
            }
            const phase = self.rot.phase();
            const starting = phase == .idle and self.rear_len + 1 > self.front_len;
            // Как в C: тыл выделяется первым, пул — только шаг ротации. Порядок
            // выделений задаёт порядок адресов узлов в списках: с тылом из
            // пула (последним выделенным) mixed_mut 10^7 был вдвое медленнее.
            // При отказе пула ссылка на прежний тыл остаётся у очереди.
            const rear = try newNode(allocator);
            errdefer freeNode(allocator, rear);
            const old_front = if (starting) self.front_stack else self.rot.old_front;
            var pool = try Pool.init(allocator, stepAllocs(if (starting) .reversing else phase, old_front, self.ok));
            defer pool.deinit(allocator);

            // После подготовки пула ошибок нет. В atomic режиме инкременты
            // неотказывающие: предпроверка предела не защищает от гонок.
            rear.* = .{ .value = value, .next = self.rear };
            self.rear = rear;
            self.size += 1;
            self.rear_len += 1;
            if (starting) self.start(options.atomic) catch unreachable;
            self.step(allocator, &pool, options.atomic) catch unreachable;
            std.debug.assert(pool.count == 0);
        }

        /// Снять и вернуть голову на месте; пустая очередь даёт null.
        /// При ошибке версия и её ссылки остаются нетронутыми.
        pub fn popInPlace(self: *Self, allocator: Allocator) Error!?T {
            const value = self.front() orelse return null;
            if (self.nearLimit()) {
                const next = (try self.pop(allocator)).?;
                self.deinit(allocator);
                self.* = next;
                return value;
            }
            const phase = self.rot.phase();
            const starting = phase == .idle and self.rear_len > self.front_len - 1;
            const old_front = if (starting) self.front_stack.?.next else self.rot.old_front;
            const ok = if (phase == .idle) self.front_len - 1 else self.ok - 1;
            var pool = try Pool.init(allocator, stepAllocs(if (starting) .reversing else phase, old_front, ok));
            defer pool.deinit(allocator);

            advanceMode(allocator, &self.front_stack, options.atomic) catch unreachable;
            self.size -= 1;
            self.front_len -= 1;
            if (phase != .idle) {
                self.ok -= 1;
                if (phase == .appending and self.ok == 0) self.finish(allocator);
            } else if (starting) self.start(options.atomic) catch unreachable;
            self.step(allocator, &pool, options.atomic) catch unreachable;
            std.debug.assert(pool.count == 0);
            return value;
        }

        /// Итератор владеет отдельной версией и требует deinit.
        pub const Iterator = struct {
            rest: Self,

            /// Выдать следующий элемент; при ошибке позиция не меняется.
            pub fn next(it: *@This(), allocator: Allocator) Error!?T {
                return it.rest.popInPlace(allocator);
            }

            /// Освободить ссылки версии; после вызова значение не определено.
            pub fn deinit(it: *@This(), allocator: Allocator) void {
                it.rest.deinit(allocator);
            }
        };

        /// Создать итератор по отдельной O(1)-копии очереди.
        pub fn iterator(self: Self) Error!Iterator {
            return .{ .rest = try self.clone() };
        }
    };
}

test {
    _ = @import("tests.zig");
}

fn InternalSuite(comptime atomic: bool) type {
    return struct {
        const limit = if (atomic) std.math.maxInt(u32) / 2 else std.math.maxInt(u32);
        // размер узла, перенос хвоста и переполнение
        fn nodeLayoutAndOverflow() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = limit });
            const a = std.testing.allocator;
            try std.testing.expectEqual(@as(usize, 16), @sizeOf(Q.Node));
            try std.testing.expectEqual(@as(usize, 64), @sizeOf(Q));
            var head: ?*Q.Node = null;
            defer Q.release(a, head);
            try Q.prepend(a, &head, 1, null);
            const tail = head.?;
            try Q.prepend(a, &head, 2, null);
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(tail, .monotonic));
            try Q.advance(a, &head);
            try std.testing.expect(head == tail);
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(tail, .monotonic));
            var q = try Q.init().push(a, 7);
            defer q.deinit(a);
            q.size = std.math.maxInt(u32);
            try std.testing.expectError(error.Overflow, q.push(a, 8));
            try std.testing.expectError(error.Overflow, q.pushInPlace(a, 8));
            q.size = 1;
            Q.setRefs(q.front_stack.?, limit);
            try std.testing.expectError(error.Overflow, q.clone());
            try std.testing.expectError(error.Overflow, q.pop(a));
            Q.setRefs(q.front_stack.?, 1);
            // clone должен откатить первую ссылку, если следующее поле переполнено.
            try q.pushInPlace(a, 8);
            const original = Q.refCount(q.front_stack.?, .monotonic);
            Q.setRefs(q.rear.?, limit);
            try std.testing.expectError(error.Overflow, q.clone());
            try std.testing.expectEqual(original, Q.refCount(q.front_stack.?, .monotonic));
            Q.setRefs(q.rear.?, 1);
            // Shared advance не расходует голову при переполнении хвоста.
            try Q.prepend(a, &head, 3, null);
            Q.setRefs(head.?, 2);
            Q.setRefs(tail, limit);
            const before = head;
            try std.testing.expectError(error.Overflow, Q.advance(a, &head));
            try std.testing.expectEqual(before, head);
            Q.setRefs(head.?, 1);
            Q.setRefs(tail, 1);
        }

        // малый предел ссылок: запасной путь, переполнение и откат
        fn smallRefsLimit() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = 8 });
            const a = std.testing.allocator;
            for (1..40) |size| {
                var q = Q.init();
                defer q.deinit(a);
                for (0..size) |i| try q.pushInPlace(a, @intCast(i));
                var copies: [8]Q = undefined;
                var count: usize = 0;
                defer for (copies[0..count]) |*copy| copy.deinit(a);
                while (true) {
                    const copy = q.clone() catch |err| {
                        try std.testing.expectEqual(error.Overflow, err);
                        break;
                    };
                    copies[count] = copy;
                    count += 1;
                }
                const before = q;
                const refs = Q.refCount(q.front_stack.?, .monotonic);
                try std.testing.expectError(error.Overflow, q.pushInPlace(a, 99));
                try std.testing.expectError(error.Overflow, q.popInPlace(a));
                try std.testing.expect(std.meta.eql(before, q));
                try std.testing.expectEqual(refs, Q.refCount(q.front_stack.?, .monotonic));
                for (copies[0..count]) |copy| {
                    try std.testing.expectEqual(@as(u32, @intCast(size)), copy.len());
                    try std.testing.expectEqual(@as(?i32, 0), copy.front());
                }
            }
            // Вблизи предела запасной путь также умеет успешно закончиться.
            var q = try Q.init().push(a, 10);
            defer q.deinit(a);
            var copies: [6]Q = undefined;
            for (&copies) |*copy| copy.* = try q.clone();
            defer for (&copies) |*copy| copy.deinit(a);
            try std.testing.expect(q.nearLimit());
            try q.pushInPlace(a, 20);
            try std.testing.expectEqual(@as(?i32, 10), try q.popInPlace(a));
            try std.testing.expectEqual(@as(?i32, 20), q.front());
        }

        // быстрый pop не клонирует версию и не выделяет узлы без ротации
        fn allocationFreePop() !void {
            const a = std.testing.allocator;
            var q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = limit }).init();
            defer q.deinit(a);
            try q.pushInPlace(a, 1);
            var failing = std.testing.FailingAllocator.init(a, .{ .fail_index = 0 });
            try std.testing.expectEqual(@as(?i32, 1), try q.popInPlace(failing.allocator()));
            try std.testing.expectEqual(@as(usize, 0), failing.alloc_index);
            try std.testing.expect(q.isEmpty());
        }

        fn nearLimitAllocationFailures(allocator: Allocator) !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = 8 });
            var q = try Q.init().push(allocator, 10);
            defer q.deinit(allocator);
            var copies: [6]Q = undefined;
            for (&copies) |*copy| copy.* = try q.clone();
            defer for (&copies) |*copy| copy.deinit(allocator);
            const before = q;
            const refs = Q.refCount(q.front_stack.?, .monotonic);
            q.pushInPlace(allocator, 20) catch |err| {
                try std.testing.expect(std.meta.eql(before, q));
                try std.testing.expectEqual(refs, Q.refCount(q.front_stack.?, .monotonic));
                for (copies) |copy| try std.testing.expectEqual(@as(?i32, 10), copy.front());
                return err;
            };
            const before_pop = q;
            const refs_before_pop = Q.refCount(q.front_stack.?, .monotonic);
            _ = q.popInPlace(allocator) catch |err| {
                try std.testing.expect(std.meta.eql(before_pop, q));
                try std.testing.expectEqual(refs_before_pop, Q.refCount(q.front_stack.?, .monotonic));
                for (copies) |copy| try std.testing.expectEqual(@as(?i32, 10), copy.front());
                return err;
            };
            try std.testing.expectEqual(@as(?i32, 20), q.front());
        }

        // отказ каждой аллокации запасного пути вблизи предела ссылок
        fn nearLimitFailures() !void {
            try std.testing.checkAllAllocationFailures(std.testing.allocator, nearLimitAllocationFailures, .{});
        }

        // переполнение хвоста при advance: запасной pop откатывает клонирование
        fn advanceOverflowRollback() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = 8 });
            const a = std.testing.allocator;
            var q = Q.init();
            defer q.deinit(a);
            try Q.prepend(a, &q.front_stack, 2, null);
            const tail = q.front_stack;
            try Q.prepend(a, &q.front_stack, 1, null);
            q.size = 2;
            q.front_len = 2;
            var tails: [7]Q = undefined;
            for (&tails) |*version| {
                try Q.retain(tail);
                version.* = .{ .front_stack = tail, .size = 1, .front_len = 1 };
            }
            defer for (&tails) |*version| version.deinit(a);
            const before = q;
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(q.front_stack.?, .monotonic));
            try std.testing.expectError(error.Overflow, q.popInPlace(a));
            try std.testing.expect(std.meta.eql(before, q));
            try std.testing.expectEqual(@as(u32, 1), Q.refCount(q.front_stack.?, .monotonic));
            try std.testing.expectEqual(@as(u32, 8), Q.refCount(tail.?, .monotonic));
        }

        // Случайные истории при малом пределе ссылок и многих живых версиях:
        // предпроверка nearLimit не должна пропустить ни одного приобретения
        // (без atomic пропуск — Overflow в `catch unreachable` быстрого пути);
        // при ошибке версия не меняется, иначе совпадает с моделью.
        fn randomNearLimit() !void {
            const Q = QueueWithOptions(i32, .{ .atomic = atomic, .refs_limit = 6 });
            const a = std.testing.allocator;
            const max = 12;
            for (0..300) |seed| {
                var prng = std.Random.DefaultPrng.init(seed);
                const rnd = prng.random();
                var qs: [max]Q = undefined;
                var models: [max]std.ArrayList(i32) = undefined;
                var n: usize = 1;
                qs[0] = Q.init();
                models[0] = .empty;
                defer for (qs[0..n], models[0..n]) |*q, *m| {
                    q.deinit(a);
                    m.deinit(a);
                };
                var next_value: i32 = 0;
                for (0..400) |_| {
                    const i = rnd.uintLessThan(usize, n);
                    const before = qs[i];
                    switch (rnd.uintLessThan(u8, 6)) {
                        0 => if (qs[i].pushInPlace(a, next_value)) {
                            try models[i].append(a, next_value);
                        } else |err| {
                            try std.testing.expectEqual(error.Overflow, err);
                            try std.testing.expect(std.meta.eql(before, qs[i]));
                        },
                        1 => if (qs[i].popInPlace(a)) |value| {
                            if (value) |v| {
                                try std.testing.expectEqual(models[i].items[0], v);
                                _ = models[i].orderedRemove(0);
                            } else try std.testing.expectEqual(@as(usize, 0), models[i].items.len);
                        } else |err| {
                            try std.testing.expectEqual(error.Overflow, err);
                            try std.testing.expect(std.meta.eql(before, qs[i]));
                        },
                        2, 3, 4 => |op| if (n < max) {
                            const child = switch (op) {
                                2 => qs[i].push(a, next_value),
                                3 => if (qs[i].pop(a)) |c| c orelse continue else |err| err,
                                else => qs[i].clone(),
                            } catch |err| {
                                try std.testing.expectEqual(error.Overflow, err);
                                continue;
                            };
                            qs[n] = child;
                            models[n] = try models[i].clone(a);
                            switch (op) {
                                2 => try models[n].append(a, next_value),
                                3 => _ = models[n].orderedRemove(0),
                                else => {},
                            }
                            n += 1;
                        },
                        else => if (n > 1) {
                            qs[i].deinit(a);
                            models[i].deinit(a);
                            n -= 1;
                            qs[i] = qs[n];
                            models[i] = models[n];
                        },
                    }
                    next_value += 1;
                    if (i >= n) continue; // удалена последняя версия
                    try std.testing.expectEqual(@as(u32, @intCast(models[i].items.len)), qs[i].len());
                    try std.testing.expectEqual(if (models[i].items.len > 0) models[i].items[0] else null, qs[i].front());
                }
            }
        }
    };
}
test "обе реализации" {
    inline for (.{ false, true }) |atomic| {
        try InternalSuite(atomic).nodeLayoutAndOverflow();
        try InternalSuite(atomic).smallRefsLimit();
        try InternalSuite(atomic).allocationFreePop();
        try InternalSuite(atomic).nearLimitFailures();
        try InternalSuite(atomic).advanceOverflowRollback();
        try InternalSuite(atomic).randomNearLimit();
    }
}

test "атомарный мягкий предел: неотказывающий инкремент и откат выше предела" {
    const Q = QueueWithOptions(i32, .{ .atomic = true, .refs_limit = 8 });
    const a = std.testing.allocator;
    var q = try Q.init().push(a, 1);
    defer q.deinit(a);
    const node = q.front_stack.?;
    // Моделируем конкурирующее приобретение после nearLimit предпроверки.
    Q.setRefs(node, 8);
    defer Q.setRefs(node, 1);
    try Q.retainMode(node, true);
    try std.testing.expectEqual(@as(u32, 9), Q.refCount(node, .monotonic));
    try std.testing.expectError(error.Overflow, q.clone());
    try std.testing.expectError(error.Overflow, q.push(a, 2));
    try std.testing.expectError(error.Overflow, q.pop(a));
    try std.testing.expectEqual(@as(u32, 9), Q.refCount(node, .monotonic));
    _ = Q.dropRef(node);
}
