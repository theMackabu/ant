const std = @import("std");
const builtin = @import("builtin");
const process_env = @import("process_env.zig");

const LibcTag = enum(u8) {
  unknown = 0,
  glibc = 1,
  musl = 2,
  none = 3,
};

const LibcDetection = struct {
  tag: LibcTag,
  cacheable: bool,
};

var cached_linux_libc = std.atomic.Value(u8).init(@intFromEnum(LibcTag.unknown));

pub fn osName() []const u8 {
  return switch (builtin.os.tag) {
    .macos => "darwin",
    .linux => "linux",
    .windows => "win32",
    .freebsd => "freebsd",
    else => "unknown",
  };
}

pub fn cpuName() []const u8 {
  return switch (builtin.cpu.arch) {
    .aarch64 => "arm64",
    .x86_64 => "x64",
    .x86 => "ia32",
    .arm => "arm",
    else => "unknown",
  };
}

pub fn abiName() []const u8 {
  if (builtin.os.tag == .linux) return libcName() orelse targetAbiName();
  return targetAbiName();
}

pub fn libcName() ?[]const u8 {
  if (builtin.os.tag != .linux) return null;

  const cached: LibcTag = @enumFromInt(cached_linux_libc.load(.acquire));
  if (cached != .unknown) return libcTagName(cached);

  const detected = detectLinuxLibcUncached();
  if (detected.cacheable) cached_linux_libc.store(@intFromEnum(detected.tag), .release);
  return libcTagName(detected.tag);
}

fn libcTagName(tag: LibcTag) ?[]const u8 {
  return switch (tag) {
    .glibc => "glibc",
    .musl => "musl",
    .none, .unknown => null,
  };
}

fn targetAbiName() []const u8 {
  return switch (builtin.abi) {
    .gnu, .gnueabi, .gnueabihf => "glibc",
    .musl, .musleabi, .musleabihf => "musl",
    else => @tagName(builtin.abi),
  };
}

fn detectLinuxLibc() LibcTag {
  return detectLinuxLibcUncached().tag;
}

fn detectLinuxLibcUncached() LibcDetection {
  var probe_failed = false;
  const commands = [_][]const []const u8{
    &.{ "getconf", "GNU_LIBC_VERSION" },
    &.{ "ldd", "--version" },
  };
  for (commands) |argv| {
    const detected = detectLibcFromCommand(argv) catch {
      probe_failed = true;
      continue;
    };
    if (detected) |tag| return .{ .tag = tag, .cacheable = true };
  }

  return fallbackLinuxLibc(probe_failed);
}

fn fallbackLinuxLibc(probe_failed: bool) LibcDetection {
  return .{
    .tag = targetAbiLibcTag() orelse .none,
    .cacheable = !probe_failed,
  };
}

fn detectLibcFromCommand(argv: []const []const u8) !?LibcTag {
  const allocator = std.heap.page_allocator;
  var threaded_io = process_env.initThreaded(allocator);
  defer threaded_io.deinit();
  const result = try std.process.run(allocator, threaded_io.io(), .{
    .argv = argv,
    .stdout_limit = .limited(64 * 1024),
    .stderr_limit = .limited(64 * 1024),
  });
  defer allocator.free(result.stdout);
  defer allocator.free(result.stderr);

  if (detectLibcFromOutput(result.stdout)) |tag| return tag;
  if (detectLibcFromOutput(result.stderr)) |tag| return tag;
  return null;
}

fn detectLibcFromOutput(content: []const u8) ?LibcTag {
  if (containsAny(content, &.{ "musl libc", "musl " })) return .musl;
  if (containsAny(content, &.{ "GNU libc", "GNU C Library", "glibc", "GLIBC" })) return .glibc;
  return null;
}

fn containsAny(haystack: []const u8, needles: []const []const u8) bool {
  for (needles) |needle| {
    if (std.mem.indexOf(u8, haystack, needle) != null) return true;
  }
  return false;
}

fn targetAbiLibcTag() ?LibcTag {
  return switch (builtin.abi) {
    .gnu, .gnueabi, .gnueabihf => .glibc,
    .musl, .musleabi, .musleabihf => .musl,
    else => null,
  };
}

test "detects libc from command output" {
  try std.testing.expectEqual(LibcTag.glibc, detectLibcFromOutput("glibc 2.39").?);
  try std.testing.expectEqual(LibcTag.glibc, detectLibcFromOutput("ldd (GNU libc) 2.39").?);
  try std.testing.expectEqual(LibcTag.glibc, detectLibcFromOutput("ldd (GNU C Library) 2.39").?);
  try std.testing.expectEqual(LibcTag.musl, detectLibcFromOutput("musl libc (x86_64)").?);
  try std.testing.expect(detectLibcFromOutput("getconf: GNU_LIBC_VERSION: unknown variable") == null);
  try std.testing.expect(detectLibcFromOutput("not an ldd marker") == null);
}

test "failed libc probes do not cache the target ABI fallback" {
  try std.testing.expect(!fallbackLinuxLibc(true).cacheable);
  try std.testing.expect(fallbackLinuxLibc(false).cacheable);
}

test "platform names use npm-compatible values" {
  try std.testing.expect(osName().len > 0);
  try std.testing.expect(cpuName().len > 0);
  try std.testing.expect(abiName().len > 0);
}

test "detectLinuxLibc detects runtime libc on linux" {
  if (builtin.os.tag == .linux) {
    const detected = detectLinuxLibc();
    try std.testing.expect(detected == .glibc or detected == .musl);
  }
}
