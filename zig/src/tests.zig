const std = @import("std");
fn Suite(comptime Q: type) type {
    return struct {
        const a = std.testing.allocator;
        fn check(q: Q, model: []const i32) !void {
            try std.testing.expectEqual(model.len, q.len());
            try std.testing.expectEqual(if (model.len == 0) null else model[0], q.front());
            var it = try q.iterator();
            defer it.deinit(a);
            for (model) |v| try std.testing.expectEqual(v, (try it.next(a)).?);
            try std.testing.expectEqual(@as(?i32, null), try it.next(a));
        }
        fn exhaustive(q: Q, model: []const i32, depth: i32) !void {
            try check(q, model);
            if (depth == 0) return;
            var buf: [32]i32 = undefined;
            @memcpy(buf[0..model.len], model);
            buf[model.len] = depth;
            var pushed = try q.push(a, depth);
            defer pushed.deinit(a);
            try exhaustive(pushed, buf[0 .. model.len + 1], depth - 1);
            if (try q.pop(a)) |next| {
                var popped = next;
                defer popped.deinit(a);
                try exhaustive(popped, model[1..], depth - 1);
            }
            try check(q, model);
        }
        // полный перебор ветвящихся историй
        fn exhaustiveHistories() !void {
            try exhaustive(Q.init(), &.{}, 13);
        }
        // линейные истории и изменения разделяемых версий
        fn sharedMutations() !void {
            var q = Q.init();
            defer q.deinit(a);
            var model: std.ArrayList(i32) = .empty;
            defer model.deinit(a);
            var x: u64 = 12345;
            for (0..400) |i| {
                var old = try q.clone();
                defer old.deinit(a);
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                if (model.items.len == 0 or x % 3 != 0) {
                    try q.pushInPlace(a, @intCast(i));
                    try check(old, model.items);
                    try model.append(a, @intCast(i));
                } else {
                    try std.testing.expectEqual(model.items[0], (try q.popInPlace(a)).?);
                    try check(old, model.items);
                    _ = model.orderedRemove(0);
                }
                try check(q, model.items);
            }
        }
        fn failures(allocator: std.mem.Allocator) !void {
            var q = Q.init();
            defer q.deinit(allocator);
            for (0..40) |i| {
                const snapshot = q;
                const before = q.len();
                const first = q.front();
                q.pushInPlace(allocator, @intCast(i)) catch |err| {
                    try std.testing.expect(std.meta.eql(snapshot, q));
                    try std.testing.expectEqual(before, q.len());
                    try std.testing.expectEqual(first, q.front());
                    return err;
                };
                var old = try q.clone();
                defer old.deinit(allocator);
                var child = q.push(allocator, 100) catch |err| {
                    try std.testing.expect(std.meta.eql(old, q));
                    return err;
                };
                defer child.deinit(allocator);
                var popped = (q.pop(allocator) catch |err| {
                    try std.testing.expect(std.meta.eql(old, q));
                    return err;
                }).?;
                defer popped.deinit(allocator);
                if (i % 3 == 0) _ = q.popInPlace(allocator) catch |err| {
                    try std.testing.expectEqual(before + 1, q.len());
                    try std.testing.expectEqual(old.front(), q.front());
                    return err;
                };
            }
        }
        // отказ каждой аллокации
        fn allocationFailures() !void {
            try std.testing.checkAllAllocationFailures(a, failures, .{});
        }

        // случайное ветвление от сохранённых версий
        fn branchingHistories() !void {
            var versions: [400]Q = undefined;
            var models: [400][64]i32 = undefined;
            var sizes: [400]usize = undefined;
            versions[0] = Q.init();
            sizes[0] = 0;
            var count: usize = 1;
            defer for (versions[0..count]) |*q| q.deinit(a);
            var heads = [_]usize{0} ** 8;
            var x: u64 = 0x9e3779b97f4a7c15;
            while (count < versions.len) : (count += 1) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const slot = (x >> 3) % 8;
                const parent = if (x & 63 == 0) (x >> 12) % count else heads[slot];
                const m = models[parent][0..sizes[parent]];
                const remove = m.len != 0 and (x >> 40) % 3 == 0;
                if (remove) {
                    versions[count] = (try versions[parent].pop(a)).?;
                    sizes[count] = m.len - 1;
                    @memcpy(models[count][0..sizes[count]], m[1..]);
                } else {
                    versions[count] = try versions[parent].push(a, @intCast(count));
                    sizes[count] = m.len + 1;
                    @memcpy(models[count][0..m.len], m);
                    models[count][m.len] = @intCast(count);
                }
                heads[slot] = count;
                try check(versions[parent], m);
                try check(versions[count], models[count][0..sizes[count]]);
            }
            for (versions, 0..) |q, i| try check(q, models[i][0..sizes[i]]);
        }

        // Для каждого состояния проверяем все точки OOM, отдельно с разделением
        // узлов и без него. Модель и сохранённые версии проверяются и при ошибке.
        fn inPlaceFailures(allocator: std.mem.Allocator, size: usize, shared: bool, remove: bool) !void {
            var q = Q.init();
            defer q.deinit(allocator);
            var model: [40]i32 = undefined;
            for (0..size) |i| {
                try q.pushInPlace(allocator, @intCast(i));
                model[i] = @intCast(i);
            }
            var sibling = if (shared) try q.clone() else Q.init();
            defer sibling.deinit(allocator);
            const before = q;
            if (remove) {
                _ = q.popInPlace(allocator) catch |err| {
                    try std.testing.expect(std.meta.eql(before, q));
                    try check(q, model[0..size]);
                    if (shared) try check(sibling, model[0..size]);
                    return err;
                };
                try check(q, if (size == 0) &.{} else model[1..size]);
            } else {
                q.pushInPlace(allocator, 99) catch |err| {
                    try std.testing.expect(std.meta.eql(before, q));
                    try check(q, model[0..size]);
                    if (shared) try check(sibling, model[0..size]);
                    return err;
                };
                model[size] = 99;
                try check(q, model[0 .. size + 1]);
            }
            if (shared) try check(sibling, model[0..size]);
        }

        // отказ каждой аллокации на месте: все фазы, уникальные и общие узлы
        fn inPlaceAllocationFailures() !void {
            for (0..32) |size| {
                for ([_]bool{ false, true }) |shared| {
                    for ([_]bool{ false, true }) |remove| {
                        try std.testing.checkAllAllocationFailures(a, inPlaceFailures, .{ size, shared, remove });
                    }
                }
            }
        }
    };
}
test "обе реализации" {
    inline for (.{ @import("root.zig").Queue(i32), @import("root.zig").SyncQueue(i32) }) |Q| {
        try Suite(Q).exhaustiveHistories();
        try Suite(Q).sharedMutations();
        try Suite(Q).allocationFailures();
        try Suite(Q).branchingHistories();
        try Suite(Q).inPlaceAllocationFailures();
    }
}

const Sync = @import("root.zig").SyncQueue(i32);
fn checkThread(q: Sync, model: []const i32, allocator: std.mem.Allocator) !void {
    try std.testing.expectEqual(model.len, q.len());
    try std.testing.expectEqual(if (model.len == 0) null else model[0], q.front());
    var it = try q.iterator();
    defer it.deinit(allocator);
    for (model) |v| try std.testing.expectEqual(v, (try it.next(allocator)).?);
    try std.testing.expectEqual(@as(?i32, null), try it.next(allocator));
}
const Worker = struct {
    versions: []const Sync,
    allocator: std.mem.Allocator,
    id: usize,
    failure: ?anyerror = null,

    fn run(self: *@This()) void {
        self.work() catch |err| {
            self.failure = err;
        };
    }
    fn work(self: *@This()) !void {
        for (0..300) |round| {
            const size = (round * 17 + self.id * 7) % self.versions.len;
            const parent = self.versions[size];
            var model: [80]i32 = undefined;
            for (0..size) |i| model[i] = @intCast(i);
            try checkThread(parent, model[0..size], self.allocator);
            var q = try parent.clone();
            defer q.deinit(self.allocator);
            const value: i32 = @intCast(round + self.id * 1000);
            var pushed = try parent.push(self.allocator, value);
            defer pushed.deinit(self.allocator);
            model[size] = value;
            try checkThread(pushed, model[0 .. size + 1], self.allocator);
            if (try parent.pop(self.allocator)) |next| {
                var popped = next;
                defer popped.deinit(self.allocator);
                try checkThread(popped, model[1..size], self.allocator);
            }
            try q.pushInPlace(self.allocator, value);
            var sibling = try q.clone();
            defer sibling.deinit(self.allocator);
            try checkThread(q, model[0 .. size + 1], self.allocator);
            for (model[0 .. size + 1]) |v| try std.testing.expectEqual(v, (try q.popInPlace(self.allocator)).?);
            try std.testing.expect(q.isEmpty());
            try checkThread(sibling, model[0 .. size + 1], self.allocator);
            try checkThread(parent, model[0..size], self.allocator);
        }
    }
};

test "общие версии: параллельное ветвление, изменение клонов и освобождение" {
    var safe: std.heap.DebugAllocator(.{ .thread_safe = true }) = .init;
    defer std.testing.expect(safe.deinit() == .ok) catch @panic("queue leaked");
    const allocator = safe.allocator();
    var versions: [64]Sync = undefined;
    versions[0] = Sync.init();
    var count: usize = 1;
    defer for (versions[0..count]) |*q| q.deinit(allocator);
    while (count < versions.len) : (count += 1) versions[count] = try versions[count - 1].push(allocator, @intCast(count - 1));
    var workers: [6]Worker = undefined;
    var threads: [6]std.Thread = undefined;
    var started: usize = 0;
    // Join also on spawn failure, before freeing any shared versions.
    defer for (threads[0..started]) |thread| thread.join();
    for (&workers, &threads, 0..) |*worker, *thread, id| {
        worker.* = .{ .versions = &versions, .allocator = allocator, .id = id };
        thread.* = try std.Thread.spawn(.{}, Worker.run, .{worker});
        started += 1;
    }
    for (threads[0..started]) |thread| thread.join();
    started = 0;
    for (workers) |worker| if (worker.failure) |err| return err;
    var model: [64]i32 = undefined;
    for (&model, 0..) |*v, i| v.* = @intCast(i);
    for (versions, 0..) |q, size| try checkThread(q, model[0..size], allocator);
}
