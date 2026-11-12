const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const lib = b.addModule("fqueue", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/root.zig"),
            .target = target,
            .optimize = .Debug,
        }),
    });
    const test_step = b.step("test", "Проверка очереди и утечек");
    test_step.dependOn(&b.addRunArtifact(tests).step);
    // Переполнение счётчика ссылок — недопустимое поведение: безопасные
    // сборки должны завершиться паникой (tests/refs_overflow.zig).
    inline for (.{ .Debug, .ReleaseSafe }) |mode| {
        inline for (.{ false, true }) |atomic| {
            const opts = b.addOptions();
            opts.addOption(bool, "atomic", atomic);
            const exe = b.addExecutable(.{
                .name = "refs_overflow",
                .root_module = b.createModule(.{
                    .root_source_file = b.path("tests/refs_overflow.zig"),
                    .target = target,
                    .optimize = mode,
                }),
            });
            exe.root_module.addImport("fqueue", b.createModule(.{
                .root_source_file = b.path("src/root.zig"),
                .target = target,
                .optimize = mode,
            }));
            exe.root_module.addOptions("refs_overflow_options", opts);
            const run = b.addRunArtifact(exe);
            run.addCheck(.{ .expect_term = .{ .signal = .ABRT } });
            run.addCheck(.{ .expect_stderr_match = "reached unreachable code" });
            test_step.dependOn(&run.step);
        }
    }

    const tsan = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = .Debug,
        .sanitize_thread = true,
    }) });
    b.step("test-tsan", "Тесты с ThreadSanitizer").dependOn(&b.addRunArtifact(tsan).step);
    // fqueue_bench[_pool][_sync]: очередь без пула и с пулом узлов
    // (docs/pool.md), обычная и потокобезопасная.
    inline for (.{ false, true }) |pool| {
        inline for (.{ false, true }) |sync| {
            const opts = b.addOptions();
            opts.addOption(bool, "sync", sync);
            opts.addOption(bool, "pool", pool);
            const exe = b.addExecutable(.{
                .name = "fqueue_bench" ++ (if (pool) "_pool" else "") ++ (if (sync) "_sync" else ""),
                .root_module = b.createModule(.{
                    .root_source_file = b.path("bench/fqueue_bench.zig"),
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                }),
            });
            exe.root_module.addImport("fqueue", lib);
            exe.root_module.addOptions("bench_options", opts);
            b.installArtifact(exe);
        }
    }

    // fqueue_par и fqueue_par_pool (пул узлов, docs/pool.md).
    inline for (.{ false, true }) |pool| {
        const par_opts = b.addOptions();
        par_opts.addOption(bool, "pool", pool);
        const par = b.addExecutable(.{
            .name = if (pool) "fqueue_par_pool" else "fqueue_par",
            .root_module = b.createModule(.{
                .root_source_file = b.path("bench/fqueue_par.zig"),
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });
        par.root_module.addImport("fqueue", lib);
        par.root_module.addOptions("bench_options", par_opts);
        b.installArtifact(par);
    }

    const fmt = b.addFmt(.{
        .paths = &.{ "build.zig", "src", "bench", "tests" },
        .check = true,
    });
    b.step("fmt", "Проверка форматирования Zig").dependOn(&fmt.step);
}
