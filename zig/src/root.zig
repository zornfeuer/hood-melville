const std = @import("std");
const Allocator = std.mem.Allocator;

/// Значения T копируются присваиванием, как в C-шаблоне. Ресурсами внутри T
/// владеет вызывающий. Очередь не потокобезопасна; все версии одного графа
/// должны использовать один и тот же аллокатор. Копия по значению — перенос
/// владения; для независимой версии нужен clone.
pub fn Queue(comptime T: type) type {
    return QueueWithOptions(T, .{});
}

/// Настройки предела ссылок, в том числе для проверки переполнения.
pub const Options = struct {
    /// Максимальное число владеющих ссылок на узел.
    refs_limit: u32 = std.math.maxInt(u32),
};

/// Очередь с заданным пределом счётчика ссылок (не меньше трёх).
pub fn QueueWithOptions(comptime T: type, comptime options: Options) type {
    if (options.refs_limit < 3) @compileError("refs_limit must be at least 3");
    return struct {
        const Self = @This();
        const Node = struct {
            value: T,
            refs: u32 = 1,
            next: ?*@This(),
        };

        /// Ошибки выделения памяти и переполнения длины или ссылок.
        pub const Error = Allocator.Error || error{Overflow};
        const Reversing = struct {
            old_front: ?*Node,
            old_rear: ?*Node,
            rfront: ?*Node = null,
            acc: ?*Node = null,
        };
        const Appending = struct {
            rfront: ?*Node,
            acc: ?*Node,
        };
        const Rotation = union(enum) {
            idle,
            reversing: Reversing,
            appending: Appending,
        };
        size: u32 = 0,
        front_len: u32 = 0,
        rear_len: u32 = 0,
        ok: u32 = 0,
        front_stack: ?*Node = null,
        rear: ?*Node = null,
        rot: Rotation = .idle,

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

        fn retain(n: ?*Node) Error!void {
            if (n) |p| {
                if (p.refs == options.refs_limit) return error.Overflow;
                p.refs += 1;
            }
        }

        fn touch(n: ?*Node) void {
            if (n) |p| {
                const refs: *volatile u32 = &p.refs;
                _ = refs.*;
            }
        }

        fn release(allocator: Allocator, head: ?*Node) void {
            var cur = head;
            while (cur) |n| {
                n.refs -= 1;
                if (n.refs != 0) break;
                cur = n.next;
                touch(cur);
                allocator.destroy(n);
            }
        }

        // cons забирает ссылку на хвост лишь после успешного выделения.
        fn prepend(allocator: Allocator, dst: *?*Node, value: T, pool: ?*Pool) Error!void {
            const n = if (pool) |p| p.take() else try allocator.create(Node);
            n.* = .{ .value = value, .next = dst.* };
            dst.* = n;
        }

        fn advance(allocator: Allocator, dst: *?*Node) Error!void {
            const n = dst.* orelse return;
            const next = n.next;
            if (n.refs == 1) {
                touch(next);
                allocator.destroy(n);
            } else {
                // При переполнении ссылка остаётся у исходного поля.
                try retain(next);
                n.refs -= 1;
            }
            dst.* = next;
        }

        /// Освободить ссылки версии; после вызова значение не определено.
        pub fn deinit(self: *Self, allocator: Allocator) void {
            release(allocator, self.front_stack);
            release(allocator, self.rear);
            switch (self.rot) {
                .idle => {},
                .reversing => |s| {
                    release(allocator, s.old_front);
                    release(allocator, s.old_rear);
                    release(allocator, s.rfront);
                    release(allocator, s.acc);
                },
                .appending => |s| {
                    release(allocator, s.rfront);
                    release(allocator, s.acc);
                },
            }
            self.* = undefined;
        }

        /// Приобрести ссылки на узлы для независимой версии за O(1).
        pub fn clone(self: Self) Error!Self {
            // Поэтапное приобретение ссылок позволяет откатить даже алиасы
            // между полями, не оставляя временного переполнения счётчика.
            var nodes: [6]?*Node = .{ self.front_stack, self.rear, null, null, null, null };
            switch (self.rot) {
                .idle => {},
                .reversing => |s| {
                    nodes[2..6].* = .{ s.old_front, s.old_rear, s.rfront, s.acc };
                },
                .appending => |s| {
                    nodes[2] = s.rfront;
                    nodes[3] = s.acc;
                },
            }
            var acquired: usize = 0;
            errdefer for (nodes[0..acquired]) |n| {
                if (n) |p| p.refs -= 1;
            };
            for (nodes) |n| {
                try retain(n);
                acquired += 1;
            }
            return self;
        }

        fn start(self: *Self) Error!void {
            try retain(self.front_stack);
            self.rot = .{ .reversing = .{ .old_front = self.front_stack, .old_rear = self.rear } };
            self.ok = self.front_len;
            self.rear = null;
            self.rear_len = 0;
        }

        fn finish(self: *Self, allocator: Allocator) void {
            const s = self.rot.appending;
            release(allocator, self.front_stack);
            release(allocator, s.rfront);
            self.front_stack = s.acc;
            self.front_len = self.size - self.rear_len;
            self.rot = .idle;
            self.ok = 0;
        }

        fn step(self: *Self, allocator: Allocator, pool: ?*Pool) Error!void {
            switch (self.rot) {
                .idle => {},
                .reversing => |*s| {
                    try prepend(allocator, &s.acc, s.old_rear.?.value, pool);
                    try advance(allocator, &s.old_rear);
                    if (s.old_front) |n| {
                        try prepend(allocator, &s.rfront, n.value, pool);
                        try advance(allocator, &s.old_front);
                    }
                    if (s.old_rear == null) {
                        std.debug.assert(s.old_front == null);
                        const next: Appending = .{ .rfront = s.rfront, .acc = s.acc };
                        self.rot = .{ .appending = next };
                        if (self.ok == 0) self.finish(allocator);
                    }
                },
                .appending => |*s| {
                    try prepend(allocator, &s.acc, s.rfront.?.value, pool);
                    try advance(allocator, &s.rfront);
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
            if (q.rot == .idle and q.rear_len > q.front_len) try q.start();
            try q.step(allocator, null);
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
            if (q.rot != .idle) {
                q.ok -= 1;
                if (q.rot == .appending and q.ok == 0) q.finish(allocator);
            } else if (q.rear_len > q.front_len) try q.start();
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
                    pool.nodes[pool.count] = try allocator.create(Node);
                return pool;
            }

            fn take(self: *Pool) *Node {
                std.debug.assert(self.count > 0);
                self.count -= 1;
                return self.nodes[self.count];
            }

            fn deinit(self: *Pool, allocator: Allocator) void {
                while (self.count > 0) allocator.destroy(self.take());
                self.* = undefined;
            }
        };

        // Ровно один шаг модели Rust: переход Reversing не переносит rfront.
        fn stepAllocs(rotation: Rotation, ok: u32) usize {
            return switch (rotation) {
                .idle => 0,
                .reversing => |s| 1 + @as(usize, @intFromBool(s.old_front != null)),
                .appending => @intFromBool(ok != 0),
            };
        }

        fn nextNearLimit(node: ?*Node) bool {
            const n = node orelse return false;
            const next = n.next orelse return false;
            return next.refs > options.refs_limit - 2;
        }

        // За операцию один узел получает не больше двух новых ссылок:
        // advance живого фронта, затем start или advance снимка фронта.
        // Проверка консервативна; вблизи предела точный откат даёт slow path.
        fn nearLimit(self: Self) bool {
            if (self.front_stack) |n| {
                if (n.refs > options.refs_limit - 2 or nextNearLimit(n) or nextNearLimit(n.next)) return true;
            }
            if (nextNearLimit(self.rear)) return true;
            return switch (self.rot) {
                .idle => false,
                .reversing => |s| nextNearLimit(s.old_front) or nextNearLimit(s.old_rear) or nextNearLimit(s.rfront),
                .appending => |s| nextNearLimit(s.rfront),
            };
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
            const starting = self.rot == .idle and self.rear_len + 1 > self.front_len;
            const rotation: Rotation = if (starting)
                .{ .reversing = .{ .old_front = self.front_stack, .old_rear = null } }
            else
                self.rot;
            var pool = try Pool.init(allocator, 1 + stepAllocs(rotation, self.ok));
            defer pool.deinit(allocator);

            // После подготовки пула и запаса ссылок ошибок уже быть не может.
            prepend(allocator, &self.rear, value, &pool) catch unreachable;
            self.size += 1;
            self.rear_len += 1;
            if (starting) self.start() catch unreachable;
            self.step(allocator, &pool) catch unreachable;
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
            const starting = self.rot == .idle and self.rear_len > self.front_len - 1;
            const rotation: Rotation = if (starting)
                .{ .reversing = .{ .old_front = self.front_stack.?.next, .old_rear = self.rear } }
            else
                self.rot;
            const ok = if (self.rot == .idle) self.front_len - 1 else self.ok - 1;
            var pool = try Pool.init(allocator, stepAllocs(rotation, ok));
            defer pool.deinit(allocator);

            advance(allocator, &self.front_stack) catch unreachable;
            self.size -= 1;
            self.front_len -= 1;
            if (self.rot != .idle) {
                self.ok -= 1;
                if (self.rot == .appending and self.ok == 0) self.finish(allocator);
            } else if (starting) self.start() catch unreachable;
            self.step(allocator, &pool) catch unreachable;
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

test "размер узла, перенос хвоста и переполнение" {
    const Q = Queue(i32);
    const a = std.testing.allocator;
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(Q.Node));
    try std.testing.expectEqual(@as(usize, 72), @sizeOf(Q));
    var head: ?*Q.Node = null;
    defer Q.release(a, head);
    try Q.prepend(a, &head, 1, null);
    const tail = head.?;
    try Q.prepend(a, &head, 2, null);
    try std.testing.expectEqual(@as(u32, 1), tail.refs);
    try Q.advance(a, &head);
    try std.testing.expect(head == tail);
    try std.testing.expectEqual(@as(u32, 1), tail.refs);
    var q = try Q.init().push(a, 7);
    defer q.deinit(a);
    q.size = std.math.maxInt(u32);
    try std.testing.expectError(error.Overflow, q.push(a, 8));
    try std.testing.expectError(error.Overflow, q.pushInPlace(a, 8));
    q.size = 1;
    q.front_stack.?.refs = std.math.maxInt(u32);
    try std.testing.expectError(error.Overflow, q.clone());
    try std.testing.expectError(error.Overflow, q.pop(a));
    q.front_stack.?.refs = 1;
    // clone должен откатить первую ссылку, если следующее поле переполнено.
    try q.pushInPlace(a, 8);
    const original = q.front_stack.?.refs;
    q.rear.?.refs = std.math.maxInt(u32);
    try std.testing.expectError(error.Overflow, q.clone());
    try std.testing.expectEqual(original, q.front_stack.?.refs);
    q.rear.?.refs = 1;
    // Shared advance не расходует голову при переполнении хвоста.
    try Q.prepend(a, &head, 3, null);
    head.?.refs = 2;
    tail.refs = std.math.maxInt(u32);
    const before = head;
    try std.testing.expectError(error.Overflow, Q.advance(a, &head));
    try std.testing.expectEqual(before, head);
    head.?.refs = 1;
    tail.refs = 1;
}

test "малый предел ссылок: запасной путь, переполнение и откат" {
    const Q = QueueWithOptions(i32, .{ .refs_limit = 8 });
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
        const refs = q.front_stack.?.refs;
        try std.testing.expectError(error.Overflow, q.pushInPlace(a, 99));
        try std.testing.expectError(error.Overflow, q.popInPlace(a));
        try std.testing.expect(std.meta.eql(before, q));
        try std.testing.expectEqual(refs, q.front_stack.?.refs);
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

test "быстрый pop не клонирует версию и не выделяет узлы без ротации" {
    const a = std.testing.allocator;
    var q = Queue(i32).init();
    defer q.deinit(a);
    try q.pushInPlace(a, 1);
    var failing = std.testing.FailingAllocator.init(a, .{ .fail_index = 0 });
    try std.testing.expectEqual(@as(?i32, 1), try q.popInPlace(failing.allocator()));
    try std.testing.expectEqual(@as(usize, 0), failing.alloc_index);
    try std.testing.expect(q.isEmpty());
}

fn nearLimitAllocationFailures(allocator: Allocator) !void {
    const Q = QueueWithOptions(i32, .{ .refs_limit = 8 });
    var q = try Q.init().push(allocator, 10);
    defer q.deinit(allocator);
    var copies: [6]Q = undefined;
    for (&copies) |*copy| copy.* = try q.clone();
    defer for (&copies) |*copy| copy.deinit(allocator);
    const before = q;
    const refs = q.front_stack.?.refs;
    q.pushInPlace(allocator, 20) catch |err| {
        try std.testing.expect(std.meta.eql(before, q));
        try std.testing.expectEqual(refs, q.front_stack.?.refs);
        for (copies) |copy| try std.testing.expectEqual(@as(?i32, 10), copy.front());
        return err;
    };
    const before_pop = q;
    const refs_before_pop = q.front_stack.?.refs;
    _ = q.popInPlace(allocator) catch |err| {
        try std.testing.expect(std.meta.eql(before_pop, q));
        try std.testing.expectEqual(refs_before_pop, q.front_stack.?.refs);
        for (copies) |copy| try std.testing.expectEqual(@as(?i32, 10), copy.front());
        return err;
    };
    try std.testing.expectEqual(@as(?i32, 20), q.front());
}

test "отказ каждой аллокации запасного пути вблизи предела ссылок" {
    try std.testing.checkAllAllocationFailures(std.testing.allocator, nearLimitAllocationFailures, .{});
}

test "переполнение хвоста при advance: запасной pop откатывает клонирование" {
    const Q = QueueWithOptions(i32, .{ .refs_limit = 8 });
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
    try std.testing.expectEqual(@as(u32, 1), q.front_stack.?.refs);
    try std.testing.expectError(error.Overflow, q.popInPlace(a));
    try std.testing.expect(std.meta.eql(before, q));
    try std.testing.expectEqual(@as(u32, 1), q.front_stack.?.refs);
    try std.testing.expectEqual(@as(u32, 8), tail.?.refs);
}
