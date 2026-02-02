const std = @import("std");

fn getEnv(key: []const u8) ?[]const u8 {
  return std.process.getEnvVarOwned(std.heap.page_allocator, key) catch null;
}

fn darwinMinVersion(os_tag: ?std.Target.Os.Tag) ?std.Target.Query.OsVersion {
  const tag = os_tag orelse return null;
  if (!tag.isDarwin()) return null;
  return .{ .semver = .{ .major = 15, .minor = 0, .patch = 0 } };
}

pub fn build(b: *std.Build) void {
  const resolved_target = blk: {
    const target_str = getEnv("PKG_TARGET") orelse break :blk b.standardTargetOptions(.{});
    defer std.heap.page_allocator.free(target_str);
    var it = std.mem.splitScalar(u8, target_str, '-');
    
    const cpu_arch = if (it.next()) |a| 
      std.meta.stringToEnum(std.Target.Cpu.Arch, a) else null;
    
    const os_tag = if (it.next()) |o| blk2: {
      if (std.mem.eql(u8, o, "darwin")) break :blk2 std.Target.Os.Tag.macos;
      break :blk2 std.meta.stringToEnum(std.Target.Os.Tag, o);
    } else null;

    std.debug.print("[zig.build] cpu_arch: {?}\n", .{cpu_arch});
    std.debug.print("[zig.build] os_tag: {?}\n", .{os_tag});

    break :blk b.resolveTargetQuery(.{
      .cpu_arch = cpu_arch,
      .os_tag = os_tag,
      .os_version_min = darwinMinVersion(os_tag),
      .cpu_model = .baseline,
    });
  };

  const lmdb_include = getEnv("LMDB_INCLUDE");
  const zlib_include = getEnv("ZLIB_INCLUDE");
  const libuv_include = getEnv("LIBUV_INCLUDE");
  const yyjson_include = getEnv("YYJSON_INCLUDE");

  const lib = b.addLibrary(.{
    .name = "pkg",
    .root_module = b.createModule(.{
      .root_source_file = b.path("root.zig"),
      .target = resolved_target,
      .optimize = .ReleaseFast,
      .link_libc = true,
      .link_libcpp = true,
      .omit_frame_pointer = true,
      .unwind_tables = .none,
      .strip = true,
    }),
  });

  lib.use_llvm = true;
  if (!resolved_target.result.os.tag.isDarwin()) lib.use_lld = true;
  
  const version = getEnv("ANT_VERSION") orelse "unknown";
  const options = b.addOptions();
  options.addOption([]const u8, "version", version);
  
  lib.root_module.addOptions("config", options);
  lib.root_module.addCMacro("NDEBUG", "1");

  if (lmdb_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (zlib_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (libuv_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (yyjson_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });

  b.installArtifact(lib);
}
