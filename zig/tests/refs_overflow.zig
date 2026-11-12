//! Переполнение счётчика ссылок — недопустимое поведение: безопасные сборки
//! (Debug, ReleaseSafe) обязаны его поймать. `zig build test` запускает эту
//! программу и ждёт паники; предел снижен, чтобы дойти до него сразу.
const std = @import("std");
const fqueue = @import("fqueue");
const options = @import("refs_overflow_options");

pub fn main() !void {
    const Q = fqueue.QueueWithOptions(i32, .{ .atomic = options.atomic, .refs_limit = 8 });
    const a = std.heap.page_allocator;
    var q = try Q.init().push(a, 1);
    var copies: [8]Q = undefined;
    // Восьмая копия — девятая ссылка на узел.
    for (&copies) |*copy| copy.* = q.clone();
    std.process.exit(0); // недостижимо в безопасных сборках
}
