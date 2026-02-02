const std = @import("std");
const builtin = @import("builtin");
const json = @import("json.zig");
const debug = @import("debug.zig");

const PARALLEL_LINK_THRESHOLD = 500;
const LINK_THREAD_COUNT = 8;

pub fn createSymlinkOrCopy(dir: std.fs.Dir, target: []const u8, link_name: []const u8) void {
  if (comptime builtin.os.tag == .windows) {
    createSymlinkWindows(dir, target, link_name);
  } else dir.symLink(target, link_name, .{}) catch {};
}

pub fn createSymlinkAbsolute(target: []const u8, link_path: []const u8) void {
  if (comptime builtin.os.tag == .windows) {
    createSymlinkAbsoluteWindows(target, link_path);
  } else std.posix.symlink(target, link_path) catch {};
}

fn createSymlinkWindows(dir: std.fs.Dir, target: []const u8, link_name: []const u8) void {
  if (comptime builtin.os.tag != .windows) return;
  var target_utf16: [std.fs.max_path_bytes]u16 = undefined;
  var link_utf16: [std.fs.max_path_bytes]u16 = undefined;
  const target_len = std.unicode.utf8ToUtf16Le(&target_utf16, target) catch return;
  const link_len = std.unicode.utf8ToUtf16Le(&link_utf16, link_name) catch return;
  target_utf16[target_len] = 0;
  
  _ = std.os.windows.CreateSymbolicLink(
    dir.fd,
    link_utf16[0..link_len],
    target_utf16[0..target_len :0],
    .{ .is_directory = false }
  ) catch {};
}

fn createSymlinkAbsoluteWindows(target: []const u8, link_path: []const u8) void {
  if (comptime builtin.os.tag != .windows) return;
  var target_utf16: [std.fs.max_path_bytes]u16 = undefined;
  var link_utf16: [std.fs.max_path_bytes]u16 = undefined;
  const target_len = std.unicode.utf8ToUtf16Le(&target_utf16, target) catch return;
  const link_len = std.unicode.utf8ToUtf16Le(&link_utf16, link_path) catch return;
  target_utf16[target_len] = 0;
  
  _ = std.os.windows.CreateSymbolicLink(
    null,
    link_utf16[0..link_len],
    target_utf16[0..target_len :0],
    .{ .is_directory = false }
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
};

pub const Linker = struct {
  allocator: std.mem.Allocator,
  stats: LinkStats,
  node_modules_dir: ?std.fs.Dir,
  bin_dir: ?std.fs.Dir,
  node_modules_path: []const u8,
  cross_device: bool,

  pub fn init(allocator: std.mem.Allocator) Linker {
    return .{
      .allocator = allocator,
      .stats = LinkStats.init(),
      .node_modules_dir = null,
      .bin_dir = null,
      .node_modules_path = "",
      .cross_device = false,
    };
  }

  pub fn deinit(self: *Linker) void {
    if (self.node_modules_dir) |*d| d.close();
    if (self.bin_dir) |*d| d.close();
    if (self.node_modules_path.len > 0) self.allocator.free(self.node_modules_path);
  }

  pub fn setNodeModulesPath(self: *Linker, path: []const u8) !void {
    std.fs.cwd().makePath(path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };

    self.node_modules_dir = try std.fs.cwd().openDir(path, .{});
    self.node_modules_path = try self.allocator.dupe(u8, path);

    const bin_path = try std.fmt.allocPrint(self.allocator, "{s}/.bin", .{path});
    defer self.allocator.free(bin_path);
    std.fs.cwd().makePath(bin_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };
    self.bin_dir = try std.fs.cwd().openDir(bin_path, .{});
  }

  pub fn linkPackage(self: *Linker, pkg: PackageLink) !void {
    const node_modules = self.node_modules_dir orelse return error.IoError;

    const install_path = if (pkg.parent_path) |parent|
      try std.fmt.allocPrint(self.allocator, "{s}/node_modules/{s}", .{ parent, pkg.name })
    else
      try self.allocator.dupe(u8, pkg.name);
    defer self.allocator.free(install_path);

    const d = node_modules.openDir(install_path, .{}) catch null;
    if (d) |dir| {
      var dd = dir;
      defer dd.close();
      if (dd.statFile("package.json")) |_| {
        _ = self.stats.packages_skipped.fetchAdd(1, .release);
        return;
      } else |_| {}
    }

    var source_dir = std.fs.cwd().openDir(pkg.cache_path, .{ .iterate = true }) catch {
      return error.PathNotFound;
    };
    defer source_dir.close();

    node_modules.makePath(install_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };
    _ = self.stats.dirs_created.fetchAdd(1, .release);

    var dest_dir = node_modules.openDir(install_path, .{}) catch return error.IoError;
    defer dest_dir.close();

    self.linkDirectoryWithHint(source_dir, dest_dir, pkg.file_count, pkg.name) catch |err| return err;

    if (pkg.parent_path == null) try self.linkBinaries(pkg.name);
    _ = self.stats.packages_installed.fetchAdd(1, .release);
  }

  fn linkBinaries(self: *Linker, pkg_name: []const u8) !void {
    const bin_dir = self.bin_dir orelse return;
    const node_modules = self.node_modules_dir orelse return;

    const pkg_dir = node_modules.openDir(pkg_name, .{}) catch return;
    defer @constCast(&pkg_dir).close();

    const pkg_json_file = pkg_dir.openFile("package.json", .{}) catch return;
    defer pkg_json_file.close();

    const content = pkg_json_file.readToEndAlloc(self.allocator, 1024 * 1024) catch return;
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

  fn createBinSymlink(self: *Linker, pkg_name: []const u8, cmd_name: []const u8, bin_path: []const u8, bin_dir: std.fs.Dir) !void {
    var normalized_path = bin_path;
    if (std.mem.startsWith(u8, normalized_path, "./")) {
      normalized_path = normalized_path[2..];
    }

    const target = try std.fmt.allocPrint(self.allocator, "../{s}/{s}", .{ pkg_name, normalized_path });
    defer self.allocator.free(target);

    bin_dir.deleteFile(cmd_name) catch {};
    createSymlinkOrCopy(bin_dir, target, cmd_name);

    _ = self.stats.bins_linked.fetchAdd(1, .release);
  }

  const FileWorkItem = struct {
    source_path: []const u8,
    dest_path: []const u8,
    kind: std.fs.Dir.Entry.Kind,
    link_target: ?[]const u8,
  };

  fn linkDirectory(self: *Linker, source: std.fs.Dir, dest: std.fs.Dir) !void {
    try self.linkDirectorySequential(source, dest);
  }

  pub fn linkDirectoryWithHint(self: *Linker, source: std.fs.Dir, dest: std.fs.Dir, file_count: u32, name: []const u8) !void {
    if (file_count >= PARALLEL_LINK_THRESHOLD) {
      debug.log("    parallel link: {s} ({d} files)", .{ name, file_count });
      try self.linkDirectoryParallel(source, dest);
    } else try self.linkDirectorySequential(source, dest);
  }

  fn linkDirectorySequential(self: *Linker, source: std.fs.Dir, dest: std.fs.Dir) !void {
    var iter = source.iterate();
    while (try iter.next()) |entry| {
      switch (entry.kind) {
        .directory => {
          dest.makePath(entry.name) catch |err| switch (err) {
            error.PathAlreadyExists => {},
            else => return error.IoError,
          };
          _ = self.stats.dirs_created.fetchAdd(1, .release);

          var child_source = source.openDir(entry.name, .{ .iterate = true }) catch continue;
          defer child_source.close();

          var child_dest = dest.openDir(entry.name, .{}) catch continue;
          defer child_dest.close();

          try self.linkDirectorySequential(child_source, child_dest);
        },
        .file => try self.linkFile(source, dest, entry.name),
        .sym_link => {
          var link_buf: [std.fs.max_path_bytes]u8 = undefined;
          const target = source.readLink(entry.name, &link_buf) catch continue;
          createSymlinkOrCopy(dest, target, entry.name);
        },
        else => {},
      }
    }
  }

  const ParallelThreadContext = struct {
    linker: *Linker,
    items: []const FileWorkItem,
    source_base: std.fs.Dir,
    dest_base: std.fs.Dir,
  };

  fn linkDirectoryParallel(self: *Linker, source: std.fs.Dir, dest: std.fs.Dir) !void {
    var work_items = std.ArrayListUnmanaged(FileWorkItem){};
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
    var thread_count: usize = 0;

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

      threads[i] = std.Thread.spawn(.{}, processWorkItems, .{&contexts[i]}) catch null;
      if (threads[i] != null) thread_count += 1;
      offset = end;
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
            ctx.source_base.openDir(src_dir_path, .{}) catch continue
          else
            ctx.source_base;
          defer if (src_dir_path.len > 0) src_dir.close();

          var dst_dir = if (dst_dir_path.len > 0)
            ctx.dest_base.openDir(dst_dir_path, .{}) catch continue
          else
            ctx.dest_base;
          defer if (dst_dir_path.len > 0) dst_dir.close();

          ctx.linker.linkFile(src_dir, dst_dir, filename) catch {};
        },
        .sym_link => {
          if (item.link_target) |target| {
            const dst_dir_path = std.fs.path.dirname(item.dest_path) orelse "";
            const filename = std.fs.path.basename(item.dest_path);

            var dst_dir = if (dst_dir_path.len > 0)
              ctx.dest_base.openDir(dst_dir_path, .{}) catch continue
            else
              ctx.dest_base;
            defer if (dst_dir_path.len > 0) dst_dir.close();
            createSymlinkOrCopy(dst_dir, target, filename);
          }
        },
        else => {},
      }
    }
  }

  fn collectWorkItems(self: *Linker, source: std.fs.Dir, dest: std.fs.Dir, prefix: []const u8, work_items: *std.ArrayListUnmanaged(FileWorkItem)) !void {
    var iter = source.iterate();
    while (try iter.next()) |entry| {
      const rel_path = if (prefix.len > 0)
        try std.fmt.allocPrint(self.allocator, "{s}/{s}", .{ prefix, entry.name })
      else
        try self.allocator.dupe(u8, entry.name);
      errdefer self.allocator.free(rel_path);

      switch (entry.kind) {
        .directory => {
          dest.makePath(rel_path) catch |err| switch (err) {
            error.PathAlreadyExists => {},
            else => {
              self.allocator.free(rel_path);
              return error.IoError;
            },
          };
          _ = self.stats.dirs_created.fetchAdd(1, .release);

          var child_source = source.openDir(entry.name, .{ .iterate = true }) catch {
            self.allocator.free(rel_path);
            continue;
          };
          defer child_source.close();

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
          var link_buf: [std.fs.max_path_bytes]u8 = undefined;
          const target = source.readLink(entry.name, &link_buf) catch {
            self.allocator.free(rel_path);
            continue;
          };
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

  fn linkFile(self: *Linker, source_dir: std.fs.Dir, dest_dir: std.fs.Dir, name: []const u8) !void {
    dest_dir.deleteFile(name) catch {};

    if (!self.cross_device) {
      if (linkAt(source_dir, name, dest_dir, name)) {
        _ = self.stats.files_linked.fetchAdd(1, .release);
        return;
      } else |err| {
        if (err == error.CrossDevice) {
          self.cross_device = true;
        } else if (err != error.IoError) return err;
      }
    }

    if (comptime builtin.os.tag == .macos) {
      if (fclonefileat(source_dir.fd, name, dest_dir.fd, name)) {
        _ = self.stats.files_cloned.fetchAdd(1, .release);
        return;
      } else |_| {}
    }

    try self.copyFile(source_dir, dest_dir, name);
    _ = self.stats.files_copied.fetchAdd(1, .release);
  }

  fn linkAt(source_dir: std.fs.Dir, source_name: []const u8, dest_dir: std.fs.Dir, dest_name: []const u8) !void {
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

    const result = std.c.linkat(source_dir.fd, source_z, dest_dir.fd, dest_z, 0);
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

  fn copyFile(self: *Linker, source_dir: std.fs.Dir, dest_dir: std.fs.Dir, name: []const u8) !void {
    _ = self;

    var source = source_dir.openFile(name, .{}) catch return error.IoError;
    defer source.close();

    var dest = dest_dir.createFile(name, .{}) catch return error.IoError;
    defer dest.close();

    var buf: [64 * 1024]u8 = undefined;
    while (true) {
      const bytes_read = source.read(&buf) catch return error.IoError;
      if (bytes_read == 0) break;
      dest.writeAll(buf[0..bytes_read]) catch return error.IoError;
    }
  }

  pub fn getStats(self: *const Linker) StatsSnapshot {
    return self.stats.snapshot();
  }
};