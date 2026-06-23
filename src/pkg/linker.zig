const std = @import("std");
const io = std.Io.Threaded.global_single_threaded.io();
const builtin = @import("builtin");
const json = @import("json.zig");
const debug = @import("debug.zig");

pub const DIR_CLONE_THRESHOLD = 16;
const PARALLEL_LINK_THRESHOLD = DIR_CLONE_THRESHOLD;
const LINK_THREAD_COUNT = 8;

pub fn createSymlinkOrCopy(dir: std.Io.Dir, target: []const u8, link_name: []const u8) !void {
  try dir.symLink(io, target, link_name, .{});
}

pub fn createSymlinkAbsolute(target: []const u8, link_path: []const u8) void {
  std.Io.Dir.symLinkAbsolute(io, target, link_path, .{}) catch {};
}

fn makeExecutable(dir: std.Io.Dir, path: []const u8) !void {
  if (comptime builtin.os.tag == .windows) return;
  if (path.len >= std.Io.Dir.max_path_bytes) return error.PathTooLong;

  var path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  @memcpy(path_buf[0..path.len], path);
  path_buf[path.len] = 0;
  const path_z: [*:0]const u8 = path_buf[0..path.len :0];

  const stat = dir.statFile(io, path, .{}) catch return error.IoError;
  const mode = (stat.permissions.toMode() & 0o777) | 0o111;
  if (std.c.fchmodat(dir.handle, path_z, @intCast(mode), 0) != 0) return error.IoError;
}

pub const LinkError = error{
  IoError,
  PathNotFound,
  CrossDevice,
  PermissionDenied,
  OutOfMemory,
  PathTooLong,
};

pub const StatsSnapshot = struct {
  files_linked: u32,
  files_copied: u32,
  files_cloned: u32,
  bytes_linked: u64,
  bytes_copied: u64,
  dirs_created: u32,
  bins_linked: u32,
  packages_installed: u32,
  packages_skipped: u32,
};

pub const LinkStats = struct {
  files_linked: std.atomic.Value(u32),
  files_copied: std.atomic.Value(u32),
  files_cloned: std.atomic.Value(u32),
  bytes_linked: std.atomic.Value(u64),
  bytes_copied: std.atomic.Value(u64),
  dirs_created: std.atomic.Value(u32),
  bins_linked: std.atomic.Value(u32),
  packages_installed: std.atomic.Value(u32),
  packages_skipped: std.atomic.Value(u32),

  pub fn init() LinkStats {
    return .{
      .files_linked = std.atomic.Value(u32).init(0),
      .files_copied = std.atomic.Value(u32).init(0),
      .files_cloned = std.atomic.Value(u32).init(0),
      .bytes_linked = std.atomic.Value(u64).init(0),
      .bytes_copied = std.atomic.Value(u64).init(0),
      .dirs_created = std.atomic.Value(u32).init(0),
      .bins_linked = std.atomic.Value(u32).init(0),
      .packages_installed = std.atomic.Value(u32).init(0),
      .packages_skipped = std.atomic.Value(u32).init(0),
    };
  }

  pub fn snapshot(self: *const LinkStats) StatsSnapshot {
    return .{
      .files_linked = self.files_linked.load(.acquire),
      .files_copied = self.files_copied.load(.acquire),
      .files_cloned = self.files_cloned.load(.acquire),
      .bytes_linked = self.bytes_linked.load(.acquire),
      .bytes_copied = self.bytes_copied.load(.acquire),
      .dirs_created = self.dirs_created.load(.acquire),
      .bins_linked = self.bins_linked.load(.acquire),
      .packages_installed = self.packages_installed.load(.acquire),
      .packages_skipped = self.packages_skipped.load(.acquire),
    };
  }
};

pub const PackageLink = struct {
  cache_path: []const u8,
  node_modules_path: []const u8,
  name: []const u8,
  parent_path: ?[]const u8 = null,
  file_count: u32 = 0,
  has_bin: bool = true,
  bins: []const PackageBin = &[_]PackageBin{},
  allow_dir_symlink: bool = false,
  trust_installed: bool = false,
};

pub const PackageBin = struct {
  name: []const u8,
  path: []const u8,
};

pub const Linker = struct {
  allocator: std.mem.Allocator,
  stats: LinkStats,
  node_modules_dir: ?std.Io.Dir,
  bin_dir: ?std.Io.Dir,
  node_modules_path: []const u8,
  cross_device: std.atomic.Value(bool),
  linux_seen_exdev: std.atomic.Value(bool),
  linux_ficlone_failed: std.atomic.Value(bool),
  linux_copy_file_range_failed: std.atomic.Value(bool),
  linux_sendfile_failed: std.atomic.Value(bool),

  pub fn init(allocator: std.mem.Allocator) Linker {
    return .{
      .allocator = allocator,
      .stats = LinkStats.init(),
      .node_modules_dir = null,
      .bin_dir = null,
      .node_modules_path = "",
      .cross_device = std.atomic.Value(bool).init(false),
      .linux_seen_exdev = std.atomic.Value(bool).init(false),
      .linux_ficlone_failed = std.atomic.Value(bool).init(false),
      .linux_copy_file_range_failed = std.atomic.Value(bool).init(false),
      .linux_sendfile_failed = std.atomic.Value(bool).init(false),
    };
  }

  pub fn deinit(self: *Linker) void {
    if (self.node_modules_dir) |*d| d.close(io);
    if (self.bin_dir) |*d| d.close(io);
    if (self.node_modules_path.len > 0) self.allocator.free(self.node_modules_path);
  }

  pub fn setNodeModulesPath(self: *Linker, path: []const u8) !void {
    std.Io.Dir.cwd().createDirPath(io, path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };

    var new_nm_dir = try std.Io.Dir.cwd().openDir(io, path, .{});
    errdefer new_nm_dir.close(io);

    const new_path = try self.allocator.dupe(u8, path);
    errdefer self.allocator.free(new_path);

    const bin_path = try std.fmt.allocPrint(self.allocator, "{s}/.bin", .{path});
    defer self.allocator.free(bin_path);
    std.Io.Dir.cwd().createDirPath(io, bin_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };

    const new_bin_dir = try std.Io.Dir.cwd().openDir(io, bin_path, .{});

    if (self.bin_dir) |*d| d.close(io);
    if (self.node_modules_dir) |*d| d.close(io);
    if (self.node_modules_path.len > 0) self.allocator.free(self.node_modules_path);

    self.node_modules_dir = new_nm_dir;
    self.node_modules_path = new_path;
    self.bin_dir = new_bin_dir;
  }

  pub fn enableCopyMode(self: *Linker) void {
    self.cross_device.store(true, .release);
  }

  pub fn pathsAreCrossDevice(source_path: []const u8, dest_path: []const u8) bool {
    if (comptime builtin.os.tag != .linux) return false;

    var source_dir = std.Io.Dir.cwd().openDir(io, source_path, .{}) catch return false;
    defer source_dir.close(io);
    var dest_dir = std.Io.Dir.cwd().openDir(io, dest_path, .{}) catch return false;
    defer dest_dir.close(io);

    return dirsAreCrossDevice(source_dir, dest_dir);
  }

  pub fn linkPackage(self: *Linker, pkg: PackageLink) !void {
    const node_modules = self.node_modules_dir orelse return error.IoError;

    const install_path = if (pkg.parent_path) |parent|
      try std.fmt.allocPrint(self.allocator, "{s}/node_modules/{s}", .{ parent, pkg.name })
    else
      try self.allocator.dupe(u8, pkg.name);
    defer self.allocator.free(install_path);

    if (pkg.allow_dir_symlink and self.installedSymlinkMatches(node_modules, install_path, pkg.cache_path)) {
      _ = self.stats.packages_skipped.fetchAdd(1, .release);
      return;
    }

    var source_dir = std.Io.Dir.cwd().openDir(io, pkg.cache_path, .{ .iterate = true }) catch {
      return error.PathNotFound;
    };
    defer source_dir.close(io);

    if (!self.cross_device.load(.acquire) and dirsAreCrossDevice(source_dir, node_modules)) {
      self.enableCopyMode();
    }

    var should_skip = false;
    var has_existing_install = false;
    var has_existing_package = false;
    
    {
      const existing = node_modules.openDir(io, install_path, .{ .iterate = true }) catch null;
      if (existing) |dir| {
        has_existing_install = true;
        var installed_dir = dir;
        defer installed_dir.close(io);

        const source_version = readPackageVersion(self.allocator, source_dir);
        defer if (source_version) |v| self.allocator.free(v);

        const installed_version = readPackageVersion(self.allocator, installed_dir);
        defer if (installed_version) |v| self.allocator.free(v);
        has_existing_package = installed_version != null;
        should_skip = packageVersionsMatch(source_version, installed_version) and
          (pkg.trust_installed or
            (pkg.file_count != 0 and installedFileCountMatches(installed_dir, pkg.file_count)));
      }
    }

    if (should_skip) {
      _ = self.stats.packages_skipped.fetchAdd(1, .release);
      return;
    }

    if (has_existing_install and has_existing_package) {
      self.deleteInstalledPath(node_modules, install_path) catch return error.IoError;
    }

    const use_symlinked_cli = pkg.has_bin and self.packageSupportsSymlinkedCli(source_dir);
    const use_dir_symlink = pkg.allow_dir_symlink and (!pkg.has_bin or use_symlinked_cli);

    if (use_dir_symlink) {
      try self.symlinkPackageDirectory(node_modules, pkg.cache_path, install_path);
      if (pkg.parent_path == null and pkg.has_bin) try self.linkPackageBinaries(pkg.name, pkg.bins, use_symlinked_cli);
      _ = self.stats.packages_installed.fetchAdd(1, .release);
      return;
    }

    const replace_existing_files = has_existing_install and !has_existing_package;
    if (!replace_existing_files and pkg.file_count >= PARALLEL_LINK_THRESHOLD and
        self.clonePackageDirectory(node_modules, pkg.cache_path, install_path, pkg.file_count)) {
      if (pkg.parent_path == null and pkg.has_bin) try self.linkPackageBinaries(pkg.name, pkg.bins, false);
      _ = self.stats.packages_installed.fetchAdd(1, .release);
      return;
    }

    node_modules.createDirPath(io, install_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };
    _ = self.stats.dirs_created.fetchAdd(1, .release);

    var dest_dir = node_modules.openDir(io, install_path, .{ .iterate = true }) catch return error.IoError;
    defer dest_dir.close(io);

    self.linkDirectoryWithHint(source_dir, dest_dir, pkg.file_count, replace_existing_files) catch |err| return err;
    if (pkg.file_count != 0 and !installedFileCountMatches(dest_dir, pkg.file_count)) return error.IoError;

    if (pkg.parent_path == null and pkg.has_bin) try self.linkPackageBinaries(pkg.name, pkg.bins, false);
    _ = self.stats.packages_installed.fetchAdd(1, .release);
  }

  fn deleteInstalledPath(_: *Linker, node_modules: std.Io.Dir, install_path: []const u8) !void {
    node_modules.deleteTree(io, install_path) catch |tree_err| {
      node_modules.deleteFile(io, install_path) catch |file_err| {
        if (tree_err == error.FileNotFound or file_err == error.FileNotFound) return;
        return error.IoError;
      };
    };
  }

  fn symlinkPackageDirectory(self: *Linker, node_modules: std.Io.Dir, cache_path: []const u8, install_path: []const u8) !void {
    if (std.fs.path.dirname(install_path)) |parent| {
      node_modules.createDirPath(io, parent) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return error.IoError,
      };
    }

    node_modules.deleteFile(io, install_path) catch {};
    node_modules.symLink(io, cache_path, install_path, .{}) catch return error.IoError;
    _ = self.stats.files_linked.fetchAdd(1, .release);
    _ = self.stats.dirs_created.fetchAdd(1, .release);
  }

  fn clonePackageDirectory(self: *Linker, node_modules: std.Io.Dir, cache_path: []const u8, install_path: []const u8, file_count: u32) bool {
    if (comptime builtin.os.tag != .macos) return false;

    if (std.fs.path.dirname(install_path)) |parent| {
      node_modules.createDirPath(io, parent) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return false,
      };
    }

    const dest_path = std.fmt.allocPrintSentinel(self.allocator, "{s}/{s}", .{ self.node_modules_path, install_path }, 0) catch return false;
    defer self.allocator.free(dest_path);
    const source_path = self.allocator.dupeZ(u8, cache_path) catch return false;
    defer self.allocator.free(source_path);

    const copyfile_fn = struct {
      extern "c" fn copyfile([*:0]const u8, [*:0]const u8, ?*anyopaque, u32) c_int;
    }.copyfile;

    const COPYFILE_ACL: u32 = 1 << 0;
    const COPYFILE_STAT: u32 = 1 << 1;
    const COPYFILE_XATTR: u32 = 1 << 2;
    const COPYFILE_DATA: u32 = 1 << 3;
    const COPYFILE_ALL: u32 = COPYFILE_ACL | COPYFILE_STAT | COPYFILE_XATTR | COPYFILE_DATA;
    const COPYFILE_RECURSIVE: u32 = 1 << 15;
    const COPYFILE_CLONE: u32 = 1 << 24;

    if (copyfile_fn(source_path.ptr, dest_path.ptr, null, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE) != 0) {
      node_modules.deleteTree(io, install_path) catch {};
      return false;
    }

    _ = self.stats.files_cloned.fetchAdd(file_count, .release);
    _ = self.stats.dirs_created.fetchAdd(1, .release);
    return true;
  }

  fn installedSymlinkMatches(_: *Linker, node_modules: std.Io.Dir, install_path: []const u8, cache_path: []const u8) bool {
    var link_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const target_len = node_modules.readLink(io, install_path, &link_buf) catch return false;
    const target = link_buf[0..target_len];
    if (!std.mem.eql(u8, target, cache_path)) return false;

    std.Io.Dir.cwd().access(io, cache_path, .{}) catch return false;
    return true;
  }

  fn packageSupportsSymlinkedCli(self: *Linker, dir: std.Io.Dir) bool {
    const content = dir.readFileAlloc(io, "package.json", self.allocator, .limited(1024 * 1024)) catch return false;
    defer self.allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch return false;
    defer doc.deinit();

    const root_val = doc.root();
    if (root_val.getString("main") != null or
        root_val.getString("module") != null or
        root_val.getString("exports") != null or
        root_val.getObject("exports") != null) return false;

    if (root_val.getObject("bin")) |bin_obj| {
      var iter = bin_obj.objectIterator() orelse return false;
      defer iter.deinit();
      var count: u32 = 0;
      while (iter.next()) |entry| {
        const bin_path = entry.value.asString() orelse return false;
        if (!self.binPathIsNodeScript(dir, bin_path)) return false;
        count += 1;
      }
      return count > 0;
    }

    if (root_val.getString("bin")) |bin_path| {
      return self.binPathIsNodeScript(dir, bin_path);
    }
    return false;
  }

  fn binPathIsNodeScript(self: *Linker, dir: std.Io.Dir, bin_path: []const u8) bool {
    _ = self;
    var normalized_path = bin_path;
    if (std.mem.startsWith(u8, normalized_path, "./")) normalized_path = normalized_path[2..];
    var file = dir.openFile(io, normalized_path, .{}) catch return false;
    defer file.close(io);

    var buf: [256]u8 = undefined;
    const len = file.readPositionalAll(io, &buf, 0) catch return false;
    const content = buf[0..len];
    if (!std.mem.startsWith(u8, content, "#!")) return false;
    return std.mem.indexOf(u8, content[0..@min(content.len, 128)], "node") != null;
  }

  fn explicitBinNameIsSafe(name: []const u8) bool {
    if (name.len == 0 or std.fs.path.isAbsolute(name)) return false;
    if (std.mem.indexOfScalar(u8, name, '/') != null or std.mem.indexOfScalar(u8, name, '\\') != null) return false;
    return !std.mem.eql(u8, name, ".") and !std.mem.eql(u8, name, "..");
  }

  fn explicitBinPathIsSafe(path: []const u8) bool {
    if (path.len == 0 or std.fs.path.isAbsolute(path)) return false;
    var normalized = path;
    while (std.mem.startsWith(u8, normalized, "./")) normalized = normalized[2..];
    if (normalized.len == 0) return false;

    var parts = std.mem.splitAny(u8, normalized, "/\\");
    while (parts.next()) |part| {
      if (part.len == 0 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return false;
    }
    return true;
  }

  fn explicitBinIsSafe(bin: PackageBin) bool {
    return explicitBinNameIsSafe(bin.name) and explicitBinPathIsSafe(bin.path);
  }

  fn linkBinaries(self: *Linker, pkg_name: []const u8, preserve_symlinks_main: bool) !void {
    const bin_dir = self.bin_dir orelse return;
    const node_modules = self.node_modules_dir orelse return;

    var pkg_dir = node_modules.openDir(io, pkg_name, .{}) catch return;
    defer pkg_dir.close(io);

    const content = pkg_dir.readFileAlloc(io, "package.json", self.allocator, .limited(1024 * 1024)) catch return;
    defer self.allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch return;
    defer doc.deinit();

    const root_val = doc.root();

    if (root_val.getObject("bin")) |bin_obj| {
      var iter = bin_obj.objectIterator() orelse return;
      while (iter.next()) |entry| {
        const bin_path = entry.value.asString() orelse continue;
        if (preserve_symlinks_main)
          self.createBinWrapper(pkg_name, entry.key, bin_path, bin_dir) catch continue
        else
          self.createBinSymlink(pkg_name, entry.key, bin_path, bin_dir) catch continue;
      }
    } else if (root_val.getString("bin")) |bin_path| {
      const simple_name = if (std.mem.indexOf(u8, pkg_name, "/")) |slash|
        pkg_name[slash + 1 ..]
      else
        pkg_name;
      if (preserve_symlinks_main)
        self.createBinWrapper(pkg_name, simple_name, bin_path, bin_dir) catch {}
      else
        self.createBinSymlink(pkg_name, simple_name, bin_path, bin_dir) catch {};
    }
  }

  fn linkPackageBinaries(self: *Linker, pkg_name: []const u8, bins: []const PackageBin, preserve_symlinks_main: bool) !void {
    if (bins.len == 0) return self.linkBinaries(pkg_name, preserve_symlinks_main);

    const bin_dir = self.bin_dir orelse return;
    for (bins) |bin| {
      if (!explicitBinIsSafe(bin)) continue;
      if (preserve_symlinks_main)
        try self.createBinWrapper(pkg_name, bin.name, bin.path, bin_dir)
      else
        try self.createBinSymlink(pkg_name, bin.name, bin.path, bin_dir);
    }
  }

  fn readPackageVersion(allocator: std.mem.Allocator, dir: std.Io.Dir) ?[]const u8 {
    const content = dir.readFileAlloc(io, "package.json", allocator, .limited(256 * 1024)) catch return null;
    defer allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch return null;
    defer doc.deinit();

    const version = doc.root().getString("version") orelse return null;
    return allocator.dupe(u8, version) catch null;
  }

  fn packageVersionsMatch(source_version: ?[]const u8, installed_version: ?[]const u8) bool {
    const src = source_version orelse return false;
    const dst = installed_version orelse return false;
    return std.mem.eql(u8, src, dst);
  }

  fn installedFileCountMatches(dir: std.Io.Dir, expected_files: u32) bool {
    if (expected_files == 0) return true;
    const actual_files = countFilesRecursive(dir) catch return false;
    return actual_files >= expected_files;
  }

  fn countFilesRecursive(dir: std.Io.Dir) !u32 {
    var count: u32 = 0;
    var iter = dir.iterate();
    while (try iter.next(io)) |entry| {
    switch (entry.kind) {
      .file => count += 1,
      .directory => {
        var child = dir.openDir(io, entry.name, .{ .iterate = true }) catch continue;
        defer child.close(io);
        count += try countFilesRecursive(child);
      },
      else => {},
    }}
    return count;
  }

  fn createBinSymlink(self: *Linker, pkg_name: []const u8, cmd_name: []const u8, bin_path: []const u8, bin_dir: std.Io.Dir) !void {
    var normalized_path = bin_path;
    if (std.mem.startsWith(u8, normalized_path, "./")) {
      normalized_path = normalized_path[2..];
    }

    const target = try std.fmt.allocPrint(self.allocator, "../{s}/{s}", .{ pkg_name, normalized_path });
    defer self.allocator.free(target);

    bin_dir.deleteFile(io, cmd_name) catch {};
    makeExecutable(bin_dir, target) catch {};
    try createSymlinkOrCopy(bin_dir, target, cmd_name);

    _ = self.stats.bins_linked.fetchAdd(1, .release);
  }

  fn createBinWrapper(self: *Linker, pkg_name: []const u8, cmd_name: []const u8, bin_path: []const u8, bin_dir: std.Io.Dir) !void {
    var normalized_path = bin_path;
    if (std.mem.startsWith(u8, normalized_path, "./")) {
      normalized_path = normalized_path[2..];
    }

    const target = try std.fmt.allocPrint(self.allocator, "../{s}/{s}", .{ pkg_name, normalized_path });
    defer self.allocator.free(target);
    const script = try std.fmt.allocPrint(self.allocator,
      "#!/bin/sh\nbasedir=$(dirname \"$0\")\nexec node --preserve-symlinks-main \"$basedir/{s}\" \"$@\"\n",
      .{target},
    );
    defer self.allocator.free(script);

    bin_dir.deleteFile(io, cmd_name) catch {};
    var file = try bin_dir.createFile(io, cmd_name, if (comptime builtin.os.tag == .windows) .{} else .{ .permissions = .executable_file });
    defer file.close(io);
    try file.writeStreamingAll(io, script);

    _ = self.stats.bins_linked.fetchAdd(1, .release);
  }

  const FileWorkItem = struct {
    source_path: []const u8,
    dest_path: []const u8,
    kind: std.Io.File.Kind,
    link_target: ?[]const u8,
  };

  fn linkDirectory(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, replace_existing: bool) !void {
    try self.linkDirectorySequential(source, dest, replace_existing);
  }

  pub fn linkDirectoryWithHint(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, file_count: u32, replace_existing: bool) !void {
    if (!self.cross_device.load(.acquire) and file_count >= PARALLEL_LINK_THRESHOLD) {
      try self.linkDirectoryParallel(source, dest, replace_existing);
    } else try self.linkDirectorySequential(source, dest, replace_existing);
  }

  fn linkDirectorySequential(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, replace_existing: bool) !void {
    var iter = source.iterate();
    while (try iter.next(io)) |entry| {
      switch (entry.kind) {
        .directory => {
          dest.createDirPath(io, entry.name) catch |err| switch (err) {
            error.PathAlreadyExists => {},
            else => return error.IoError,
          };
          _ = self.stats.dirs_created.fetchAdd(1, .release);

          var child_source = source.openDir(io, entry.name, .{ .iterate = true }) catch continue;
          defer child_source.close(io);

          var child_dest = dest.openDir(io, entry.name, .{}) catch continue;
          defer child_dest.close(io);

          try self.linkDirectorySequential(child_source, child_dest, replace_existing);
        },
        .file => try self.linkFile(source, dest, entry.name, replace_existing),
        .sym_link => {
          var link_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
          const target_len = source.readLink(io, entry.name, &link_buf) catch continue;
          const target = link_buf[0..target_len];
          createSymlinkOrCopy(dest, target, entry.name) catch {};
        },
        else => {},
      }
    }
  }

  const ParallelThreadContext = struct {
    linker: *Linker,
    items: []const FileWorkItem,
    source_base: std.Io.Dir,
    dest_base: std.Io.Dir,
    replace_existing: bool,
  };

  fn linkDirectoryParallel(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, replace_existing: bool) !void {
    var work_items = std.ArrayListUnmanaged(FileWorkItem).empty;
    defer {
      for (work_items.items) |item| {
        self.allocator.free(item.source_path);
        self.allocator.free(item.dest_path);
        if (item.link_target) |t| self.allocator.free(t);
      }
      work_items.deinit(self.allocator);
    }

    try self.collectWorkItems(source, dest, "", &work_items);
    if (work_items.items.len == 0) return;

    const items_slice = work_items.items;
    const chunk_size = (items_slice.len + LINK_THREAD_COUNT - 1) / LINK_THREAD_COUNT;

    var threads: [LINK_THREAD_COUNT]?std.Thread = undefined;
    for (&threads) |*t| t.* = null;
    var contexts: [LINK_THREAD_COUNT]ParallelThreadContext = undefined;

    var offset: usize = 0;
    for (0..LINK_THREAD_COUNT) |i| {
      if (offset >= items_slice.len) break;
      const end = @min(offset + chunk_size, items_slice.len);

      contexts[i] = .{
        .linker = self,
        .items = items_slice[offset..end],
        .source_base = source,
        .dest_base = dest,
        .replace_existing = replace_existing,
      };

      threads[i] = std.Thread.spawn(.{}, processWorkItems, .{&contexts[i]}) catch |err| blk: {
        debug.log("Thread spawn failed for chunk {d}-{d}: {s}", .{ offset, end, @errorName(err) });
        processWorkItems(&contexts[i]);
        break :blk null;
      }; offset = end;
    }

    for (&threads) |*t| if (t.*) |thread| thread.join();
  }

  fn processWorkItems(ctx: *const ParallelThreadContext) void {
    for (ctx.items) |item| {
      switch (item.kind) {
        .file => {
          const src_dir_path = std.fs.path.dirname(item.source_path) orelse "";
          const dst_dir_path = std.fs.path.dirname(item.dest_path) orelse "";
          const filename = std.fs.path.basename(item.source_path);

          var src_dir = if (src_dir_path.len > 0)
            ctx.source_base.openDir(io, src_dir_path, .{}) catch continue
          else
            ctx.source_base;
          defer if (src_dir_path.len > 0) src_dir.close(io);

          var dst_dir = if (dst_dir_path.len > 0)
            ctx.dest_base.openDir(io, dst_dir_path, .{}) catch continue
          else
            ctx.dest_base;
          defer if (dst_dir_path.len > 0) dst_dir.close(io);

          ctx.linker.linkFile(src_dir, dst_dir, filename, ctx.replace_existing) catch {};
        },
        .sym_link => {
          if (item.link_target) |target| {
            const dst_dir_path = std.fs.path.dirname(item.dest_path) orelse "";
            const filename = std.fs.path.basename(item.dest_path);

            var dst_dir = if (dst_dir_path.len > 0)
              ctx.dest_base.openDir(io, dst_dir_path, .{}) catch continue
            else
              ctx.dest_base;
            defer if (dst_dir_path.len > 0) dst_dir.close(io);
            createSymlinkOrCopy(dst_dir, target, filename) catch {};
          }
        },
        else => {},
      }
    }
  }

  fn collectWorkItems(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, prefix: []const u8, work_items: *std.ArrayListUnmanaged(FileWorkItem)) !void {
    var iter = source.iterate();
    while (try iter.next(io)) |entry| {
      const rel_path = if (prefix.len > 0)
        try std.fmt.allocPrint(self.allocator, "{s}/{s}", .{ prefix, entry.name })
      else
        try self.allocator.dupe(u8, entry.name);
      errdefer self.allocator.free(rel_path);

      switch (entry.kind) {
        .directory => {
          dest.createDirPath(io, rel_path) catch |err| switch (err) {
            error.PathAlreadyExists => {},
            else => {
              self.allocator.free(rel_path);
              return error.IoError;
            },
          };
          _ = self.stats.dirs_created.fetchAdd(1, .release);

          var child_source = source.openDir(io, entry.name, .{ .iterate = true }) catch {
            self.allocator.free(rel_path);
            continue;
          };
          defer child_source.close(io);

          try self.collectWorkItems(child_source, dest, rel_path, work_items);
          self.allocator.free(rel_path);
        },
        .file => {
          try work_items.append(self.allocator, .{
            .source_path = rel_path,
            .dest_path = try self.allocator.dupe(u8, rel_path),
            .kind = .file,
            .link_target = null,
          });
        },
        .sym_link => {
          var link_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
          const target_len = source.readLink(io, entry.name, &link_buf) catch {
            self.allocator.free(rel_path);
            continue;
          };
          const target = link_buf[0..target_len];
          try work_items.append(self.allocator, .{
            .source_path = rel_path,
            .dest_path = try self.allocator.dupe(u8, rel_path),
            .kind = .sym_link,
            .link_target = try self.allocator.dupe(u8, target),
          });
        },
        else => self.allocator.free(rel_path),
      }
    }
  }

  fn linkFile(self: *Linker, source_dir: std.Io.Dir, dest_dir: std.Io.Dir, name: []const u8, replace_existing: bool) !void {
    if (replace_existing) dest_dir.deleteFile(io, name) catch {};

    if (comptime builtin.os.tag != .windows) {
      if (!self.cross_device.load(.acquire)) {
        if (linkAt(source_dir, name, dest_dir, name)) {
          _ = self.stats.files_linked.fetchAdd(1, .release);
          return;
        } else |err| {
          if (err == error.CrossDevice) {
            self.cross_device.store(true, .release);
          } else if (err != error.IoError) return err;
        }
      }
    }

    if (comptime builtin.os.tag == .macos) {
      if (fclonefileat(source_dir.handle, name, dest_dir.handle, name)) {
        _ = self.stats.files_cloned.fetchAdd(1, .release);
        return;
      } else |_| {}
    }

    try self.copyFile(source_dir, dest_dir, name);
    _ = self.stats.files_copied.fetchAdd(1, .release);
  }

  fn linkAt(source_dir: std.Io.Dir, source_name: []const u8, dest_dir: std.Io.Dir, dest_name: []const u8) !void {
    if (comptime builtin.os.tag == .windows) return error.IoError;
    
    var source_buf: [256]u8 = undefined;
    var dest_buf: [256]u8 = undefined;

    if (source_name.len >= source_buf.len or dest_name.len >= dest_buf.len) {
      return error.PathTooLong;
    }

    @memcpy(source_buf[0..source_name.len], source_name);
    source_buf[source_name.len] = 0;

    @memcpy(dest_buf[0..dest_name.len], dest_name);
    dest_buf[dest_name.len] = 0;

    const source_z: [*:0]const u8 = source_buf[0..source_name.len :0];
    const dest_z: [*:0]const u8 = dest_buf[0..dest_name.len :0];

    const result = std.c.linkat(source_dir.handle, source_z, dest_dir.handle, dest_z, 0);
    if (result != 0) {
      const errno = std.c.errno(result);
      return switch (errno) {
        .XDEV => error.CrossDevice,
        .PERM, .ACCES => error.IoError,
        else => error.IoError,
      };
    }
  }

  fn dirsAreCrossDevice(source_dir: std.Io.Dir, dest_dir: std.Io.Dir) bool {
    if (comptime builtin.os.tag != .linux) return false;

    var source_stat: std.os.linux.Statx = undefined;
    var dest_stat: std.os.linux.Statx = undefined;
    const flags = std.os.linux.AT.EMPTY_PATH | std.os.linux.AT.STATX_DONT_SYNC;
    if (std.c.statx(source_dir.handle, "", flags, std.os.linux.STATX.BASIC_STATS, &source_stat) != 0) return false;
    if (std.c.statx(dest_dir.handle, "", flags, std.os.linux.STATX.BASIC_STATS, &dest_stat) != 0) return false;

    return source_stat.dev_major != dest_stat.dev_major or source_stat.dev_minor != dest_stat.dev_minor;
  }

  fn fclonefileat(src_fd: std.posix.fd_t, src_name: []const u8, dst_fd: std.posix.fd_t, dst_name: []const u8) !void {
    var src_buf: [256]u8 = undefined;
    var dst_buf: [256]u8 = undefined;

    if (src_name.len >= src_buf.len or dst_name.len >= dst_buf.len) {
      return error.PathTooLong;
    }

    @memcpy(src_buf[0..src_name.len], src_name);
    src_buf[src_name.len] = 0;

    @memcpy(dst_buf[0..dst_name.len], dst_name);
    dst_buf[dst_name.len] = 0;

    const src_z: [*:0]const u8 = src_buf[0..src_name.len :0];
    const dst_z: [*:0]const u8 = dst_buf[0..dst_name.len :0];

    const fclonefileat_fn = struct {
      extern "c" fn fclonefileat(c_int, [*:0]const u8, c_int, [*:0]const u8, c_uint) c_int;
    }.fclonefileat;

    const result = fclonefileat_fn(src_fd, src_z, dst_fd, dst_z, 0);
    if (result != 0) {
      return error.IoError;
    }
  }

  fn copyFile(self: *Linker, source_dir: std.Io.Dir, dest_dir: std.Io.Dir, name: []const u8) !void {
    var source = source_dir.openFile(io, name, .{}) catch return error.IoError;
    defer source.close(io);

    const stat = source.stat(io) catch return error.IoError;
    var dest = dest_dir.createFile(io, name, 
      if (comptime builtin.os.tag != .windows) .{ .permissions = stat.permissions } else .{}
    ) catch return error.IoError; defer dest.close(io);

    const bytes_copied = if (comptime builtin.os.tag == .linux)
      try self.copyFileLinux(source.handle, dest.handle)
    else
      try copyFileBuffered(source, dest);

    _ = self.stats.bytes_copied.fetchAdd(bytes_copied, .release);
  }

  fn copyFileLinux(self: *Linker, source_fd: std.posix.fd_t, dest_fd: std.posix.fd_t) !u64 {
    if (self.tryLinuxFiclone(source_fd, dest_fd)) return 0;

    var total: u64 = 0;
    while (self.tryLinuxCopyFileRange(source_fd, dest_fd)) |copied| {
      if (copied == 0) return total;
      total += copied;
    }

    while (self.tryLinuxSendfile(source_fd, dest_fd)) |copied| {
      if (copied == 0) return total;
      total += copied;
    }

    return total + try copyFileBufferedFd(source_fd, dest_fd);
  }

  fn tryLinuxFiclone(self: *Linker, source_fd: std.posix.fd_t, dest_fd: std.posix.fd_t) bool {
    if (self.linux_seen_exdev.load(.acquire) or self.linux_ficlone_failed.load(.acquire)) return false;

    const linux = std.os.linux;
    const FICLONE = linux.IOCTL.IOW(0x94, 9, c_int);
    while (true) {
      const rc = linux.ioctl(dest_fd, FICLONE, @as(usize, @bitCast(@as(isize, source_fd))));
      const err = linux.errno(rc);
      if (err == .SUCCESS) return true;
      if (err == .INTR) continue;
      if (err == .XDEV) {
        self.linux_seen_exdev.store(true, .release);
      } else {
        self.linux_ficlone_failed.store(true, .release);
      }
      return false;
    }
  }

  fn tryLinuxCopyFileRange(self: *Linker, source_fd: std.posix.fd_t, dest_fd: std.posix.fd_t) ?u64 {
    if (self.linux_seen_exdev.load(.acquire) or self.linux_copy_file_range_failed.load(.acquire)) return null;

    const linux = std.os.linux;
    while (true) {
      const rc = linux.copy_file_range(source_fd, null, dest_fd, null, std.math.maxInt(i32) - 1, 0);
      const err = linux.errno(rc);
      if (err == .SUCCESS) return @intCast(rc);
      if (err == .INTR) continue;
      if (err == .XDEV) {
        self.linux_seen_exdev.store(true, .release);
      }
      self.linux_copy_file_range_failed.store(true, .release);
      return null;
    }
  }

  fn tryLinuxSendfile(self: *Linker, source_fd: std.posix.fd_t, dest_fd: std.posix.fd_t) ?u64 {
    if (self.linux_sendfile_failed.load(.acquire)) return null;

    const linux = std.os.linux;
    while (true) {
      const rc = linux.sendfile(dest_fd, source_fd, null, std.math.maxInt(i32) - 1);
      const err = linux.errno(rc);
      if (err == .SUCCESS) return @intCast(rc);
      if (err == .INTR) continue;
      if (err == .XDEV) {
        self.linux_seen_exdev.store(true, .release);
      }
      self.linux_sendfile_failed.store(true, .release);
      return null;
    }
  }

  fn copyFileBuffered(source: std.Io.File, dest: std.Io.File) !u64 {
    var total: u64 = 0;
    var buf: [64 * 1024]u8 = undefined;
    while (true) {
      const bytes_read = source.readStreaming(io, &.{&buf}) catch |err| switch (err) {
        error.EndOfStream => break,
        else => return error.IoError,
      };
      dest.writeStreamingAll(io, buf[0..bytes_read]) catch return error.IoError;
      total += bytes_read;
    }
    return total;
  }

  fn copyFileBufferedFd(source_fd: std.posix.fd_t, dest_fd: std.posix.fd_t) !u64 {
    var total: u64 = 0;
    var buf: [64 * 1024]u8 = undefined;
    while (true) {
      const read_rc = std.os.linux.read(source_fd, &buf, buf.len);
      const err = std.os.linux.errno(read_rc);
      if (err == .INTR) continue;
      if (err != .SUCCESS) return error.IoError;
      if (read_rc == 0) return total;

      try writeAllFd(dest_fd, buf[0..read_rc]);
      total += read_rc;
    }
  }

  fn writeAllFd(fd: std.posix.fd_t, bytes: []const u8) !void {
    var written: usize = 0;
    while (written < bytes.len) {
      const rc = std.os.linux.write(fd, bytes[written..].ptr, bytes.len - written);
      const err = std.os.linux.errno(rc);
      if (err == .INTR) continue;
      if (err != .SUCCESS or rc == 0) return error.IoError;
      written += rc;
    }
  }

  pub fn getStats(self: *const Linker) StatsSnapshot {
    return self.stats.snapshot();
  }
};
