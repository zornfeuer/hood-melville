const std = @import("std");
const Q = @import("root.zig").Queue(i32);
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
test "полный перебор ветвящихся историй" {
    try exhaustive(Q.init(), &.{}, 13);
}
test "линейные истории и изменения разделяемых версий" {
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
test "отказ каждой аллокации" {
    try std.testing.checkAllAllocationFailures(a, failures, .{});
}

test "случайное ветвление от сохранённых версий" {
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

test "отказ каждой аллокации на месте: все фазы, уникальные и общие узлы" {
    for (0..32) |size| {
        for ([_]bool{ false, true }) |shared| {
            for ([_]bool{ false, true }) |remove| {
                try std.testing.checkAllAllocationFailures(a, inPlaceFailures, .{ size, shared, remove });
            }
        }
    }
}
