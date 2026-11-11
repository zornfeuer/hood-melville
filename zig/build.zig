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
    b.step("test", "Проверка очереди и утечек").dependOn(&b.addRunArtifact(tests).step);

    const tsan = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = .Debug,
        .sanitize_thread = true,
    }) });
    b.step("test-tsan", "Тесты с ThreadSanitizer").dependOn(&b.addRunArtifact(tsan).step);
    inline for (.{ false, true }) |sync| {
        const opts = b.addOptions();
        opts.addOption(bool, "sync", sync);
        const exe = b.addExecutable(.{
            .name = if (sync) "fqueue_bench_sync" else "fqueue_bench",
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

    const fmt = b.addFmt(.{
        .paths = &.{ "build.zig", "src", "bench" },
        .check = true,
    });
    b.step("fmt", "Проверка форматирования Zig").dependOn(&fmt.step);
}
