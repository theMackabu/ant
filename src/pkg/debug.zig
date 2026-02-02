const std = @import("std");
const builtin = @import("builtin");

pub var enabled: bool = false;

pub fn log(comptime fmt: []const u8, args: anytype) void {
  if (!enabled) return;
  
  var buf: [2048]u8 = undefined;
  const msg = std.fmt.bufPrint(&buf, "[pkg] " ++ fmt ++ "\n", args) catch return;
  
  if (builtin.os.tag == .windows) {
    const stderr = std.io.getStdErr();
    stderr.writeAll(msg) catch {};
  } else _ = std.c.write(2, msg.ptr, msg.len);
}

pub fn timer(comptime label: []const u8, start: u64) u64 {
  if (!enabled) return start;
  
  const now = std.time.nanoTimestamp();
  const elapsed_ns: u64 = @intCast(now - @as(i128, start));
  const elapsed_ms = elapsed_ns / 1_000_000;
  
  log("{s}: {d}ms", .{ label, elapsed_ms });
  
  return @intCast(now);
}
