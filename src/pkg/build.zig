const std = @import("std");

fn getEnv(key: []const u8) ?[]const u8 {
  return std.process.getEnvVarOwned(std.heap.page_allocator, key) catch null;
}

pub fn build(b: *std.Build) void {
  const resolved_target = blk: {
    if (getEnv("PKG_TARGET")) |target_str| {
      defer std.heap.page_allocator.free(target_str);
      var query: std.Target.Query = .{};

      var it = std.mem.splitScalar(u8, target_str, '-');
      if (it.next()) |arch| query.cpu_arch = std.meta.stringToEnum(std.Target.Cpu.Arch, arch);
      if (it.next()) |os| query.os_tag = std.meta.stringToEnum(std.Target.Os.Tag, os);

      query.cpu_model = .baseline;
      break :blk b.resolveTargetQuery(query);
    } else break :blk b.standardTargetOptions(.{});
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
  lib.use_lld = true;
  
  const version = std.posix.getenv("ANT_VERSION") orelse "unknown";
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
