const std = @import("std");
const io = std.Io.Threaded.global_single_threaded.io();
const builtin = @import("builtin");
const json = @import("json.zig");
const debug = @import("debug.zig");

const PARALLEL_LINK_THRESHOLD = 500;
const LINK_THREAD_COUNT = 8;

pub fn createSymlinkOrCopy(dir: std.Io.Dir, target: []const u8, link_name: []const u8) !void {
  if (comptime builtin.os.tag == .windows) {
    try createSymlinkWindows(dir, target, link_name);
  } else try dir.symLink(io, target, link_name, .{});
}

pub fn createSymlinkAbsolute(target: []const u8, link_path: []const u8) void {
  if (comptime builtin.os.tag == .windows) {
    createSymlinkAbsoluteWindows(target, link_path);
  } else std.Io.Dir.symLinkAbsolute(io, target, link_path, .{}) catch {};
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

fn createSymlinkWindows(dir: std.Io.Dir, target: []const u8, link_name: []const u8) !void {
  if (comptime builtin.os.tag != .windows) return;
  var target_utf16: [std.Io.Dir.max_path_bytes]u16 = undefined;
  var link_utf16: [std.Io.Dir.max_path_bytes]u16 = undefined;
  const target_len = try std.unicode.utf8ToUtf16Le(&target_utf16, target);
  const link_len = try std.unicode.utf8ToUtf16Le(&link_utf16, link_name);
  target_utf16[target_len] = 0;
  
  _ = try std.os.windows.CreateSymbolicLink(
    dir.fd, link_utf16[0..link_len],
    target_utf16[0..target_len :0], false,
  );
}

fn createSymlinkAbsoluteWindows(target: []const u8, link_path: []const u8) void {
  if (comptime builtin.os.tag != .windows) return;
  var target_utf16: [std.Io.Dir.max_path_bytes]u16 = undefined;
  var link_utf16: [std.Io.Dir.max_path_bytes]u16 = undefined;
  const target_len = std.unicode.utf8ToUtf16Le(&target_utf16, target) catch return;
  const link_len = std.unicode.utf8ToUtf16Le(&link_utf16, link_path) catch return;
  target_utf16[target_len] = 0;
  
  _ = std.os.windows.CreateSymbolicLink(
    null, link_utf16[0..link_len],
    target_utf16[0..target_len :0], false,
  ) catch {};
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
};

pub const Linker = struct {
  allocator: std.mem.Allocator,
  stats: LinkStats,
  node_modules_dir: ?std.Io.Dir,
  bin_dir: ?std.Io.Dir,
  node_modules_path: []const u8,
  cross_device: std.atomic.Value(bool),

  pub fn init(allocator: std.mem.Allocator) Linker {
    return .{
      .allocator = allocator,
      .stats = LinkStats.init(),
      .node_modules_dir = null,
      .bin_dir = null,
      .node_modules_path = "",
      .cross_device = std.atomic.Value(bool).init(false),
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

  pub fn linkPackage(self: *Linker, pkg: PackageLink) !void {
    const node_modules = self.node_modules_dir orelse return error.IoError;

    const install_path = if (pkg.parent_path) |parent|
      try std.fmt.allocPrint(self.allocator, "{s}/node_modules/{s}", .{ parent, pkg.name })
    else
      try self.allocator.dupe(u8, pkg.name);
    defer self.allocator.free(install_path);

    var source_dir = std.Io.Dir.cwd().openDir(io, pkg.cache_path, .{ .iterate = true }) catch {
      return error.PathNotFound;
    };
    defer source_dir.close(io);

    const source_version = readPackageVersion(self.allocator, source_dir);
    defer if (source_version) |v| self.allocator.free(v);

    var should_skip = false;
    var has_existing_install = false;
    var has_existing_package = false;
    
    {
      const existing = node_modules.openDir(io, install_path, .{ .iterate = true }) catch null;
      if (existing) |dir| {
        has_existing_install = true;
        var installed_dir = dir;
        defer installed_dir.close(io);

        const installed_version = readPackageVersion(self.allocator, installed_dir);
        defer if (installed_version) |v| self.allocator.free(v);
        has_existing_package = installed_version != null;
        should_skip = packageVersionsMatch(source_version, installed_version) and
          installedFileCountMatches(installed_dir, pkg.file_count);
      }
    }

    if (should_skip) {
      _ = self.stats.packages_skipped.fetchAdd(1, .release);
      return;
    }

    if (has_existing_install and has_existing_package) {
      node_modules.deleteTree(io, install_path) catch return error.IoError;
    }

    node_modules.createDirPath(io, install_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };
    _ = self.stats.dirs_created.fetchAdd(1, .release);

    var dest_dir = node_modules.openDir(io, install_path, .{}) catch return error.IoError;
    defer dest_dir.close(io);

    self.linkDirectoryWithHint(source_dir, dest_dir, pkg.file_count, pkg.name) catch |err| return err;

    if (pkg.parent_path == null and pkg.has_bin) try self.linkBinaries(pkg.name);
    _ = self.stats.packages_installed.fetchAdd(1, .release);
  }

  fn linkBinaries(self: *Linker, pkg_name: []const u8) !void {
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
        self.createBinSymlink(pkg_name, entry.key, bin_path, bin_dir) catch continue;
      }
    } else if (root_val.getString("bin")) |bin_path| {
      const simple_name = if (std.mem.indexOf(u8, pkg_name, "/")) |slash|
        pkg_name[slash + 1 ..]
      else
        pkg_name;
      self.createBinSymlink(pkg_name, simple_name, bin_path, bin_dir) catch {};
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

  const FileWorkItem = struct {
    source_path: []const u8,
    dest_path: []const u8,
    kind: std.Io.File.Kind,
    link_target: ?[]const u8,
  };

  fn linkDirectory(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir) !void {
    try self.linkDirectorySequential(source, dest);
  }

  pub fn linkDirectoryWithHint(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir, file_count: u32, name: []const u8) !void {
    if (file_count >= PARALLEL_LINK_THRESHOLD) {
      debug.log("    parallel link: {s} ({d} files)", .{ name, file_count });
      try self.linkDirectoryParallel(source, dest);
    } else try self.linkDirectorySequential(source, dest);
  }

  fn linkDirectorySequential(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir) !void {
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

          try self.linkDirectorySequential(child_source, child_dest);
        },
        .file => try self.linkFile(source, dest, entry.name),
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
  };

  fn linkDirectoryParallel(self: *Linker, source: std.Io.Dir, dest: std.Io.Dir) !void {
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

          ctx.linker.linkFile(src_dir, dst_dir, filename) catch {};
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

  fn linkFile(self: *Linker, source_dir: std.Io.Dir, dest_dir: std.Io.Dir, name: []const u8) !void {
    dest_dir.deleteFile(io, name) catch {};

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
      const errno = std.posix.errno(result);
      return switch (errno) {
        .XDEV => error.CrossDevice,
        .PERM, .ACCES => error.PermissionDenied,
        else => error.IoError,
      };
    }
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
    _ = self;

    var source = source_dir.openFile(io, name, .{}) catch return error.IoError;
    defer source.close(io);

    const stat = source.stat(io) catch return error.IoError;
    var dest = dest_dir.createFile(io, name, 
      if (comptime builtin.os.tag != .windows) .{ .permissions = stat.permissions } else .{}
    ) catch return error.IoError; defer dest.close(io);

    var buf: [64 * 1024]u8 = undefined;
    while (true) {
      const bytes_read = source.readStreaming(io, &.{&buf}) catch return error.IoError;
      if (bytes_read == 0) break;
      dest.writeStreamingAll(io, buf[0..bytes_read]) catch return error.IoError;
    }
  }

  pub fn getStats(self: *const Linker) StatsSnapshot {
    return self.stats.snapshot();
  }
};
