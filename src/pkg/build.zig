const std = @import("std");

fn getEnv(key: []const u8) ?[]const u8 {
  return std.process.getEnvVarOwned(std.heap.page_allocator, key) catch null;
}

pub fn build(b: *std.Build) void {
  const resolved_target = blk: {
    var query: std.Target.Query = .{};

    query.cpu_arch = .aarch64;
    query.os_tag = .macos;
    query.os_version_min = .{ .semver = .{ .major = 15, .minor = 0, .patch = 0 } };
    query.cpu_model = .native;

    break :blk b.resolveTargetQuery(query);
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

  lib.root_module.addCMacro("NDEBUG", "1");

  if (lmdb_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (zlib_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (libuv_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (yyjson_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });

  b.installArtifact(lib);
}
