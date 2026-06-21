const std = @import("std");
const io = std.Io.Threaded.global_single_threaded.io();

pub var enabled: bool = false;

pub fn nowNs() u64 {
  return @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds());
}

pub fn elapsedMsSince(start: u64) u64 {
  const now = std.Io.Timestamp.now(io, .boot).toNanoseconds();
  const elapsed_ns: u64 = @intCast(now - @as(i128, start));
  return elapsed_ns / 1_000_000;
}

pub fn elapsedUsSince(start: u64) u64 {
  const now = std.Io.Timestamp.now(io, .boot).toNanoseconds();
  const elapsed_ns: u64 = @intCast(now - @as(i128, start));
  return elapsed_ns / 1_000;
}

pub fn log(comptime fmt: []const u8, args: anytype) void {
  if (!enabled) return;
  
  var buf: [2048]u8 = undefined;
  const msg = std.fmt.bufPrint(&buf, "[pkg] " ++ fmt ++ "\n", args) catch return;
  std.Io.File.stderr().writeStreamingAll(io, msg) catch {};
}

pub fn trace(comptime fmt: []const u8, args: anytype) void {
  if (!enabled) return;

  var buf: [2048]u8 = undefined;
  const msg = std.fmt.bufPrint(&buf, "[pkg:trace] " ++ fmt ++ "\n", args) catch return;
  std.Io.File.stderr().writeStreamingAll(io, msg) catch {};
}

pub const StageTrace = struct {
  slowest_label: []const u8 = "none",
  slowest_us: u64 = 0,

  pub fn mark(self: *StageTrace, comptime label: []const u8, start: u64) u64 {
    const elapsed_us = elapsedUsSince(start);
    if (elapsed_us > self.slowest_us) {
      self.slowest_us = elapsed_us;
      self.slowest_label = label;
    }
    return nowNs();
  }

  pub fn summary(self: StageTrace, comptime label: []const u8) void {
    trace("{s} slowest={s} {d}us", .{ label, self.slowest_label, self.slowest_us });
  }
};

pub fn timer(comptime label: []const u8, start: u64) u64 {
  if (!enabled) return start;
  
  const now = nowNs();
  const elapsed_ms = elapsedMsSince(start);
  
  log("{s}: {d}ms", .{ label, elapsed_ms });
  
  return now;
}
