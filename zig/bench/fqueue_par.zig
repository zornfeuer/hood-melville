//! Параллельная нагрузка на SyncQueue (#6). Rust-двойник —
//! rust/examples/fqueue_par.rs (там описание сценариев), C — c/bench/fqueue_par.c;
//! сценарии и контрольные суммы совпадают.
//!
//! Использование:
//!   fqueue_par share <pool|threads> <share> <P>
//!     pool — пул `std.Io.Threaded`: P - 1 рабочих (`Group.concurrent`) и
//!     вызывающий берут номера задач из общего атомарного счётчика —
//!     динамическая раздача, как `schedule(dynamic)` в OpenMP. Готового
//!     параллельного цикла в std Zig 0.16 нет (`std.Thread.Pool` удалён), и
//!     задача на элемент через `Group.async` не годится: при занятом пуле её
//!     выполняет вызывающий, и раздача стоит (#7);
//!     threads — `std.Thread.spawn`, задачи разбиты между P потоками заранее.
//!     Печатает контрольную сумму и время параллельной фазы, нс.
//!   fqueue_par snapshot <R>
//!     писатель и R читателей снимков (`std.Thread.spawn`, `std.Io.Mutex`).
//!     Печатает контрольную сумму писателя, его время (нс), число прочитанных
//!     снимков и элементов.

const std = @import("std");
const pooled = @import("bench_options").pool;
// С пулом узлов (docs/pool.md) — тот же API; c_allocator — источник плит.
const Q = if (pooled) @import("fqueue").SyncPooledQueue(i32) else @import("fqueue").SyncQueue(i32);

/// Перед выходом потока, работавшего с очередью, — сбросить его кэш пула
/// (деструкторов у threadlocal в Zig нет).
fn flushPool() void {
    if (pooled) Q.node_pool.flushThreadCache();
}
const allocator = std.heap.c_allocator;

const Mode = enum { pool, threads };
const Scenario = enum { share, snapshot };

const tasks = 256;
const ops = 40000;
const shared_count = 64;

const snap_size = 4096;
const snap_pairs = 1000000;
const period = 512;

const Shared = [shared_count]Q;

fn xorshift(x: *u64) void {
    x.* ^= x.* << 13;
    x.* ^= x.* >> 7;
    x.* ^= x.* << 17;
}

fn mix(h: u64, x: i32) u64 {
    return h *% 31 +% @as(u64, @bitCast(@as(i64, x)));
}

fn buildShared(shared: *Shared) !void {
    var q = Q.init();
    defer q.deinit(allocator);
    var j: i32 = 0;
    while (j < shared_count * 24) : (j += 1) {
        if (@rem(j, 24) == 0) shared[@intCast(@divExact(j, 24))] = q.clone();
        const next = if (@rem(j, 3) == 2) (try q.pop(allocator)).? else try q.push(allocator, j);
        q.deinit(allocator);
        q = next;
    }
}

fn runTask(shared: *const Shared, share: u64, t: u64) !u64 {
    var x: u64 = 0x9E3779B97F4A7C15 ^ ((t + 1) *% 0xBF58476D1CE4E5B9);
    var h: u64 = 0;
    var q = Q.init();
    defer q.deinit(allocator);
    var k: i32 = 0;
    while (k < ops) : (k += 1) {
        xorshift(&x);
        const parent: *const Q = if (x % 100 < share) &shared[(x >> 8) % shared_count] else &q;
        const child = if (parent.front()) |value| blk: {
            h = mix(h, value);
            break :blk if ((x >> 40) & 1 == 0)
                (try parent.pop(allocator)).?
            else
                try parent.push(allocator, k);
        } else try parent.push(allocator, k);
        q.deinit(allocator);
        q = child;
    }
    return h;
}

// Ошибка нагрузки (нехватка памяти) фатальна; задачи пула и потоки не
// возвращают ошибок.
fn runTaskOrPanic(shared: *const Shared, share: u64, t: u64) u64 {
    return runTask(shared, share, t) catch |err| std.debug.panic("fqueue: {s}", .{@errorName(err)});
}

const Work = struct {
    shared: *const Shared,
    share: u64,
    next: std.atomic.Value(usize) = .init(0),
    results: [tasks]u64 = undefined,
};

fn poolWorker(work: *Work) void {
    while (true) {
        const t = work.next.fetchAdd(1, .monotonic);
        if (t >= tasks) return;
        work.results[t] = runTaskOrPanic(work.shared, work.share, t);
    }
}

fn idle() void {}

fn runPool(shared: *const Shared, share: u64, p: usize, elapsed: *i96) !u64 {
    // Параллельность задаёт число задач (P - 1 рабочих и вызывающий), а не
    // concurrent_limit: с пределом P - 1 рабочие прогрева, ещё не отмеченные
    // свободными, изредка давали ConcurrencyUnavailable.
    var threaded: std.Io.Threaded = .init(allocator, .{ .async_limit = .nothing });
    defer threaded.deinit();
    const io = threaded.io();
    // Потоки пула создаются до замера, как у OpenMP и rayon.
    var warmup: std.Io.Group = .init;
    for (1..p) |_| try warmup.concurrent(io, idle, .{});
    try warmup.await(io);

    var work: Work = .{ .shared = shared, .share = share };
    const t0 = std.Io.Clock.awake.now(io);
    var group: std.Io.Group = .init;
    for (1..p) |_| try group.concurrent(io, poolWorker, .{&work});
    poolWorker(&work);
    try group.await(io);
    elapsed.* = t0.durationTo(std.Io.Clock.awake.now(io)).nanoseconds;
    const results = work.results;
    var h: u64 = 0;
    for (results) |r| h +%= r;
    return h;
}

fn worker(shared: *const Shared, share: u64, first: usize, step: usize, out: *u64) void {
    var h: u64 = 0;
    var t = first;
    while (t < tasks) : (t += step) h +%= runTaskOrPanic(shared, share, t);
    out.* = h;
    flushPool();
}

fn runThreads(io: std.Io, shared: *const Shared, share: u64, p: usize, elapsed: *i96) !u64 {
    const threads = try allocator.alloc(std.Thread, p);
    defer allocator.free(threads);
    const results = try allocator.alloc(u64, p);
    defer allocator.free(results);
    const t0 = std.Io.Clock.awake.now(io);
    for (threads, results, 0..) |*thread, *out, i|
        thread.* = try std.Thread.spawn(.{}, worker, .{ shared, share, i, p, out });
    for (threads) |thread| thread.join();
    elapsed.* = t0.durationTo(std.Io.Clock.awake.now(io)).nanoseconds;
    var h: u64 = 0;
    for (results) |r| h +%= r;
    return h;
}

const Snapshot = struct {
    io: std.Io,
    mutex: std.Io.Mutex = .init,
    slot: Q,
    done: std.atomic.Value(bool) = .init(false),
};

const ReaderResult = struct { snapshots: u64 = 0, elements: u64 = 0 };

fn reader(snapshot: *Snapshot, out: *ReaderResult) void {
    defer flushPool();
    readSnapshots(snapshot, out) catch |err| std.debug.panic("fqueue: {s}", .{@errorName(err)});
}

fn readSnapshots(snapshot: *Snapshot, out: *ReaderResult) !void {
    var result: ReaderResult = .{};
    var sum: u64 = 0;
    while (!snapshot.done.load(.monotonic)) {
        // Итератор обходит копию снимка; копия снимается под мьютексом.
        snapshot.mutex.lockUncancelable(snapshot.io);
        var it = snapshot.slot.iterator();
        snapshot.mutex.unlock(snapshot.io);
        defer it.deinit(allocator);
        while (try it.next(allocator)) |value| {
            sum = mix(sum, value);
            result.elements += 1;
        }
        result.snapshots += 1;
    }
    std.mem.doNotOptimizeAway(sum);
    out.* = result;
}

fn runSnapshot(io: std.Io, readers: usize, elapsed: *i96, result: *ReaderResult) !u64 {
    var q = Q.init();
    defer q.deinit(allocator);
    var i: i32 = 0;
    while (i < snap_size) : (i += 1) try q.pushInPlace(allocator, i);
    var snapshot: Snapshot = .{ .io = io, .slot = q.clone() };
    defer snapshot.slot.deinit(allocator);
    const threads = try allocator.alloc(std.Thread, readers);
    defer allocator.free(threads);
    const results = try allocator.alloc(ReaderResult, readers);
    defer allocator.free(results);
    for (threads, results) |*thread, *out| thread.* = try std.Thread.spawn(.{}, reader, .{ &snapshot, out });
    const t0 = std.Io.Clock.awake.now(io);
    var h: u64 = 0;
    var k: i32 = 0;
    while (k < snap_pairs) : (k += 1) {
        try q.pushInPlace(allocator, snap_size + k);
        h = mix(h, (try q.popInPlace(allocator)).?);
        if (@rem(k + 1, period) == 0) {
            // Копия снимается и прежний снимок освобождается вне мьютекса.
            const copy = q.clone();
            snapshot.mutex.lockUncancelable(io);
            var old = snapshot.slot;
            snapshot.slot = copy;
            snapshot.mutex.unlock(io);
            old.deinit(allocator);
        }
    }
    elapsed.* = t0.durationTo(std.Io.Clock.awake.now(io)).nanoseconds;
    snapshot.done.store(true, .monotonic);
    result.* = .{};
    for (threads) |thread| thread.join();
    for (results) |r| {
        result.snapshots += r.snapshots;
        result.elements += r.elements;
    }
    return h;
}

fn usage() noreturn {
    std.debug.print(
        \\usage: fqueue_par share <pool|threads> <share 0..100> <threads>
        \\       fqueue_par snapshot <readers>
        \\
    , .{});
    std.process.exit(2);
}

pub fn main(init: std.process.Init) !void {
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    if (args.len < 2) usage();
    var buffer: [128]u8 = undefined;
    const text = switch (std.meta.stringToEnum(Scenario, args[1]) orelse usage()) {
        .share => text: {
            if (args.len != 5) usage();
            const mode = std.meta.stringToEnum(Mode, args[2]) orelse usage();
            const share = std.fmt.parseInt(u64, args[3], 10) catch usage();
            const p = std.fmt.parseInt(usize, args[4], 10) catch usage();
            if (share > 100 or p == 0) usage();
            var shared: Shared = undefined;
            try buildShared(&shared);
            defer for (&shared) |*q| q.deinit(allocator);
            var elapsed: i96 = undefined;
            const h = switch (mode) {
                .pool => try runPool(&shared, share, p, &elapsed),
                .threads => try runThreads(init.io, &shared, share, p, &elapsed),
            };
            break :text try std.fmt.bufPrint(&buffer, "{d} {d}\n", .{ h, elapsed });
        },
        .snapshot => text: {
            if (args.len != 3) usage();
            const readers = std.fmt.parseInt(usize, args[2], 10) catch usage();
            var elapsed: i96 = undefined;
            var result: ReaderResult = undefined;
            const h = try runSnapshot(init.io, readers, &elapsed, &result);
            break :text try std.fmt.bufPrint(&buffer, "{d} {d} {d} {d}\n", .{ h, elapsed, result.snapshots, result.elements });
        },
    };
    try std.Io.File.stdout().writeStreamingAll(init.io, text);
}
