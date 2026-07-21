const std = @import("std");
const builtin = @import("builtin");

pub fn current() std.process.Environ {
  return switch (builtin.os.tag) {
    .windows => .{ .block = .global },
    else => blk: {
      var env_count: usize = 0;
      while (std.c.environ[env_count] != null) : (env_count += 1) {}
      break :blk .{ .block = .{ .slice = std.c.environ[0..env_count :null] } };
    },
  };
}

pub fn initThreaded(allocator: std.mem.Allocator) std.Io.Threaded {
  return .init(allocator, .{ .environ = current() });
}

test "current environment preserves PATH" {
  if (builtin.os.tag == .windows) return;
  const expected = std.c.getenv("PATH") orelse return;
  const actual = try std.process.Environ.getAlloc(current(), std.testing.allocator, "PATH");
  defer std.testing.allocator.free(actual);
  try std.testing.expectEqualStrings(std.mem.span(expected), actual);
}
