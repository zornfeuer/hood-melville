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

    const exe = b.addExecutable(.{
        .name = "fqueue_bench",
        .root_module = b.createModule(.{
            .root_source_file = b.path("bench/fqueue_bench.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    exe.root_module.addImport("fqueue", lib);
    b.installArtifact(exe);

    const fmt = b.addFmt(.{
        .paths = &.{ "build.zig", "src", "bench" },
        .check = true,
    });
    b.step("fmt", "Проверка форматирования Zig").dependOn(&fmt.step);
}
