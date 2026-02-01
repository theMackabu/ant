const std = @import("std");
const builtin = @import("builtin");

pub const lockfile = @import("lockfile.zig");
pub const cache = @import("cache.zig");
pub const fetcher = @import("fetcher.zig");
pub const extractor = @import("extractor.zig");
pub const linker = @import("linker.zig");
pub const resolver = @import("resolver.zig");
pub const intern = @import("intern.zig");
pub const json = @import("json.zig");
pub const debug = @import("debug.zig");

const global_allocator: std.mem.Allocator = std.heap.c_allocator;

pub const PkgError = enum(c_int) {
  ok = 0,
  out_of_memory = -1,
  invalid_lockfile = -2,
  io_error = -3,
  network_error = -4,
  cache_error = -5,
  extract_error = -6,
  resolve_error = -7,
  invalid_argument = -8,
  not_found = -9,
  integrity_mismatch = -10,
};

pub const ProgressCallback = ?*const fn (
  user_data: ?*anyopaque,
  phase: Phase,
  current: u32,
  total: u32,
  message: [*:0]const u8,
) callconv(.c) void;

pub const Phase = enum(c_int) {
  resolving = 0,
  fetching = 1,
  extracting = 2,
  linking = 3,
  caching = 4,
  postinstall = 5,
};

pub const PkgOptions = extern struct {
  cache_dir: ?[*:0]const u8 = null,
  registry_url: ?[*:0]const u8 = null,
  max_connections: u32 = 6,
  progress_callback: ProgressCallback = null,
  user_data: ?*anyopaque = null,
  verbose: bool = false,
};

pub const CacheStats = extern struct {
  total_size: u64,
  package_count: u32,
};

pub const AddedPackage = extern struct {
  name: [*:0]const u8,
  version: [*:0]const u8,
  direct: bool,
};

pub const InstallResult = extern struct {
  package_count: u32,
  cache_hits: u32,
  cache_misses: u32,
  files_linked: u32,
  files_copied: u32,
  packages_installed: u32,
  packages_skipped: u32,
  elapsed_ms: u64,
};

pub const LifecycleScript = extern struct {
  name: [*:0]const u8,
  script: [*:0]const u8,
};

pub const PkgContext = struct {
  allocator: std.mem.Allocator,
  arena_state: std.heap.ArenaAllocator,
  string_pool: intern.StringPool,
  cache_db: ?*cache.CacheDB,
  http: ?*fetcher.Fetcher,
  options: PkgOptions,
  last_error: ?[:0]u8,
  cache_dir: []const u8,
  metadata_cache: std.StringHashMap(resolver.PackageMetadata),
  last_install_result: InstallResult,
  added_packages: std.ArrayListUnmanaged(AddedPackage),
  added_packages_storage: std.ArrayListUnmanaged([:0]u8),
  lifecycle_scripts: std.ArrayListUnmanaged(LifecycleScript),
  lifecycle_scripts_storage: std.ArrayListUnmanaged([:0]u8),

  pub fn init(allocator: std.mem.Allocator, options: PkgOptions) !*PkgContext {
    const ctx = try allocator.create(PkgContext);
    errdefer allocator.destroy(ctx);

    const cache_path = if (options.cache_dir) |dir| std.mem.span(dir)
    else try getDefaultCacheDir(allocator);

    ctx.* = .{
      .allocator = allocator,
      .arena_state = std.heap.ArenaAllocator.init(allocator),
      .string_pool = intern.StringPool.init(allocator),
      .cache_db = null,
      .http = null,
      .options = options,
      .last_error = null,
      .cache_dir = try allocator.dupe(u8, cache_path),
      .metadata_cache = std.StringHashMap(resolver.PackageMetadata).init(allocator),
      .last_install_result = .{ 
        .package_count = 0,
        .cache_hits = 0,
        .cache_misses = 0,
        .files_linked = 0,
        .files_copied = 0,
        .packages_installed = 0,
        .packages_skipped = 0,
        .elapsed_ms = 0
      },
      .added_packages = .{},
      .added_packages_storage = .{},
      .lifecycle_scripts = .{},
      .lifecycle_scripts_storage = .{},
    };

    debug.enabled = options.verbose;
    debug.log("init: cache_dir={s}", .{ctx.cache_dir});
    ctx.cache_db = cache.CacheDB.open(ctx.cache_dir) catch |err| {
      ctx.setErrorFmt("Failed to open cache database: {}", .{err});
      return error.CacheError;
    };
    
    debug.log("init: cache database opened", .{});
    const registry = if (options.registry_url) |url|
      std.mem.span(url)
    else
      "registry.npmjs.org";

    ctx.http = fetcher.Fetcher.init(allocator, registry) catch |err| {
      ctx.setErrorFmt("Failed to initialize fetcher: {}", .{err});
      return error.NetworkError;
    };
    debug.log("init: http fetcher ready, registry={s}", .{registry});

    return ctx;
  }

  pub fn deinit(self: *PkgContext) void {
    if (self.http) |h| h.deinit();
    if (self.cache_db) |db| db.close();
    var meta_iter = self.metadata_cache.valueIterator();
    while (meta_iter.next()) |meta| meta.deinit();
    self.metadata_cache.deinit();
    self.string_pool.deinit();
    self.arena_state.deinit();
    if (self.last_error) |e| self.allocator.free(e);
    for (self.added_packages_storage.items) |s| self.allocator.free(s);
    self.added_packages_storage.deinit(self.allocator);
    self.added_packages.deinit(self.allocator);
    for (self.lifecycle_scripts_storage.items) |s| self.allocator.free(s);
    self.lifecycle_scripts_storage.deinit(self.allocator);
    self.lifecycle_scripts.deinit(self.allocator);
    self.allocator.free(self.cache_dir);
    self.allocator.destroy(self);
  }

  pub fn setErrorFmt(self: *PkgContext, comptime fmt: []const u8, args: anytype) void {
    if (self.last_error) |e| self.allocator.free(e);
    self.last_error = std.fmt.allocPrintSentinel(self.allocator, fmt, args, 0) catch null;
  }

  pub fn setError(self: *PkgContext, msg: []const u8) void {
    if (self.last_error) |e| self.allocator.free(e);
    self.last_error = self.allocator.dupeZ(u8, msg) catch null;
  }

  fn getDefaultCacheDir(allocator: std.mem.Allocator) ![]const u8 {
    if (std.posix.getenv("HOME")) |home| {
      return std.fmt.allocPrint(allocator, "{s}/.ant/pkg", .{home});
    }
    return error.NoHomeDir;
  }

  fn reportProgress(self: *PkgContext, phase: Phase, current: u32, total: u32, message: [:0]const u8) void {
    if (self.options.progress_callback) |cb| {
      cb(self.options.user_data, phase, current, total, message.ptr);
    }
  }

  fn clearAddedPackages(self: *PkgContext) void {
    for (self.added_packages_storage.items) |s| self.allocator.free(s);
    self.added_packages_storage.clearRetainingCapacity();
    self.added_packages.clearRetainingCapacity();
  }

  fn clearLifecycleScripts(self: *PkgContext) void {
    for (self.lifecycle_scripts_storage.items) |s| self.allocator.free(s);
    self.lifecycle_scripts_storage.clearRetainingCapacity();
    self.lifecycle_scripts.clearRetainingCapacity();
  }

  fn addLifecycleScript(self: *PkgContext, name: []const u8, script: []const u8) !void {
    const name_z = try self.allocator.dupeZ(u8, name);
    errdefer self.allocator.free(name_z);
    const script_z = try self.allocator.dupeZ(u8, script);
    errdefer self.allocator.free(script_z);

    try self.lifecycle_scripts_storage.append(self.allocator, name_z);
    try self.lifecycle_scripts_storage.append(self.allocator, script_z);
    try self.lifecycle_scripts.append(self.allocator, .{
      .name = name_z.ptr,
      .script = script_z.ptr,
    });
  }

  fn addPackageToResults(self: *PkgContext, name: []const u8, version: []const u8, direct: bool) !void {
    for (self.added_packages.items) |pkg| {
      if (std.mem.eql(u8, std.mem.span(pkg.name), name)) return;
    }

    const name_z = try self.allocator.dupeZ(u8, name);
    errdefer self.allocator.free(name_z);
    const version_z = try self.allocator.dupeZ(u8, version);
    errdefer self.allocator.free(version_z);

    try self.added_packages_storage.append(self.allocator, name_z);
    try self.added_packages_storage.append(self.allocator, version_z);
    try self.added_packages.append(self.allocator, .{
      .name = name_z.ptr,
      .version = version_z.ptr,
      .direct = direct,
    });
  }

  pub fn install(self: *PkgContext, lockfile_path: []const u8, node_modules_path: []const u8) !void {
    _ = self.arena_state.reset(.retain_capacity);
    const arena_alloc = self.arena_state.allocator();

    self.clearAddedPackages();

    var timer = std.time.Timer.start() catch return error.OutOfMemory;
    var stage_start: u64 = @intCast(std.time.nanoTimestamp());

    debug.log("install start: lockfile={s} node_modules={s}", .{ lockfile_path, node_modules_path });

    var lf = lockfile.Lockfile.open(lockfile_path) catch {
      self.setError("Failed to open lockfile");
      return error.InvalidLockfile;
    };
    defer lf.close();

    const pkg_count = lf.header.package_count;
    stage_start = debug.timer("lockfile open", stage_start);
    debug.log("  packages in lockfile: {d}", .{pkg_count});

    var integrities = try arena_alloc.alloc([64]u8, pkg_count);
    for (lf.packages, 0..) |pkg, i| {
      integrities[i] = pkg.integrity;
    }

    const db = self.cache_db orelse return error.CacheError;
    var cache_hits = try db.batchLookup(integrities, arena_alloc);
    defer cache_hits.deinit();
    stage_start = debug.timer("cache lookup", stage_start);

    var hit_set = std.AutoHashMap(u32, u32).init(arena_alloc);
    for (cache_hits.items) |hit| {
      try hit_set.put(hit.index, hit.file_count);
    }

    var misses = std.ArrayListUnmanaged(u32){};
    for (0..pkg_count) |i| {
      if (!hit_set.contains(@intCast(i))) {
        try misses.append(arena_alloc, @intCast(i));
      }
    }
    debug.log("  cache hits: {d}, misses: {d}", .{ cache_hits.items.len, misses.items.len });

    var pkg_linker = linker.Linker.init(self.allocator);
    defer pkg_linker.deinit();
    try pkg_linker.setNodeModulesPath(node_modules_path);

    for (cache_hits.items, 0..) |hit, i| {
      const pkg = &lf.packages[hit.index];
      const pkg_name = pkg.name.slice(lf.string_table);
      const cache_path = try db.getPackagePath(&pkg.integrity, arena_alloc);
      const parent_path = pkg.parent_path.slice(lf.string_table);

      const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{pkg_name}, 0) catch continue;
      self.reportProgress(.linking, @intCast(i), @intCast(cache_hits.items.len), msg);

      try pkg_linker.linkPackage(.{
        .cache_path = cache_path,
        .node_modules_path = node_modules_path,
        .name = pkg_name,
        .parent_path = if (parent_path.len > 0) parent_path else null,
        .file_count = hit.file_count,
      });

      if (pkg.flags.direct) {
        const version_str = std.fmt.allocPrint(arena_alloc, "{d}.{d}.{d}", .{
          pkg.version_major,
          pkg.version_minor,
          pkg.version_patch,
        }) catch continue;
        self.addPackageToResults(pkg_name, version_str, true) catch {};
      }
    }
    stage_start = debug.timer("link cache hits", stage_start);

    if (misses.items.len > 0) {
      const http = self.http orelse return error.NetworkError;
      const PkgExtractCtx = struct {
        ext: *extractor.Extractor,
        pkg_idx: u32,
        integrity: [64]u8,
        cache_path: []const u8,
        pkg_name: []const u8,
        version_str: []const u8,
        direct: bool,
        parent_path: ?[]const u8,
        completed: bool,
        has_error: bool,
      };

      var extract_contexts = try arena_alloc.alloc(PkgExtractCtx, misses.items.len);
      var valid_count: usize = 0;

      debug.log("queuing {d} tarball fetches...", .{misses.items.len});

      for (misses.items, 0..) |pkg_idx, i| {
        const pkg = &lf.packages[pkg_idx];
        const pkg_name = pkg.name.slice(lf.string_table);
        const tarball_url = pkg.tarball_url.slice(lf.string_table);
        const version_str = pkg.versionString(arena_alloc, lf.string_table) catch continue;
        const cache_path = db.getPackagePath(&pkg.integrity, arena_alloc) catch continue;

        const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{pkg_name}, 0) catch continue;
        self.reportProgress(.fetching, @intCast(i), @intCast(misses.items.len), msg);

        if (self.options.verbose) {
          debug.log("  queue: {s}@{s}", .{ pkg_name, version_str });
        }

        const ext = extractor.Extractor.init(self.allocator, cache_path) catch continue;
        const parent_path_str = pkg.parent_path.slice(lf.string_table);
        
        extract_contexts[valid_count] = .{
          .ext = ext,
          .pkg_idx = pkg_idx,
          .integrity = pkg.integrity,
          .cache_path = cache_path,
          .pkg_name = pkg_name,
          .version_str = version_str,
          .direct = pkg.flags.direct,
          .parent_path = if (parent_path_str.len > 0) parent_path_str else null,
          .completed = false,
          .has_error = false,
        };

        http.fetchTarball(tarball_url, fetcher.StreamHandler.init(
          struct {
            fn onData(data: []const u8, user_data: ?*anyopaque) void {
              const ctx: *PkgExtractCtx = @ptrCast(@alignCast(user_data));
              ctx.ext.feedCompressed(data) catch {
                ctx.has_error = true;
              };
            }
          }.onData,
          struct {
            fn onComplete(_: u16, user_data: ?*anyopaque) void {
              const ctx: *PkgExtractCtx = @ptrCast(@alignCast(user_data));
              ctx.completed = true;
            }
          }.onComplete,
          struct {
            fn onError(_: fetcher.FetchError, user_data: ?*anyopaque) void {
              const ctx: *PkgExtractCtx = @ptrCast(@alignCast(user_data));
              ctx.has_error = true;
              ctx.completed = true;
            }
          }.onError,
          &extract_contexts[valid_count],
        )) catch continue;

        valid_count += 1;
      }
      
      stage_start = debug.timer("queue fetches", stage_start);
      debug.log("running event loop for {d} fetches...", .{valid_count});
      
      http.run() catch {};
      stage_start = debug.timer("fetch + extract", stage_start);

      var success_count: usize = 0;
      var error_count: usize = 0;
      for (extract_contexts[0..valid_count], 0..) |*ctx, i| {
        defer ctx.ext.deinit();

        if (ctx.has_error) {
          error_count += 1;
          debug.log("  error: {s}", .{ctx.pkg_name});
          continue;
        }
        success_count += 1;

        const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{ctx.pkg_name}, 0) catch continue;
        self.reportProgress(.linking, @intCast(i), @intCast(valid_count), msg);

        const stats = ctx.ext.stats();
        if (stats.files >= 100) {
          debug.log("  extracted {s}: {d} files, {d} bytes", .{ ctx.pkg_name, stats.files, stats.bytes });
        }
        
        db.insert(&.{
          .integrity = ctx.integrity,
          .path = ctx.cache_path,
          .unpacked_size = stats.bytes,
          .file_count = stats.files,
          .cached_at = std.time.timestamp(),
        }, ctx.pkg_name, ctx.version_str) catch continue;

        self.addPackageToResults(ctx.pkg_name, ctx.version_str, ctx.direct) catch {};

        pkg_linker.linkPackage(.{
          .cache_path = ctx.cache_path,
          .node_modules_path = node_modules_path,
          .name = ctx.pkg_name,
          .parent_path = ctx.parent_path,
          .file_count = stats.files,
        }) catch {};
      }
      stage_start = debug.timer("cache insert + link misses", stage_start);
      debug.log("  fetched: {d} success, {d} errors", .{ success_count, error_count });
    }

    db.sync();
    stage_start = debug.timer("cache sync", stage_start);

    const link_stats = pkg_linker.getStats();
    self.last_install_result = .{
      .package_count = pkg_count,
      .cache_hits = @intCast(cache_hits.items.len),
      .cache_misses = @intCast(misses.items.len),
      .files_linked = link_stats.files_linked,
      .files_copied = link_stats.files_copied,
      .packages_installed = link_stats.packages_installed,
      .packages_skipped = link_stats.packages_skipped,
      .elapsed_ms = timer.read() / 1_000_000,
    };
  }
};

export fn pkg_init(options: *const PkgOptions) ?*PkgContext {
  return PkgContext.init(global_allocator, options.*) catch null;
}

export fn pkg_free(ctx: ?*PkgContext) void {
  if (ctx) |c| c.deinit();
}

export fn pkg_install(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  lockfile_path: [*:0]const u8,
  node_modules_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;

  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  c.install(
    std.mem.span(lockfile_path),
    std.mem.span(node_modules_path),
  ) catch |err| {
    return switch (err) {
      error.InvalidLockfile => .invalid_lockfile,
      error.CacheError => .cache_error,
      error.NetworkError => .network_error,
      error.OutOfMemory => .out_of_memory,
      else => .io_error,
    };
  };

  var pkg_json = json.PackageJson.parse(arena_alloc, std.mem.span(package_json_path)) catch return .ok;
  defer pkg_json.deinit(arena_alloc);
  if (pkg_json.trusted_dependencies.count() > 0) {
    runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc);
  }

  return .ok;
}

export fn pkg_get_install_result(ctx: ?*PkgContext, out: *InstallResult) PkgError {
  const c = ctx orelse return .invalid_argument;
  out.* = c.last_install_result;
  return .ok;
}

export fn pkg_get_added_count(ctx: ?*const PkgContext) u32 {
  const c = ctx orelse return 0;
  return @intCast(c.added_packages.items.len);
}

export fn pkg_get_added_package(ctx: ?*const PkgContext, index: u32, out: *AddedPackage) PkgError {
  const c = ctx orelse return .invalid_argument;
  if (index >= c.added_packages.items.len) return .invalid_argument;
  out.* = c.added_packages.items[index];
  return .ok;
}

export fn pkg_discover_lifecycle_scripts(
  ctx: ?*PkgContext,
  node_modules_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  c.clearLifecycleScripts();

  const nm_path = std.mem.span(node_modules_path);
  var nm_dir = std.fs.cwd().openDir(nm_path, .{ .iterate = true }) catch return .io_error;
  defer nm_dir.close();

  var iter = nm_dir.iterate();
  while (iter.next() catch null) |entry| {
    if (entry.kind != .directory) continue;
    if (entry.name[0] == '@') {
      var scope_dir = nm_dir.openDir(entry.name, .{ .iterate = true }) catch continue;
      defer scope_dir.close();
      var scope_iter = scope_dir.iterate();
      while (scope_iter.next() catch null) |scoped_entry| {
        if (scoped_entry.kind != .directory) continue;
        const full_name = std.fmt.allocPrint(c.allocator, "@{s}/{s}", .{ entry.name[1..], scoped_entry.name }) catch continue;
        defer c.allocator.free(full_name);
        discoverPackageScript(c, nm_path, full_name, scope_dir, scoped_entry.name);
      }
    } else {
      discoverPackageScript(c, nm_path, entry.name, nm_dir, entry.name);
    }
  }

  return .ok;
}

fn discoverPackageScript(ctx: *PkgContext, nm_path: []const u8, pkg_name: []const u8, parent_dir: std.fs.Dir, dir_name: []const u8) void {
  var pkg_dir = parent_dir.openDir(dir_name, .{}) catch return;
  defer pkg_dir.close();

  pkg_dir.access(".postinstall", .{}) catch |err| {
    if (err != error.FileNotFound) return;
    const pkg_json = pkg_dir.openFile("package.json", .{}) catch return;
    defer pkg_json.close();

    const content = pkg_json.readToEndAlloc(ctx.allocator, 1024 * 1024) catch return;
    defer ctx.allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch return;
    defer doc.deinit();

    const root = doc.root();
    if (root.getObject("scripts")) |scripts| {
      const script = scripts.getString("postinstall") orelse
        scripts.getString("install") orelse return;

      if (std.mem.eql(u8, pkg_name, "esbuild")) return;
      ctx.addLifecycleScript(pkg_name, script) catch return;
    }
    return;
  };
  _ = nm_path;
}

export fn pkg_get_lifecycle_script_count(ctx: ?*const PkgContext) u32 {
  const c = ctx orelse return 0;
  return @intCast(c.lifecycle_scripts.items.len);
}

export fn pkg_get_lifecycle_script(ctx: ?*const PkgContext, index: u32, out: *LifecycleScript) PkgError {
  const c = ctx orelse return .invalid_argument;
  if (index >= c.lifecycle_scripts.items.len) return .invalid_argument;
  out.* = c.lifecycle_scripts.items[index];
  return .ok;
}

export fn pkg_run_postinstall(
  ctx: ?*PkgContext,
  node_modules_path: [*:0]const u8,
  package_names: [*]const [*:0]const u8,
  count: u32,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  var trusted = std.StringHashMap(void).init(arena_alloc);
  for (0..count) |i| {
    trusted.put(std.mem.span(package_names[i]), {}) catch continue;
  }

  runTrustedPostinstall(c, &trusted, std.mem.span(node_modules_path), arena_alloc);
  return .ok;
}

export fn pkg_add_trusted_dependencies(
  package_json_path: [*:0]const u8,
  package_names: [*]const [*:0]const u8,
  count: u32,
) PkgError {
  const allocator = std.heap.c_allocator;
  const path = std.mem.span(package_json_path);
  const path_z = package_json_path;

  debug.log("[trust] pkg_add_trusted_dependencies: path={s} count={d}", .{ path, count });

  const file = std.fs.cwd().openFile(path, .{ .mode = .read_write }) catch |err| {
    debug.log("[trust] failed to open file: {}", .{err});
    return .io_error;
  };
  defer file.close();

  const content = file.readToEndAlloc(allocator, 10 * 1024 * 1024) catch |err| {
    debug.log("[trust] failed to read file: {}", .{err});
    return .io_error;
  };
  defer allocator.free(content);

  debug.log("[trust] read {d} bytes from package.json", .{content.len});

  const doc = json.yyjson.yyjson_read(content.ptr, content.len, 0);
  if (doc == null) {
    debug.log("[trust] failed to parse JSON", .{});
    return .io_error;
  }
  defer json.yyjson.yyjson_doc_free(doc);

  const mdoc = json.yyjson.yyjson_doc_mut_copy(doc, null);
  if (mdoc == null) {
    debug.log("[trust] failed to create mutable doc", .{});
    return .out_of_memory;
  }
  defer json.yyjson.yyjson_mut_doc_free(mdoc);

  const root = json.yyjson.yyjson_mut_doc_get_root(mdoc);
  if (root == null) {
    debug.log("[trust] failed to get root", .{});
    return .io_error;
  }

  var trusted_arr = json.yyjson.yyjson_mut_obj_get(root, "trustedDependencies");
  if (trusted_arr == null) {
    debug.log("[trust] creating new trustedDependencies array", .{});
    trusted_arr = json.yyjson.yyjson_mut_arr(mdoc);
    if (trusted_arr == null) {
      debug.log("[trust] failed to create array", .{});
      return .out_of_memory;
    }
    _ = json.yyjson.yyjson_mut_obj_add_val(mdoc, root, "trustedDependencies", trusted_arr);
  } else {
    debug.log("[trust] trustedDependencies array already exists", .{});
  }

  var string_copies: [64][:0]u8 = undefined;
  var string_count: usize = 0;

  var added: u32 = 0;
  for (0..count) |i| {
    const pkg_name = std.mem.span(package_names[i]);
    var exists = false;

    var iter = json.yyjson.yyjson_mut_arr_iter{};
    _ = json.yyjson.yyjson_mut_arr_iter_init(trusted_arr, &iter);
    while (json.yyjson.yyjson_mut_arr_iter_next(&iter)) |val| {
      if (json.yyjson.yyjson_mut_is_str(val)) {
        const existing = json.yyjson.yyjson_mut_get_str(val);
        if (existing != null and std.mem.eql(u8, std.mem.span(existing.?), pkg_name)) {
          exists = true;
          break;
        }
      }
    }

    if (!exists) {
      const name_copy = allocator.dupeZ(u8, pkg_name) catch continue;
      if (string_count < string_copies.len) {
        string_copies[string_count] = name_copy;
        string_count += 1;
      }
      const val = json.yyjson.yyjson_mut_str(mdoc, name_copy.ptr);
      if (val != null) {
        _ = json.yyjson.yyjson_mut_arr_append(trusted_arr, val);
        added += 1;
        debug.log("[trust] added {s}", .{pkg_name});
      }
    } else {
      debug.log("[trust] {s} already in trustedDependencies", .{pkg_name});
    }
  }
  
  defer for (0..string_count) |i| allocator.free(string_copies[i]);
  debug.log("[trust] added {d} packages, writing file", .{added});

  var write_err: json.yyjson.yyjson_write_err = undefined;
  const flags = json.yyjson.YYJSON_WRITE_PRETTY_TWO_SPACES | json.yyjson.YYJSON_WRITE_ESCAPE_UNICODE;
  const written = json.yyjson.yyjson_mut_write_file(path_z, mdoc, flags, null, &write_err);
  if (!written) {
    const msg = if (write_err.msg) |m| std.mem.span(m) else "unknown";
    debug.log("[trust] failed to write file: code={d} msg={s}", .{ write_err.code, msg });
    return .io_error;
  }

  debug.log("[trust] successfully wrote package.json", .{});
  return .ok;
}

export fn pkg_resolve(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  lockfile_out_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  var stage_start: u64 = @intCast(std.time.nanoTimestamp());
  debug.log("resolve: package_json={s} lockfile={s}", .{ std.mem.span(package_json_path), std.mem.span(lockfile_out_path) });

  const http = c.http orelse return .network_error;

  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    c.cache_db,
    if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org",
    &c.metadata_cache,
  );
  defer res.deinit();

  res.resolveFromPackageJson(std.mem.span(package_json_path)) catch |err| {
    c.setErrorFmt("Failed to resolve dependencies: {}", .{err});
    return .resolve_error;
  };
  
  stage_start = debug.timer("resolve dependencies", stage_start);
  debug.log("  resolved {d} packages", .{res.resolved.count()});

  res.writeLockfile(std.mem.span(lockfile_out_path)) catch |err| {
    c.setErrorFmt("Failed to write lockfile: {}", .{err});
    return .io_error;
  };
  _ = debug.timer("write lockfile", stage_start);

  return .ok;
}

const InterleavedExtractCtx = struct {
  ext: *extractor.Extractor,
  integrity: [64]u8,
  cache_path: []const u8,
  pkg_name: []const u8,
  version_str: []const u8,
  direct: bool,
  parent_path: ?[]const u8,
  completed: bool,
  has_error: bool,
  queued: bool,
  parent: *InterleavedContext,
};

const InterleavedContext = struct {
  allocator: std.mem.Allocator,
  arena_alloc: std.mem.Allocator,
  db: *cache.CacheDB,
  http: *fetcher.Fetcher,
  pkg_ctx: *PkgContext,
  extract_contexts: std.ArrayListUnmanaged(*InterleavedExtractCtx),
  queued_integrities: std.AutoHashMap([64]u8, void),
  callbacks_received: usize,
  integrity_duplicates: usize,
  cache_hits: usize,
  tarballs_queued: usize,
  tarballs_completed: std.atomic.Value(u32),

  fn init(allocator: std.mem.Allocator, arena_alloc: std.mem.Allocator, db: *cache.CacheDB, http: *fetcher.Fetcher, pkg_ctx: *PkgContext) InterleavedContext {
    return .{
      .allocator = allocator,
      .arena_alloc = arena_alloc,
      .db = db,
      .http = http,
      .pkg_ctx = pkg_ctx,
      .extract_contexts = .{},
      .queued_integrities = std.AutoHashMap([64]u8, void).init(arena_alloc),
      .callbacks_received = 0,
      .integrity_duplicates = 0,
      .cache_hits = 0,
      .tarballs_queued = 0,
      .tarballs_completed = std.atomic.Value(u32).init(0),
    };
  }

  fn deinit(self: *InterleavedContext) void {
    self.extract_contexts.deinit(self.arena_alloc);
    self.queued_integrities.deinit();
  }

  fn onPackageResolved(pkg: *const resolver.ResolvedPackage, user_data: ?*anyopaque) void {
    const self: *InterleavedContext = @ptrCast(@alignCast(user_data));
    self.callbacks_received += 1;

    const pkg_name = pkg.name.slice();
    const current: u32 = @intCast(self.callbacks_received);
    const msg = std.fmt.allocPrintSentinel(self.arena_alloc, "{s}", .{pkg_name}, 0) catch return;
    self.pkg_ctx.reportProgress(.resolving, current, current, msg);

    if (self.queued_integrities.contains(pkg.integrity)) {
      self.integrity_duplicates += 1; return;
    } self.queued_integrities.put(pkg.integrity, {}) catch return;

    if (self.db.hasIntegrity(&pkg.integrity)) {
      self.cache_hits += 1; return;
    } self.tarballs_queued += 1;

    const cache_path = self.db.getPackagePath(&pkg.integrity, self.arena_alloc) catch return;
    const version_str = std.fmt.allocPrint(self.arena_alloc, "{d}.{d}.{d}", .{
      pkg.version.major, pkg.version.minor, pkg.version.patch,
    }) catch return;

    const ext = extractor.Extractor.init(self.allocator, cache_path) catch return;
    const ctx = self.arena_alloc.create(InterleavedExtractCtx) catch {
      ext.deinit(); return;
    };
    
    ctx.* = .{
      .ext = ext,
      .integrity = pkg.integrity,
      .cache_path = cache_path,
      .pkg_name = pkg.name.slice(),
      .version_str = version_str,
      .direct = pkg.direct,
      .parent_path = pkg.parent_path,
      .completed = false,
      .has_error = false,
      .queued = false,
      .parent = self,
    };

    self.http.fetchTarball(pkg.tarball_url, fetcher.StreamHandler.init(
      struct {
        fn onData(data: []const u8, ud: ?*anyopaque) void {
          const c: *InterleavedExtractCtx = @ptrCast(@alignCast(ud));
          c.ext.feedCompressed(data) catch { c.has_error = true; };
        }
      }.onData,
      struct {
        fn onComplete(_: u16, ud: ?*anyopaque) void {
          const c: *InterleavedExtractCtx = @ptrCast(@alignCast(ud));
          c.completed = true;

          const completed = c.parent.tarballs_completed.fetchAdd(1, .monotonic) + 1;
          const total: u32 = @intCast(c.parent.tarballs_queued);
          
          var msg_buf: [256]u8 = undefined;
          const msg_len = std.fmt.bufPrint(&msg_buf, "{s}", .{c.pkg_name}) catch return;
          
          msg_buf[msg_len.len] = 0;
          c.parent.pkg_ctx.reportProgress(.fetching, completed, total, msg_buf[0..msg_len.len :0]);
        }
      }.onComplete,
      struct {
        fn onError(_: fetcher.FetchError, ud: ?*anyopaque) void {
          const c: *InterleavedExtractCtx = @ptrCast(@alignCast(ud));
          c.has_error = true;
          c.completed = true;
        }
      }.onError,
      ctx,
    )) catch {
      ext.deinit();
      self.arena_alloc.destroy(ctx);
      return;
    };

    ctx.queued = true;
    self.extract_contexts.append(self.arena_alloc, ctx) catch return;

    debug.log("  queued tarball: {s}@{s}", .{ pkg.name.slice(), version_str });
  }
};

export fn pkg_resolve_and_install(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  lockfile_path: [*:0]const u8,
  node_modules_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  var timer = std.time.Timer.start() catch return .out_of_memory;
  var stage_start: u64 = @intCast(std.time.nanoTimestamp());

  debug.log("resolve+install (interleaved): package_json={s} lockfile={s} node_modules={s}", .{
    std.mem.span(package_json_path),
    std.mem.span(lockfile_path),
    std.mem.span(node_modules_path),
  });

  const http = c.http orelse return .network_error;
  const db = c.cache_db orelse return .cache_error;

  const pkg_json_path_z = arena_alloc.dupeZ(u8, std.mem.span(package_json_path)) catch return .out_of_memory;
  var pkg_json = json.PackageJson.parse(arena_alloc, pkg_json_path_z) catch {
    c.setError("Failed to parse package.json");
    return .io_error;
  };  defer pkg_json.deinit(arena_alloc);

  if (pkg_json.trusted_dependencies.count() > 0) {
    debug.log("  trusted dependencies: {d}", .{pkg_json.trusted_dependencies.count()});
  }

  var interleaved = InterleavedContext.init(c.allocator, arena_alloc, db, http, c);
  defer interleaved.deinit();

  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    db,
    if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org",
    &c.metadata_cache,
  ); defer res.deinit();

  res.setOnPackageResolved(InterleavedContext.onPackageResolved, &interleaved);
  res.resolveFromPackageJson(std.mem.span(package_json_path)) catch |err| {
    c.setErrorFmt("Failed to resolve dependencies: {}", .{err});
    return .resolve_error;
  };
  
  stage_start = debug.timer("resolve + queue tarballs", stage_start);
  debug.log("  resolved {d} packages, callbacks={d} (dupes={d}), cache hits={d}, queued={d}", .{
    res.resolved.count(),
    interleaved.callbacks_received,
    interleaved.integrity_duplicates,
    interleaved.cache_hits,
    interleaved.tarballs_queued,
  });

  var direct_iter = res.resolved.valueIterator();
  while (direct_iter.next()) |pkg_ptr| {
    const pkg = pkg_ptr.*;
    if (pkg.direct) {
      const version_str = std.fmt.allocPrint(arena_alloc, "{d}.{d}.{d}", .{
        pkg.version.major,
        pkg.version.minor,
        pkg.version.patch,
      }) catch continue;
      c.addPackageToResults(pkg.name.slice(), version_str, true) catch {};
    }
  }

  var pkg_linker = linker.Linker.init(c.allocator);
  defer pkg_linker.deinit();
  pkg_linker.setNodeModulesPath(std.mem.span(node_modules_path)) catch return .io_error;

  var cache_hit_jobs = std.ArrayListUnmanaged(linker.PackageLink){};
  var pkg_iter = res.resolved.valueIterator();
  while (pkg_iter.next()) |pkg_ptr| {
    const pkg = pkg_ptr.*;
    if (interleaved.queued_integrities.contains(pkg.integrity)) {
      var is_download = false;
      for (interleaved.extract_contexts.items) |ext_ctx| {
        if (std.mem.eql(u8, &ext_ctx.integrity, &pkg.integrity)) { is_download = true; break; }
      }
      if (is_download) continue;
    }

    const cache_entry = db.lookup(&pkg.integrity) orelse continue;
    const cache_path = arena_alloc.dupe(u8, cache_entry.path) catch continue;
    
    cache_hit_jobs.append(arena_alloc, .{
      .cache_path = cache_path,
      .node_modules_path = std.mem.span(node_modules_path),
      .name = pkg.name.slice(),
      .parent_path = pkg.parent_path,
      .file_count = cache_entry.file_count,
    }) catch continue;
  }

  var tarball_thread: ?std.Thread = null;
  if (interleaved.tarballs_queued > 0) {
    debug.log("finishing {d} tarball downloads (pending={d})...", .{
      interleaved.tarballs_queued,
      http.pending.items.len,
    });
    tarball_thread = std.Thread.spawn(.{}, struct {
      fn work(h: *fetcher.Fetcher) void { h.run() catch {}; }
    }.work, .{http}) catch null;
  }

  var linked_count: usize = 0;
  for (cache_hit_jobs.items, 0..) |job, i| {
    const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{job.name}, 0) catch continue;
    c.reportProgress(.linking, @intCast(i), @intCast(cache_hit_jobs.items.len), msg);
    pkg_linker.linkPackage(job) catch continue;
    linked_count += 1;
  }

  if (tarball_thread) |t| {
    t.join();
    stage_start = debug.timer("finish tarballs + link cache hits", stage_start);
  } else stage_start = debug.timer("link cache hits", stage_start);
  debug.log("  linked {d} from cache", .{linked_count});

  res.writeLockfile(std.mem.span(lockfile_path)) catch |err| {
    c.setErrorFmt("Failed to write lockfile: {}", .{err});
    return .io_error;
  };
  stage_start = debug.timer("write lockfile", stage_start);

  var success_count: usize = 0;
  var error_count: usize = 0;

  const LinkJobWithSize = struct {
    job: linker.PackageLink,
    size: u64,
  };

  var cache_entries = std.ArrayListUnmanaged(cache.CacheDB.NamedCacheEntry){};
  var link_jobs = std.ArrayListUnmanaged(LinkJobWithSize){};
  const current_time = std.time.timestamp();
  const nm_path = std.mem.span(node_modules_path);

  for (interleaved.extract_contexts.items) |ext_ctx| {
    if (ext_ctx.has_error or !ext_ctx.completed) {
      error_count += 1;
      debug.log("  error: {s}", .{ext_ctx.pkg_name}); continue;
    }
    success_count += 1;

    const stats = ext_ctx.ext.stats();
    cache_entries.append(arena_alloc, .{
      .entry = .{
        .integrity = ext_ctx.integrity,
        .path = ext_ctx.cache_path,
        .unpacked_size = stats.bytes,
        .file_count = stats.files,
        .cached_at = current_time,
      },
      .name = ext_ctx.pkg_name,
      .version = ext_ctx.version_str,
    }) catch continue;

    link_jobs.append(arena_alloc, .{
      .job = .{
        .cache_path = ext_ctx.cache_path,
        .node_modules_path = nm_path,
        .name = ext_ctx.pkg_name,
        .parent_path = ext_ctx.parent_path,
        .file_count = stats.files,
      },
      .size = stats.bytes,
    }) catch continue;

    c.addPackageToResults(ext_ctx.pkg_name, ext_ctx.version_str, ext_ctx.direct) catch {};
  }

  for (cache_entries.items, 0..) |entry, i| {
    const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{entry.name}, 0) catch continue;
    c.reportProgress(.caching, @intCast(i), @intCast(cache_entries.items.len), msg);
  }
  db.batchInsertNamed(cache_entries.items) catch {};
  stage_start = debug.timer("cache insert (batch)", stage_start);

  const total_jobs: u32 = @intCast(link_jobs.items.len);
  var link_counter = std.atomic.Value(u32).init(0);
  const LARGE_LINK_BYTES: u64 = 2 * 1024 * 1024;

  std.sort.heap(LinkJobWithSize, link_jobs.items, {}, struct {
    fn lessThan(_: void, a: LinkJobWithSize, b: LinkJobWithSize) bool {
      return a.size > b.size;
    }
  }.lessThan);

  var split_idx: usize = link_jobs.items.len;
  for (link_jobs.items, 0..) |job, i| {
    if (job.size < LARGE_LINK_BYTES) {
      split_idx = i;
      break;
    }
  }

  const large_jobs = link_jobs.items[0..split_idx];
  const small_jobs = link_jobs.items[split_idx..];
  const phases = [_][]const LinkJobWithSize{ large_jobs, small_jobs };

  var slow_link_count = std.atomic.Value(u32).init(0);
  var max_link_ms = std.atomic.Value(u64).init(0);
  var slow_link_names = std.ArrayListUnmanaged([]const u8){};
  defer slow_link_names.deinit(c.allocator);
  var slow_link_lock = std.Thread.Mutex{};

  for (phases) |phase_jobs| {
    if (phase_jobs.len == 0) continue;
    const num_threads = @min(8, phase_jobs.len);
    if (c.options.verbose and phase_jobs.len == large_jobs.len) {
      debug.log("  linking large packages first ({d} items)", .{phase_jobs.len});
    }
    if (num_threads > 1 and phase_jobs.len > 4) {
      var threads: [8]?std.Thread = .{null} ** 8;
      const jobs_per_thread = (phase_jobs.len + num_threads - 1) / num_threads;

      for (0..num_threads) |t| {
        const start_idx = t * jobs_per_thread;
        const end_idx = @min(start_idx + jobs_per_thread, phase_jobs.len);
        if (start_idx >= end_idx) break;

        threads[t] = std.Thread.spawn(.{}, struct {
          fn work(lnk: *linker.Linker, jobs: []const LinkJobWithSize, pkg_ctx: *PkgContext, total: u32, counter: *std.atomic.Value(u32), slow_count: *std.atomic.Value(u32), max_ms: *std.atomic.Value(u64), names: *std.ArrayListUnmanaged([]const u8), lock: *std.Thread.Mutex, alloc: std.mem.Allocator) void {
            for (jobs) |job_with_size| {
              const job = job_with_size.job;
              const current = counter.fetchAdd(1, .monotonic) + 1;
              var msg_buf: [256]u8 = undefined;
              const msg_len = std.fmt.bufPrint(&msg_buf, "{s}", .{job.name}) catch continue;
              msg_buf[msg_len.len] = 0;
              pkg_ctx.reportProgress(.linking, current, total, msg_buf[0..msg_len.len :0]);
              const start = std.time.nanoTimestamp();
              lnk.linkPackage(job) catch {};
              const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.time.nanoTimestamp())) - @as(u64, @intCast(start))) / 1_000_000);
              if (elapsed_ms > 100) {
                _ = slow_count.fetchAdd(1, .monotonic);
                lock.lock();
                const entry = std.fmt.allocPrint(alloc, "{s} {d}ms", .{ job.name, elapsed_ms }) catch null;
                if (entry) |val| {
                  names.append(alloc, val) catch {};
                }
                lock.unlock();
                var current_max = max_ms.load(.monotonic);
                while (elapsed_ms > current_max) : (current_max = max_ms.load(.monotonic)) {
                  if (max_ms.cmpxchgWeak(current_max, elapsed_ms, .monotonic, .monotonic) == null) break;
                }
              }
            }
          }
        }.work, .{ &pkg_linker, phase_jobs[start_idx..end_idx], c, total_jobs, &link_counter, &slow_link_count, &max_link_ms, &slow_link_names, &slow_link_lock, c.allocator }) catch null;
      }

      for (&threads) |*t| {
        if (t.*) |thread| thread.join();
      }
    } else {
      for (phase_jobs) |job_with_size| {
        const job = job_with_size.job;
        const current = link_counter.fetchAdd(1, .monotonic) + 1;
        const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{job.name}, 0) catch continue;
        c.reportProgress(.linking, current, total_jobs, msg);
        const start = std.time.nanoTimestamp();
        pkg_linker.linkPackage(job) catch {};
        const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.time.nanoTimestamp())) - @as(u64, @intCast(start))) / 1_000_000);
        if (elapsed_ms > 100 and c.options.verbose) {
          debug.log("  link slow: {s} {d}ms", .{ job.name, elapsed_ms });
        }
      }
    }
  }

  if (c.options.verbose) {
    debug.log("  link slow (>100ms): {d} max={d}ms", .{ slow_link_count.load(.monotonic), max_link_ms.load(.monotonic) });
    for (slow_link_names.items) |entry| {
      debug.log("  link slow: {s}", .{entry});
    }
  }
  
  for (slow_link_names.items) |entry| c.allocator.free(entry);  
  stage_start = debug.timer("link downloads (parallel)", stage_start);
  
  debug.log("  downloaded: {d} success, {d} errors", .{ success_count, error_count });
  for (interleaved.extract_contexts.items) |ext_ctx| ext_ctx.ext.deinit();

  db.sync();
  _ = debug.timer("cache sync", stage_start);

  const link_stats = pkg_linker.getStats();
  c.last_install_result = .{
    .package_count = @intCast(res.resolved.count()),
    .cache_hits = @intCast(interleaved.cache_hits),
    .cache_misses = @intCast(interleaved.tarballs_queued),
    .files_linked = link_stats.files_linked,
    .files_copied = link_stats.files_copied,
    .packages_installed = link_stats.packages_installed,
    .packages_skipped = link_stats.packages_skipped,
    .elapsed_ms = timer.read() / 1_000_000,
  };

  debug.log("total: {d} packages in {d}ms", .{ res.resolved.count(), c.last_install_result.elapsed_ms });

  if (pkg_json.trusted_dependencies.count() > 0) {
    runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc);
  }

  return .ok;
}

const PostinstallJob = struct {
  pkg_name: []const u8,
  pkg_dir: []const u8,
  script: []const u8,
  child: ?std.process.Child = null,
  exit_code: ?u8 = null,
  stderr: ?[]const u8 = null,
  failed: bool = false,
};

fn runTrustedPostinstall(
  ctx: *PkgContext,
  trusted: *std.StringHashMap(void),
  node_modules_path: []const u8,
  allocator: std.mem.Allocator,
) void {
  var env_map = std.process.getEnvMap(allocator) catch return;
  defer env_map.deinit();

  const cwd = std.fs.cwd();
  const abs_nm_path = cwd.realpathAlloc(allocator, node_modules_path) catch return;
  defer allocator.free(abs_nm_path);

  const bin_path = std.fmt.allocPrint(allocator, "{s}/.bin", .{abs_nm_path}) catch return;
  defer allocator.free(bin_path);

  const current_path = env_map.get("PATH") orelse "";
  const new_path = if (builtin.os.tag == .windows)
    std.fmt.allocPrint(allocator, "{s};{s}", .{ bin_path, current_path }) catch return
  else
    std.fmt.allocPrint(allocator, "{s}:{s}", .{ bin_path, current_path }) catch return;
  defer allocator.free(new_path);

  env_map.put("PATH", new_path) catch return;

  var jobs = std.ArrayListUnmanaged(PostinstallJob){};
  defer {
    for (jobs.items) |*job| if (job.stderr) |s| allocator.free(s);
    jobs.deinit(allocator);
  }

  var key_iter = trusted.keyIterator();
  while (key_iter.next()) |pkg_name_ptr| {
    const pkg_name = pkg_name_ptr.*;

    const pkg_json_path = std.fmt.allocPrint(allocator, "{s}/{s}/package.json", .{
      node_modules_path, pkg_name,
    }) catch continue;
    defer allocator.free(pkg_json_path);

    const file = std.fs.cwd().openFile(pkg_json_path, .{}) catch continue;
    defer file.close();

    const content = file.readToEndAlloc(allocator, 1024 * 1024) catch continue;
    defer allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch continue;
    defer doc.deinit();

    const root = doc.root();

    if (root.getObject("scripts")) |scripts| {
      const script = scripts.getString("postinstall") orelse
        scripts.getString("install") orelse continue;

      if (std.mem.eql(u8, pkg_name, "esbuild")) {
        debug.log("ignoring esbuild lifecycle scripts", .{});
        continue;
      }

      const pkg_dir = std.fmt.allocPrint(allocator, "{s}/{s}", .{
        node_modules_path, pkg_name,
      }) catch continue;

      const marker_path = std.fmt.allocPrint(allocator, "{s}/.postinstall", .{pkg_dir}) catch continue;
      defer allocator.free(marker_path);
      if (std.fs.cwd().access(marker_path, .{})) |_| {
        debug.log("postinstall already done: {s}", .{pkg_name});
        allocator.free(pkg_dir);
        continue;
      } else |_| {}

      jobs.append(allocator, .{
        .pkg_name = pkg_name,
        .pkg_dir = pkg_dir,
        .script = allocator.dupe(u8, script) catch continue,
      }) catch continue;
    }
  }

  if (jobs.items.len == 0) return;
  for (jobs.items) |job| debug.log("starting postinstall: {s}", .{job.pkg_name});

  for (jobs.items, 0..) |*job, i| {
    const msg = std.fmt.allocPrintSentinel(allocator, "{s}", .{job.pkg_name}, 0) catch continue;
    ctx.reportProgress(.postinstall, @intCast(i), @intCast(jobs.items.len), msg);
    debug.log("running postinstall: {s}", .{job.pkg_name});

    const shell_argv: []const []const u8 = if (builtin.os.tag == .windows)
      &[_][]const u8{ "cmd", "/c", job.script }
    else
      &[_][]const u8{ "sh", "-c", job.script };

    var child = std.process.Child.init(shell_argv, allocator);
    child.cwd = job.pkg_dir;
    child.env_map = &env_map;
    child.stderr_behavior = .Pipe;
    child.stdout_behavior = .Pipe;

    child.spawn() catch {
      job.failed = true;
      continue;
    };
    job.child = child;
  }

  var scripts_run: u32 = 0;
  for (jobs.items) |*job| {
    if (job.child) |*child| {
      var stdout_buf: std.ArrayList(u8) = .empty;
      var stderr_buf: std.ArrayList(u8) = .empty;

      child.collectOutput(allocator, &stdout_buf, &stderr_buf, 1024 * 1024) catch {};

      const term = child.wait() catch {
        stdout_buf.deinit(allocator);
        stderr_buf.deinit(allocator);
        job.failed = true;
        continue;
      };

      if (stdout_buf.items.len > 0) {
        var line_iter = std.mem.splitScalar(u8, stdout_buf.items, '\n');
        while (line_iter.next()) |line| {
          if (line.len > 0) debug.log("  {s}: {s}", .{ job.pkg_name, line });
        }
      } stdout_buf.deinit(allocator);

      if (term.Exited != 0) {
        job.exit_code = term.Exited;
        job.stderr = if (stderr_buf.items.len > 0) stderr_buf.toOwnedSlice(allocator) catch null else null;
        debug.log("  postinstall failed for {s}: exit code {d}", .{ job.pkg_name, term.Exited });
        if (job.stderr) |s| {
          if (s.len > 0) debug.log("  stderr: {s}", .{s});
        }
      } else {
        stderr_buf.deinit(allocator);
        scripts_run += 1;
        const marker_path = std.fmt.allocPrint(allocator, "{s}/.postinstall", .{job.pkg_dir}) catch continue;
        defer allocator.free(marker_path);
        if (std.fs.cwd().createFile(marker_path, .{})) |f| f.close() else |_| {}
      }
    }
  }

  for (jobs.items) |job| {
    allocator.free(job.pkg_dir);
    allocator.free(job.script);
  }

  if (scripts_run > 0) debug.log("ran {d} postinstall scripts", .{scripts_run});
}

export fn pkg_add(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  package_spec: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const pkg_json_str = std.mem.span(package_json_path);
  const spec_str = std.mem.span(package_spec);

  var pkg_name: []const u8 = spec_str;
  var version_constraint: []const u8 = "latest";

  if (std.mem.indexOf(u8, spec_str, "@")) |at_idx| {
    if (at_idx == 0) {
      if (std.mem.indexOfPos(u8, spec_str, 1, "@")) |second_at| {
        pkg_name = spec_str[0..second_at];
        version_constraint = spec_str[second_at + 1 ..];
      }
    } else {
      pkg_name = spec_str[0..at_idx];
      version_constraint = spec_str[at_idx + 1 ..];
    }
  }

  const http = c.http orelse return .network_error;
  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    c.cache_db,
    if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org",
    &c.metadata_cache,
  ); defer res.deinit();

  const resolved_pkg = res.resolve(pkg_name, version_constraint, 0) catch |err| {
    c.setErrorFmt("Failed to resolve {s}: {}", .{ pkg_name, err });
    return .resolve_error;
  };

  const content = blk: {
    const file = std.fs.cwd().openFile(pkg_json_str, .{ .mode = .read_only }) catch |err| {
      if (err == error.FileNotFound) break :blk "{}";
      c.setError("Failed to open package.json");
      return .io_error;
    };
    defer file.close();
    break :blk file.readToEndAlloc(arena_alloc, 10 * 1024 * 1024) catch {
      c.setError("Failed to read package.json");
      return .io_error;
    };
  };

  const parsed = std.json.parseFromSlice(std.json.Value, arena_alloc, content, .{}) catch {
    c.setError("Failed to parse package.json");
    return .invalid_argument;
  }; defer parsed.deinit();

  if (parsed.value != .object) {
    c.setError("Invalid package.json format");
    return .invalid_argument;
  }

  const version_str = resolved_pkg.version.format(arena_alloc) catch {
    return .out_of_memory;
  };
  
  const version_with_caret = std.fmt.allocPrint(arena_alloc, "^{s}", .{version_str}) catch {
    return .out_of_memory;
  };

  var deps = if (parsed.value.object.get("dependencies")) |d|
    if (d == .object) d.object else std.json.ObjectMap.init(arena_alloc)
  else std.json.ObjectMap.init(arena_alloc);

  deps.put(pkg_name, .{ .string = version_with_caret }) catch {
    return .out_of_memory;
  };

  var writer = json.JsonWriter.init() catch {
    return .out_of_memory;
  }; defer writer.deinit();

  const root_obj = writer.createObject();
  writer.setRoot(root_obj);

  for (parsed.value.object.keys(), parsed.value.object.values()) |key, value| {
    if (std.mem.eql(u8, key, "dependencies")) {
      const deps_obj = writer.createObject();
      var dep_iter = deps.iterator();
      while (dep_iter.next()) |entry| {
        if (entry.value_ptr.* == .string) {
          writer.objectAdd(deps_obj, entry.key_ptr.*, writer.createString(entry.value_ptr.string));
        }
      } writer.objectAdd(root_obj, key, deps_obj);
    } else {
      const json_val = jsonValueToMut(&writer, value) catch continue;
      writer.objectAdd(root_obj, key, json_val);
    }
  }

  if (parsed.value.object.get("dependencies") == null) {
    const deps_obj = writer.createObject();
    writer.objectAdd(deps_obj, pkg_name, writer.createString(version_with_caret));
    writer.objectAdd(root_obj, "dependencies", deps_obj);
  }

  const pkg_json_z = arena_alloc.dupeZ(u8, pkg_json_str) catch {
    return .out_of_memory;
  };
  
  writer.writeToFile(pkg_json_z) catch {
    c.setError("Failed to write package.json");
    return .io_error;
  };

  return .ok;
}

fn jsonValueToMut(writer: *json.JsonWriter, value: std.json.Value) !*json.yyjson.yyjson_mut_val {
  return switch (value) {
    .null => writer.createNull(),
    .bool => |b| writer.createBool(b),
    .integer => |i| writer.createInt(i),
    .float => |f| writer.createReal(f),
    .string => |s| writer.createString(s),
    .array => |arr| blk: {
      const json_arr = writer.createArray();
      for (arr.items) |item| {
        const item_val = try jsonValueToMut(writer, item);
        writer.arrayAppend(json_arr, item_val);
      }
      break :blk json_arr;
    },
    .object => |obj| blk: {
      const json_obj = writer.createObject();
      for (obj.keys(), obj.values()) |k, v| {
        const v_mut = try jsonValueToMut(writer, v);
        writer.objectAdd(json_obj, k, v_mut);
      }
      break :blk json_obj;
    },
    .number_string => |s| writer.createString(s),
  };
}

export fn pkg_remove(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  package_name: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const pkg_json_str = std.mem.span(package_json_path);
  const name_str = std.mem.span(package_name);

  const content = std.fs.cwd().readFileAlloc(arena_alloc, pkg_json_str, 10 * 1024 * 1024) catch {
    c.setError("Failed to read package.json");
    return .io_error;
  };

  const parsed = std.json.parseFromSlice(std.json.Value, arena_alloc, content, .{}) catch {
    c.setError("Failed to parse package.json");
    return .invalid_argument;
  };
  defer parsed.deinit();

  if (parsed.value != .object) {
    c.setError("Invalid package.json format");
    return .invalid_argument;
  }

  const dep_keys = [_][]const u8{ 
    "dependencies",
    "devDependencies",
    "peerDependencies",
    "optionalDependencies"
  };

  const found = found: {
    for (dep_keys) |dep_key| {
      const deps = parsed.value.object.get(dep_key) orelse continue;
      if (deps != .object) continue;
      if (deps.object.get(name_str) != null) break :found true;
    }
    break :found false;
  };

  if (!found) {
    c.setErrorFmt("Package {s} not found in dependencies", .{name_str});
    return .not_found;
  }

  var writer = json.JsonWriter.init() catch return .out_of_memory;
  defer writer.deinit();

  const root_obj = writer.createObject();
  writer.setRoot(root_obj);

  for (parsed.value.object.keys(), parsed.value.object.values()) |key, value| {
    const is_dep_obj = for (dep_keys) |dk| {
      if (std.mem.eql(u8, key, dk)) break true;
    } else false;

    if (!is_dep_obj or value != .object) {
      const val_mut = jsonValueToMut(&writer, value) catch continue;
      writer.objectAdd(root_obj, key, val_mut);
      continue;
    }

    const filtered_obj = writer.createObject();
    for (value.object.keys(), value.object.values()) |dk, dv| {
      if (std.mem.eql(u8, dk, name_str)) continue;
      const dv_mut = jsonValueToMut(&writer, dv) catch continue;
      writer.objectAdd(filtered_obj, dk, dv_mut);
    }
    writer.objectAdd(root_obj, key, filtered_obj);
  }

  const pkg_json_z = arena_alloc.dupeZ(u8, pkg_json_str) catch return .out_of_memory;
  writer.writeToFile(pkg_json_z) catch {
    c.setError("Failed to write package.json");
    return .io_error;
  };

  return .ok;
}

export fn pkg_error_string(ctx: ?*const PkgContext) [*:0]const u8 {
  if (ctx) |c| if (c.last_error) |e| return e.ptr;
  return "Unknown error";
}

export fn pkg_cache_sync(ctx: ?*PkgContext) void {
  if (ctx) |c| if (c.cache_db) |db| db.sync();
}

export fn pkg_cache_stats(ctx: ?*PkgContext, out: *CacheStats) PkgError {
  const c = ctx orelse return .invalid_argument;
  const db = c.cache_db orelse return .cache_error;

  const stats = db.stats() catch return .cache_error;
  out.* = .{
    .total_size = stats.used_size,
    .package_count = @intCast(stats.entries),
  };

  return .ok;
}

export fn pkg_cache_prune(ctx: ?*PkgContext, max_age_days: u32) i32 {
  const c = ctx orelse return @intFromEnum(PkgError.invalid_argument);
  _ = c;
  _ = max_age_days;

  // todo: implement cache pruning
  return 0;
}

export fn pkg_get_bin_path(
  node_modules_path: [*:0]const u8,
  bin_name: [*:0]const u8,
  out_path: [*]u8,
  out_path_len: usize,
) c_int {
  const nm_path = std.mem.span(node_modules_path);
  const name = std.mem.span(bin_name);

  var path_buf: [std.fs.max_path_bytes]u8 = undefined;
  const bin_path = std.fmt.bufPrint(&path_buf, "{s}/.bin/{s}", .{ nm_path, name }) catch return -1;

  std.fs.cwd().access(bin_path, .{}) catch return -1;

  var real_path_buf: [std.fs.max_path_bytes]u8 = undefined;
  const real_path = std.fs.cwd().realpath(bin_path, &real_path_buf) catch return -1;

  if (real_path.len >= out_path_len) return -1;

  @memcpy(out_path[0..real_path.len], real_path);
  out_path[real_path.len] = 0;

  return @intCast(real_path.len);
}

export fn pkg_list_bins(
  node_modules_path: [*:0]const u8,
  callback: ?*const fn ([*:0]const u8, ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) c_int {
  const nm_path = std.mem.span(node_modules_path);

  var path_buf: [std.fs.max_path_bytes]u8 = undefined;
  const bin_dir_path = std.fmt.bufPrint(&path_buf, "{s}/.bin", .{nm_path}) catch return -1;

  var dir = std.fs.cwd().openDir(bin_dir_path, .{ .iterate = true }) catch return -1;
  defer dir.close();

  var count: c_int = 0;
  var iter = dir.iterate();
  while (iter.next() catch null) |entry| {
    if (entry.kind == .sym_link or entry.kind == .file) {
      if (callback) |cb| {
        var name_buf: [256]u8 = undefined;
        if (entry.name.len < name_buf.len) {
          @memcpy(name_buf[0..entry.name.len], entry.name);
          name_buf[entry.name.len] = 0;
          cb(@ptrCast(&name_buf), user_data);
        }
      }
      count += 1;
    }
  }

  return count;
}

export fn pkg_list_package_bins(
  node_modules_path: [*:0]const u8,
  package_name: [*:0]const u8,
  callback: ?*const fn ([*:0]const u8, ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) c_int {
  const nm_path = std.mem.span(node_modules_path);
  const pkg_name = std.mem.span(package_name);

  var path_buf: [std.fs.max_path_bytes]u8 = undefined;
  const pkg_json_path = std.fmt.bufPrint(&path_buf, "{s}/{s}/package.json", .{ nm_path, pkg_name }) catch return -1;

  const file = std.fs.cwd().openFile(pkg_json_path, .{}) catch return -1;
  defer file.close();

  const content = file.readToEndAlloc(global_allocator, 1024 * 1024) catch return -1;
  defer global_allocator.free(content);

  var doc = json.JsonDoc.parse(content) catch return -1;
  defer doc.deinit();

  const root_val = doc.root();
  var count: c_int = 0;

  if (root_val.getObject("bin")) |bin_obj| {
    var iter = bin_obj.objectIterator() orelse return 0;
    while (iter.next()) |entry| {
      if (callback) |cb| {
        var name_buf: [256]u8 = undefined;
        if (entry.key.len < name_buf.len) {
          @memcpy(name_buf[0..entry.key.len], entry.key);
          name_buf[entry.key.len] = 0;
          cb(@ptrCast(&name_buf), user_data);
        }
      }
      count += 1;
    }
  } else if (root_val.getString("bin")) |_| {
    const simple_name = if (std.mem.indexOf(u8, pkg_name, "/")) |slash| pkg_name[slash + 1 ..]
    else pkg_name;

    if (callback) |cb| {
      var name_buf: [256]u8 = undefined;
      if (simple_name.len < name_buf.len) {
        @memcpy(name_buf[0..simple_name.len], simple_name);
        name_buf[simple_name.len] = 0;
        cb(@ptrCast(&name_buf), user_data);
      }
    }
    count = 1;
  }

  return count;
}

export fn pkg_get_script(
  package_json_path: [*:0]const u8,
  script_name: [*:0]const u8,
  out_script: [*]u8,
  out_script_len: usize,
) c_int {
  const allocator = global_allocator;
  const name = std.mem.span(script_name);

  var doc = json.JsonDoc.parseFile(std.mem.span(package_json_path)) catch return -1;
  defer doc.deinit();
  const root_val = doc.root();

  if (root_val.getObject("scripts")) |scripts_obj| {
    if (scripts_obj.getString(std.mem.span(script_name))) |script| {
      if (script.len >= out_script_len) return -1;
      @memcpy(out_script[0..script.len], script);
      out_script[script.len] = 0;
      return @intCast(script.len);
    }
  }

  if (std.mem.eql(u8, name, "start")) {
    if (root_val.getString("main")) |main_file| {
      const script = std.fmt.allocPrint(allocator, "ant {s}", .{main_file}) catch return -1;
      defer allocator.free(script);
      if (script.len >= out_script_len) return -1;
      @memcpy(out_script[0..script.len], script);
      out_script[script.len] = 0;
      return @intCast(script.len);
    }

    if (std.fs.cwd().access("server.js", .{})) |_| {
      const script = "ant server.js";
      if (script.len >= out_script_len) return -1;
      @memcpy(out_script[0..script.len], script);
      out_script[script.len] = 0;
      return @intCast(script.len);
    } else |_| {}
  }

  return -1;
}

pub const ScriptResult = extern struct {
  exit_code: c_int,
  signal: c_int,
};

fn runScriptCommand(
  allocator: std.mem.Allocator,
  script: []const u8,
  extra_args: ?[*:0]const u8,
  env_map: *std.process.EnvMap,
) !ScriptResult {
  const final_script = if (extra_args) |args| blk: {
    const args_str = std.mem.span(args);
    if (args_str.len > 0) {
      break :blk try std.fmt.allocPrint(allocator, "{s} {s}", .{ script, args_str });
    }
    break :blk try allocator.dupe(u8, script);
  } else try allocator.dupe(u8, script);
  defer allocator.free(final_script);

  const script_z = try allocator.dupeZ(u8, final_script);
  defer allocator.free(script_z);

  const shell_argv: []const []const u8 = if (builtin.os.tag == .windows)
    &[_][]const u8{ "cmd", "/c", script_z }
  else &[_][]const u8{ "sh", "-c", script_z };

  var child = std.process.Child.init(shell_argv, allocator);
  child.env_map = env_map;

  try child.spawn();
  const term = try child.wait();

  return switch (term) {
    .Exited => |code| .{ .exit_code = code, .signal = 0 },
    .Signal => |sig| .{ .exit_code = -1, .signal = @intCast(sig) },
    else => .{ .exit_code = -1, .signal = 0 },
  };
}

export fn pkg_run_script(
  package_json_path: [*:0]const u8,
  script_name: [*:0]const u8,
  node_modules_path: [*:0]const u8,
  extra_args: ?[*:0]const u8,
  result: ?*ScriptResult,
) PkgError {
  const allocator = global_allocator;
  const name = std.mem.span(script_name);

  var doc = json.JsonDoc.parseFile(std.mem.span(package_json_path)) catch return .io_error;
  defer doc.deinit(); const root_val = doc.root();

  var script_buf: [8192]u8 = undefined;
  const script_len = pkg_get_script(package_json_path, script_name, &script_buf, script_buf.len);
  
  if (script_len < 0) return .not_found;
  const script = script_buf[0..@intCast(script_len)];

  var pre_script: ?[]const u8 = null;
  var post_script: ?[]const u8 = null;

  if (root_val.getObject("scripts")) |scripts_obj| {
    var pre_key_buf: [256]u8 = undefined;
    var post_key_buf: [256]u8 = undefined;

    const pre_key = std.fmt.bufPrintZ(&pre_key_buf, "pre{s}", .{name}) catch null;
    const post_key = std.fmt.bufPrintZ(&post_key_buf, "post{s}", .{name}) catch null;

    if (pre_key) |pk| pre_script = scripts_obj.getString(pk);
    if (post_key) |pk| post_script = scripts_obj.getString(pk);
  }

  var env_map = std.process.getEnvMap(allocator) catch return .out_of_memory;
  defer env_map.deinit(); const nm_path = std.mem.span(node_modules_path);

  const cwd = std.fs.cwd();
  const abs_nm_path = cwd.realpathAlloc(allocator, nm_path) catch nm_path;
  defer if (abs_nm_path.ptr != nm_path.ptr) allocator.free(abs_nm_path);

  const bin_path = std.fmt.allocPrint(allocator, "{s}/.bin", .{abs_nm_path}) catch return .out_of_memory;
  defer allocator.free(bin_path);

  const current_path = env_map.get("PATH") orelse "";
  const new_path = if (builtin.os.tag == .windows)
    std.fmt.allocPrint(allocator, "{s};{s}", .{ bin_path, current_path }) catch return .out_of_memory
  else
    std.fmt.allocPrint(allocator, "{s}:{s}", .{ bin_path, current_path }) catch return .out_of_memory;
  defer allocator.free(new_path);

  env_map.put("PATH", new_path) catch return .out_of_memory;
  env_map.put("npm_lifecycle_event", name) catch {};

  if (root_val.getObject("config")) |config_obj| {
    if (config_obj.objectIterator()) |*config_iter_ptr| {
      var config_iter = config_iter_ptr.*;
      while (config_iter.next()) |entry| {
        if (entry.value.asString()) |value| {
          const env_key = std.fmt.allocPrint(allocator, "npm_package_config_{s}", .{entry.key}) catch continue;
          defer allocator.free(env_key);
          env_map.put(env_key, value) catch {};
        }
      }
    }
  }

  if (root_val.getString("name")) |pkg_name| env_map.put("npm_package_name", pkg_name) catch {};
  if (root_val.getString("version")) |pkg_version| env_map.put("npm_package_version", pkg_version) catch {};

  if (pre_script) |pre| {
    env_map.put("npm_lifecycle_event", std.fmt.allocPrint(allocator, "pre{s}", .{name}) catch name) catch {};
    const pre_result = runScriptCommand(allocator, pre, null, &env_map) catch return .io_error;
    if (pre_result.exit_code != 0) {
      if (result) |r| r.* = pre_result;
      return .ok;
    }
  }

  env_map.put("npm_lifecycle_event", name) catch {};
  const main_result = runScriptCommand(allocator, script, extra_args, &env_map) catch return .io_error;

  if (main_result.exit_code != 0) {
    if (result) |r| r.* = main_result;
    return .ok;
  }

  if (post_script) |post| {
    env_map.put("npm_lifecycle_event", std.fmt.allocPrint(allocator, "post{s}", .{name}) catch name) catch {};
    const post_result = runScriptCommand(allocator, post, null, &env_map) catch return .io_error;
    if (result) |r| r.* = post_result;
    return .ok;
  }

  if (result) |r| r.* = main_result;
  return .ok;
}

pub const DepType = packed struct(u8) {
  peer: bool = false,
  dev: bool = false,
  optional: bool = false,
  direct: bool = false,
  _reserved: u4 = 0,
};

pub const DepCallback = ?*const fn (
  name: [*:0]const u8,
  version: [*:0]const u8,
  constraint: [*:0]const u8,
  dep_type: DepType,
  user_data: ?*anyopaque,
) callconv(.c) void;

pub const WhyInfo = extern struct {
  target_version: [64]u8,
  found: bool,
  is_peer: bool,
  is_dev: bool,
  is_direct: bool,
};

export fn pkg_why_info(
  lockfile_path: [*:0]const u8,
  package_name: [*:0]const u8,
  out: *WhyInfo,
) c_int {
  const lf = lockfile.Lockfile.open(std.mem.span(lockfile_path)) catch return -1;
  defer @constCast(&lf).close();

  const target_name = std.mem.span(package_name);
  out.found = false;
  out.is_peer = false;
  out.is_dev = false;
  out.is_direct = false;
  @memset(&out.target_version, 0);

  for (lf.packages) |*pkg| {
    const pkg_name = pkg.name.slice(lf.string_table);
    if (std.mem.eql(u8, pkg_name, target_name)) {
      const ver_str = pkg.versionString(global_allocator, lf.string_table) catch return -1;
      defer global_allocator.free(ver_str);
      if (ver_str.len < out.target_version.len) {
        @memcpy(out.target_version[0..ver_str.len], ver_str);
        out.target_version[ver_str.len] = 0;
      }
      out.found = true;
      out.is_dev = pkg.flags.dev;
      out.is_direct = pkg.flags.direct;
    }

    const deps = lf.getPackageDeps(pkg);
    for (deps) |dep| {
      const dep_pkg = &lf.packages[dep.package_index];
      const dep_name = dep_pkg.name.slice(lf.string_table);
      if (std.mem.eql(u8, dep_name, target_name) and dep.flags.peer) {
        out.is_peer = true;
      }
    }
  }
  return 0;
}

export fn pkg_why(
  lockfile_path: [*:0]const u8,
  package_name: [*:0]const u8,
  callback: DepCallback,
  user_data: ?*anyopaque,
) c_int {
  const lf = lockfile.Lockfile.open(std.mem.span(lockfile_path)) catch return -1;
  defer @constCast(&lf).close();

  const target_name = std.mem.span(package_name);
  var count: c_int = 0;

  var name_buf: [512]u8 = undefined;
  var ver_buf: [64]u8 = undefined;
  var constraint_buf: [128]u8 = undefined;

  for (lf.packages) |*pkg| {
    const deps = lf.getPackageDeps(pkg);
    for (deps) |dep| {
      const dep_pkg = &lf.packages[dep.package_index];
      const dep_name = dep_pkg.name.slice(lf.string_table);

      if (std.mem.eql(u8, dep_name, target_name)) {
        const pkg_name = pkg.name.slice(lf.string_table);
        const constraint = dep.constraint.slice(lf.string_table);

        if (callback) |cb| {
          if (pkg_name.len < name_buf.len) {
            @memcpy(name_buf[0..pkg_name.len], pkg_name);
            name_buf[pkg_name.len] = 0;

            const ver_str = pkg.versionString(global_allocator, lf.string_table) catch continue;
            defer global_allocator.free(ver_str);
            if (ver_str.len < ver_buf.len) {
              @memcpy(ver_buf[0..ver_str.len], ver_str);
              ver_buf[ver_str.len] = 0;

              if (constraint.len < constraint_buf.len) {
                @memcpy(constraint_buf[0..constraint.len], constraint);
                constraint_buf[constraint.len] = 0;

                const dep_type = DepType{
                  .peer = dep.flags.peer,
                  .dev = dep.flags.dev or pkg.flags.dev,
                  .optional = dep.flags.optional,
                  .direct = pkg.flags.direct,
                };
                cb(@ptrCast(&name_buf), @ptrCast(&ver_buf), @ptrCast(&constraint_buf), dep_type, user_data);
              }
            }
          }
        }
        count += 1;
      }
    }
  }

  for (lf.packages) |*pkg| {
    const pkg_name = pkg.name.slice(lf.string_table);
    if (std.mem.eql(u8, pkg_name, target_name) and pkg.flags.direct) {
      if (callback) |cb| {
        const direct_str = "package.json";
        var direct_buf: [16]u8 = undefined;
        @memcpy(direct_buf[0..direct_str.len], direct_str);
        direct_buf[direct_str.len] = 0;

        var empty_buf: [1]u8 = .{0};
        var dep_buf: [16]u8 = undefined;
        const constraint_str = "dependencies";
        @memcpy(dep_buf[0..constraint_str.len], constraint_str);
        dep_buf[constraint_str.len] = 0;

        const dep_type = DepType{
          .peer = false,
          .dev = pkg.flags.dev,
          .optional = false,
          .direct = true,
        };
        cb(@ptrCast(&direct_buf), @ptrCast(&empty_buf), @ptrCast(&dep_buf), dep_type, user_data);
      }
      count += 1;
    }
  }

  return count;
}

export fn pkg_list_scripts(
  package_json_path: [*:0]const u8,
  callback: ?*const fn ([*:0]const u8, [*:0]const u8, ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) c_int {
  var doc = json.JsonDoc.parseFile(std.mem.span(package_json_path)) catch return -1;
  defer doc.deinit();

  const root_val = doc.root();
  const scripts_obj = root_val.getObject("scripts") orelse return 0;

  var iter = scripts_obj.objectIterator() orelse return 0;
  defer iter.deinit();

  var count: c_int = 0;
  while (iter.next()) |entry| {
    if (callback) |cb| {
      var name_buf: [256]u8 = undefined;
      var cmd_buf: [4096]u8 = undefined;

      if (entry.key.len < name_buf.len) {
        @memcpy(name_buf[0..entry.key.len], entry.key);
        name_buf[entry.key.len] = 0;

        if (entry.value.asString()) |cmd| {
          if (cmd.len < cmd_buf.len) {
            @memcpy(cmd_buf[0..cmd.len], cmd);
            cmd_buf[cmd.len] = 0;
            cb(@ptrCast(&name_buf), @ptrCast(&cmd_buf), user_data);
          }
        }
      }
    }
    count += 1;
  }

  return count;
}
