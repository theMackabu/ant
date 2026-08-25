const std = @import("std");

fn darwinMinVersion(os_tag: ?std.Target.Os.Tag) ?std.Target.Query.OsVersion {
  const tag = os_tag orelse return null;
  if (tag != .macos) return null;
  return .{ .semver = .{ .major = 15, .minor = 0, .patch = 0 } };
}

pub fn build(b: *std.Build) void {
  const resolved_target = b.standardTargetOptions(.{
    .default_target = .{
      .os_version_min = darwinMinVersion(b.graph.host.result.os.tag),
      .cpu_model = .baseline,
    },
  });

  const lmdb_include = b.graph.environ_map.get("LMDB_INCLUDE");
  const zlib_include = b.graph.environ_map.get("ZLIB_INCLUDE");
  const libuv_include = b.graph.environ_map.get("LIBUV_INCLUDE");
  const yyjson_include = b.graph.environ_map.get("YYJSON_INCLUDE");

  const lib = b.addLibrary(.{
    .name = "pkg",
    .root_module = b.createModule(.{
      .root_source_file = b.path("root.zig"),
      .target = resolved_target,
      .optimize = .ReleaseSmall,
      .link_libc = true,
      .link_libcpp = true,
      .omit_frame_pointer = true,
      .unwind_tables = .none,
      .strip = true,
    }),
  });

  lib.use_llvm = true;
  if (!resolved_target.result.os.tag.isDarwin()) lib.use_lld = true;

  lib.root_module.addCSourceFile(.{
    .file = b.path("metadata.c"),
    .flags = &.{ "-O3", "-DNDEBUG" },
  });
  
  const version = b.graph.environ_map.get("ANT_VERSION") orelse "unknown";
  const options = b.addOptions();
  options.addOption([]const u8, "version", version);
  
  lib.root_module.addOptions("config", options);
  lib.root_module.addCMacro("NDEBUG", "1");
  lib.root_module.addCMacro("YYJSON_DISABLE_UTILS", "1");

  if (lmdb_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (zlib_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (libuv_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });
  if (yyjson_include) |p| lib.root_module.addIncludePath(.{ .cwd_relative = p });

  b.installArtifact(lib);
}
