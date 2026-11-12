const std = @import("std");
const options = @import("bench_options");
const fqueue = @import("fqueue");
// С пулом узлов (docs/pool.md) — тот же API; c_allocator — источник плит.
const Q = if (options.pool)
    (if (options.sync) fqueue.SyncPooledQueue(i32) else fqueue.PooledQueue(i32))
else if (options.sync) fqueue.SyncQueue(i32) else fqueue.Queue(i32);
const allocator = std.heap.c_allocator;

const Scenario = enum { fill, mixed, persistent, branching, fill_mut, mixed_mut, wave };

fn mix(h: u64, x: i32) u64 {
    return h *% 31 +% @as(u64, @bitCast(@as(i64, x)));
}

// Весь вывод форматируется в буфер на стеке, без выделения памяти.
const Phases = struct {
    io: std.Io,
    on: bool,
    t0: std.Io.Timestamp,

    fn mark(self: *@This(), name: []const u8) void {
        if (!self.on) return;
        const t = std.Io.Clock.awake.now(self.io);
        var buffer: [128]u8 = undefined;
        const text = std.fmt.bufPrint(&buffer, "phase {s} {d:.3} ms\n", .{
            name, @as(f64, @floatFromInt(self.t0.durationTo(t).nanoseconds)) / 1e6,
        }) catch unreachable;
        std.Io.File.stderr().writeStreamingAll(self.io, text) catch @panic("stderr write failed");
        self.t0 = t;
    }
};

fn replacePush(q: *Q, value: i32) !void {
    const next = try q.push(allocator, value);
    q.deinit(allocator);
    q.* = next;
}

fn replacePop(q: *Q) !void {
    const next = (try q.pop(allocator)).?;
    q.deinit(allocator);
    q.* = next;
}

/// N push, затем опустошение. Старая версия освобождается сразу.
fn runFill(n: i32, ph: *Phases) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var h: u64 = 0;
    var i: i32 = 0;
    while (i < n) : (i += 1) try replacePush(&q, i);
    ph.mark("build");
    // Сначала читается голова, затем pop, как в C/Rust.
    while (!q.isEmpty()) {
        h = mix(h, q.front().?);
        try replacePop(&q);
    }
    ph.mark("drain");
    return h;
}

/// push, push, pop до размера N, затем опустошение.
fn runMixed(n: i32, ph: *Phases) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var h: u64 = 0;
    var i: i32 = 0;
    while (q.len() < @as(u32, @intCast(n))) {
        // Два отдельных присваивания: промежуточная версия умирает сразу.
        try replacePush(&q, i);
        try replacePush(&q, i + 1);
        i += 2;
        h = mix(h, q.front().?);
        try replacePop(&q);
    }
    ph.mark("build");
    while (!q.isEmpty()) {
        h = mix(h, q.front().?);
        try replacePop(&q);
    }
    ph.mark("drain");
    return h;
}

/// Та же история fill через настоящие операции на месте.
fn runFillMut(n: i32, ph: *Phases) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var h: u64 = 0;
    var i: i32 = 0;
    while (i < n) : (i += 1) try q.pushInPlace(allocator, i);
    ph.mark("build");
    while (!q.isEmpty()) h = mix(h, (try q.popInPlace(allocator)).?);
    ph.mark("drain");
    return h;
}

/// Та же история mixed через настоящие операции на месте.
fn runMixedMut(n: i32, ph: *Phases) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var h: u64 = 0;
    var i: i32 = 0;
    while (q.len() < @as(u32, @intCast(n))) {
        try q.pushInPlace(allocator, i);
        try q.pushInPlace(allocator, i + 1);
        i += 2;
        h = mix(h, (try q.popInPlace(allocator)).?);
    }
    ph.mark("build");
    while (!q.isEmpty()) h = mix(h, (try q.popInPlace(allocator)).?);
    ph.mark("drain");
    return h;
}

// Волны: живое множество растёт до N и сжимается до N/100, waves раз, через
// операции на месте; затем опустошение (удержание памяти после сжатия,
// docs/pool.md).
const waves = 4;

fn runWave(n: i32, ph: *Phases) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var h: u64 = 0;
    var i: i32 = 0;
    const big: u32 = @intCast(n);
    for (0..waves) |_| {
        while (q.len() < big) : (i += 1) try q.pushInPlace(allocator, i);
        ph.mark("grow");
        while (q.len() > big / 100) h = mix(h, (try q.popInPlace(allocator)).?);
        ph.mark("shrink");
    }
    while (!q.isEmpty()) h = mix(h, (try q.popInPlace(allocator)).?);
    return h;
}

fn last(versions: *const std.ArrayList(Q)) *const Q {
    return &versions.items[versions.items.len - 1];
}

fn keepPush(versions: *std.ArrayList(Q), value: i32) !void {
    try versions.ensureUnusedCapacity(allocator, 1);
    versions.appendAssumeCapacity(try last(versions).push(allocator, value));
}

fn keepPop(versions: *std.ArrayList(Q)) !void {
    try versions.ensureUnusedCapacity(allocator, 1);
    versions.appendAssumeCapacity((try last(versions).pop(allocator)).?);
}

fn destroy(versions: *std.ArrayList(Q)) void {
    // Версии хранятся по значению и освобождаются в порядке создания.
    for (versions.items) |*q| q.deinit(allocator);
    versions.deinit(allocator);
    versions.* = .empty;
}

/// Как mixed, но все версии живы до конца.
fn runPersistent(n: i32, ph: *Phases) !u64 {
    var versions: std.ArrayList(Q) = .empty;
    defer destroy(&versions);
    try versions.append(allocator, Q.init());
    var h: u64 = 0;
    var i: i32 = 0;
    while (last(&versions).len() < @as(u32, @intCast(n))) {
        try keepPush(&versions, i);
        try keepPush(&versions, i + 1);
        i += 2;
        h = mix(h, last(&versions).front().?);
        try keepPop(&versions);
    }
    ph.mark("build");
    while (!last(&versions).isEmpty()) {
        h = mix(h, last(&versions).front().?);
        try keepPop(&versions);
    }
    ph.mark("drain");
    destroy(&versions);
    ph.mark("destroy");
    return h;
}

/// Восемь активных веток; раз в 64 шага родитель берётся из всей истории.
/// Голова родителя читается повторно; вероятность pop — 1/3.
fn runBranching(n: i32, ph: *Phases) !u64 {
    var versions: std.ArrayList(Q) = .empty;
    defer destroy(&versions);
    try versions.append(allocator, Q.init());
    var h: u64 = 0;
    var x: u64 = 0x9E3779B97F4A7C15;
    var heads = [_]usize{0} ** 8;
    var k: i32 = 0;
    while (k < n) : (k += 1) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const slot = (x >> 3) % heads.len;
        const idx = if (x & 63 == 0) (x >> 12) % versions.items.len else heads[slot];
        // Резервирование может переместить массив: родитель берётся после него.
        try versions.ensureUnusedCapacity(allocator, 1);
        const parent = &versions.items[idx];
        const child = if (parent.front()) |value| blk: {
            h = mix(h, value);
            break :blk if ((x >> 40) % 3 == 0)
                (try parent.pop(allocator)).?
            else
                try parent.push(allocator, k);
        } else try parent.push(allocator, k);
        heads[slot] = versions.items.len;
        versions.appendAssumeCapacity(child);
    }
    ph.mark("build");
    destroy(&versions);
    ph.mark("destroy");
    return h;
}

pub fn main(init: std.process.Init) !void {
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    if (args.len != 3) {
        std.debug.print("usage: fqueue_bench <fill|mixed|persistent|branching|fill_mut|mixed_mut|wave> <N>\n", .{});
        std.process.exit(2);
    }
    const scenario = std.meta.stringToEnum(Scenario, args[1]) orelse {
        std.debug.print("unknown scenario: {s}\n", .{args[1]});
        std.process.exit(2);
    };
    const n = std.fmt.parseInt(i32, args[2], 10) catch {
        std.debug.print("invalid N: {s}\n", .{args[2]});
        std.process.exit(2);
    };
    if (n < 0 or n > std.math.maxInt(i32) / 2) {
        std.debug.print("N out of range\n", .{});
        std.process.exit(2);
    }
    const env = init.environ_map.get("FQ_PHASES");
    var ph: Phases = .{
        .io = init.io,
        .on = if (env) |s| std.mem.eql(u8, s, "1") else false,
        .t0 = std.Io.Clock.awake.now(init.io),
    };
    const h = switch (scenario) {
        .fill => try runFill(n, &ph),
        .mixed => try runMixed(n, &ph),
        .persistent => try runPersistent(n, &ph),
        .branching => try runBranching(n, &ph),
        .fill_mut => try runFillMut(n, &ph),
        .mixed_mut => try runMixedMut(n, &ph),
        .wave => try runWave(n, &ph),
    };
    var buffer: [32]u8 = undefined;
    const text = try std.fmt.bufPrint(&buffer, "{d}\n", .{h});
    try std.Io.File.stdout().writeStreamingAll(init.io, text);
}
