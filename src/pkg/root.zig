const std = @import("std");
const builtin = @import("builtin");
const process_env = @import("process_env.zig");
const io = std.Io.Threaded.global_single_threaded.io();

pub const cli = @import("cli.zig");
pub const lockfile = @import("lockfile.zig");
pub const cache = @import("cache.zig");
pub const fetcher = @import("fetcher.zig");
pub const extractor = @import("extractor.zig");
pub const linker = @import("linker.zig");
pub const resolver = @import("resolver.zig");
pub const intern = @import("intern.zig");
pub const json = @import("json.zig");
pub const debug = @import("debug.zig");
pub const platform = @import("platform.zig");

const global_allocator: std.mem.Allocator = std.heap.c_allocator;

fn getHomeDir(allocator: std.mem.Allocator) ![]const u8 {
  if (builtin.os.tag == .windows) {
    const home = std.process.Environ.getAlloc(.{ .block = .global }, allocator, "USERPROFILE") catch return error.NoHomeDir;
    if (home.len == 0) {
      allocator.free(home);
      return error.NoHomeDir;
    }
    return home;
  }
  const home = std.c.getenv("HOME") orelse return error.NoHomeDir;
  return allocator.dupe(u8, std.mem.span(home));
}

fn getAbsoluteEnv(name: [:0]const u8) ?[]const u8 {
  if (builtin.os.tag == .windows) return null;
  const value = std.c.getenv(name) orelse return null;
  const value_slice = std.mem.span(value);
  if (value_slice.len == 0 or !std.fs.path.isAbsolute(value_slice)) return null;
  return value_slice;
}

fn currentEnvMap(allocator: std.mem.Allocator) !std.process.Environ.Map {
  return process_env.current().createMap(allocator);
}

fn scriptShell() []const u8 {
  if (builtin.os.tag == .windows) return "cmd";
  if (std.c.getenv("ANT_SCRIPT_SHELL")) |shell| {
    const value = std.mem.span(shell);
    if (value.len > 0) return value;
  }
  if (std.c.getenv("SHELL")) |shell| {
    const value = std.mem.span(shell);
    if (value.len > 0) return value;
  }
  std.Io.Dir.cwd().access(io, "/bin/sh", .{}) catch return "sh";
  return "/bin/sh";
}

fn isAntsLandRegistry(registry_url: []const u8) bool {
  var host = registry_url;
  if (std.mem.startsWith(u8, host, "https://")) host = host["https://".len..];
  if (std.mem.startsWith(u8, host, "http://")) host = host["http://".len..];
  if (std.mem.endsWith(u8, host, "/")) host = host[0 .. host.len - 1];
  return std.mem.eql(u8, host, "npm.ants.land");
}

fn shouldWriteLandTarballDependency(c: *PkgContext, package_name: []const u8) bool {
  if (std.mem.startsWith(u8, package_name, "@")) return false;
  const registry_url = if (c.options.registry_url) |url| std.mem.span(url) else return false;
  return isAntsLandRegistry(registry_url);
}

fn dependencyValueForResolved(
  allocator: std.mem.Allocator,
  c: *PkgContext,
  package_name: []const u8,
  version_str: []const u8,
  resolved_pkg: *const resolver.ResolvedPackage,
) ![]const u8 {
  if (shouldWriteLandTarballDependency(c, package_name)) {
    return allocator.dupe(u8, resolved_pkg.tarball_url);
  }
  return std.fmt.allocPrint(allocator, "^{s}", .{version_str});
}

fn lockfilePackageBins(allocator: std.mem.Allocator, lf: *const lockfile.Lockfile, package_index: u32) ![]const linker.PackageBin {
  const entries = lf.getPackageBins(package_index);
  if (entries.len == 0) return &[_]linker.PackageBin{};
  const bins = try allocator.alloc(linker.PackageBin, entries.len);
  for (entries, 0..) |entry, i| {
    bins[i] = .{
      .name = entry.name.slice(lf.string_table),
      .path = entry.path.slice(lf.string_table),
    };
  }
  return bins;
}

fn resolvedPackageBins(allocator: std.mem.Allocator, pkg: *const resolver.ResolvedPackage) ![]const linker.PackageBin {
  if (pkg.bins.items.len == 0) return &[_]linker.PackageBin{};
  const bins = try allocator.alloc(linker.PackageBin, pkg.bins.items.len);
  for (pkg.bins.items, 0..) |bin, i| {
    bins[i] = .{ .name = bin.name, .path = bin.path };
  }
  return bins;
}

fn getLegacyAntDirIfExists(allocator: std.mem.Allocator) !?[]const u8 {
  const home = try getHomeDir(allocator);
  defer allocator.free(home);

  const dir = try std.fmt.allocPrint(allocator, "{s}/.ant", .{home});
  std.Io.Dir.cwd().access(io, dir, .{}) catch {
    allocator.free(dir);
    return null;
  };
  return dir;
}

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
  force: bool = false,
};

pub const CacheStats = extern struct {
  total_size: u64,
  db_size: u64,
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
  lifecycle_builds: u32,
  elapsed_ms: u64,
};

pub const LifecycleScript = extern struct {
  name: [*:0]const u8,
  script: [*:0]const u8,
};

const PkgInfo = extern struct {
  name: [*:0]const u8,
  version: [*:0]const u8,
  description: [*:0]const u8,
  license: [*:0]const u8,
  homepage: [*:0]const u8,
  tarball: [*:0]const u8,
  shasum: [*:0]const u8,
  integrity: [*:0]const u8,
  keywords: [*:0]const u8,
  published: [*:0]const u8,
  dep_count: u32,
  version_count: u32,
  unpacked_size: u64,
};

const DistTag = extern struct {
  tag: [*:0]const u8,
  version: [*:0]const u8,
};

const Maintainer = extern struct {
  name: [*:0]const u8,
  email: [*:0]const u8,
};

const Dependency = extern struct {
  name: [*:0]const u8,
  version: [*:0]const u8,
};

pub const RegistryChoice = enum(c_int) {
  unknown = 0,
  primary = 1,
  fallback = 2,
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
  info_dist_tags: std.ArrayListUnmanaged(DistTag),
  info_maintainers: std.ArrayListUnmanaged(Maintainer),
  info_dependencies: std.ArrayListUnmanaged(Dependency),
  info_storage: std.ArrayListUnmanaged([:0]u8),

  pub fn init(allocator: std.mem.Allocator, options: PkgOptions) !*PkgContext {
    debug.enabled = options.verbose;
    const total_start = debug.nowNs();
    var stage_start = total_start;
    var trace = debug.StageTrace{};
    debug.trace("context init start", .{});

    const ctx = try allocator.create(PkgContext);
    errdefer allocator.destroy(ctx);
    stage_start = trace.mark("context allocate", stage_start);
    
    const default_cache_path = if (options.cache_dir == null) 
      try getDefaultCacheDir(allocator) else null;
    stage_start = trace.mark("context cache path", stage_start);
    
    defer { if (default_cache_path) |path| allocator.free(path); }
    const cache_path = if (options.cache_dir) |dir| std.mem.span(dir) else default_cache_path.?;
    
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
        .lifecycle_builds = 0,
        .elapsed_ms = 0
      },
      .added_packages = .empty,
      .added_packages_storage = .empty,
      .lifecycle_scripts = .empty,
      .lifecycle_scripts_storage = .empty,
      .info_dist_tags = .empty,
      .info_maintainers = .empty,
      .info_dependencies = .empty,
      .info_storage = .empty,
    };
    stage_start = trace.mark("context fields init", stage_start);

    debug.log("init: cache_dir={s}", .{ctx.cache_dir});
    ctx.cache_db = cache.CacheDB.open(ctx.cache_dir) catch |err| {
      ctx.setErrorFmt("Failed to open cache database: {}", .{err});
      return error.CacheError;
    };
    stage_start = trace.mark("context cache open", stage_start);
    
    debug.log("init: cache database opened", .{});
    const registry = if (options.registry_url) |url|
      std.mem.span(url)
    else
      "registry.npmjs.org";

    stage_start = trace.mark("context registry select", stage_start);
    ctx.http = fetcher.Fetcher.init(allocator, registry) catch |err| {
      ctx.setErrorFmt("Failed to initialize fetcher: {}", .{err});
      return error.NetworkError;
    };
    _ = trace.mark("context fetcher init", stage_start);
    debug.log("init: http fetcher ready, registry={s}", .{registry});
    trace.summary("context init");
    debug.trace("context init done: registry={s} total={d}us", .{
      registry,
      debug.elapsedUsSince(total_start),
    });

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
    for (self.info_storage.items) |s| self.allocator.free(s);
    self.info_storage.deinit(self.allocator);
    self.info_dist_tags.deinit(self.allocator);
    self.info_maintainers.deinit(self.allocator);
    self.info_dependencies.deinit(self.allocator);
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

  pub fn clearError(self: *PkgContext) void {
    if (self.last_error) |e| self.allocator.free(e);
    self.last_error = null;
  }

  fn getDefaultCacheDir(allocator: std.mem.Allocator) ![]const u8 {
    if (builtin.os.tag != .windows) {
      if (try getLegacyAntDirIfExists(allocator)) |dir| {
        defer allocator.free(dir);
        return std.fmt.allocPrint(allocator, "{s}/pkg", .{dir});
      }
      if (getAbsoluteEnv("XDG_CACHE_HOME")) |base| {
        return std.fmt.allocPrint(allocator, "{s}/ant/pkg", .{base});
      }
      const home = try getHomeDir(allocator);
      defer allocator.free(home);
      return std.fmt.allocPrint(allocator, "{s}/.cache/ant/pkg", .{home});
    }

    const home = try getHomeDir(allocator);
    defer allocator.free(home);
    return std.fmt.allocPrint(allocator, "{s}/.ant/pkg", .{home});
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

  fn clearInfo(self: *PkgContext) void {
    for (self.info_storage.items) |s| self.allocator.free(s);
    self.info_storage.clearRetainingCapacity();
    self.info_dist_tags.clearRetainingCapacity();
    self.info_maintainers.clearRetainingCapacity();
    self.info_dependencies.clearRetainingCapacity();
  }

  fn storeInfoString(self: *PkgContext, str: []const u8) ![*:0]const u8 {
    const z = try self.allocator.dupeZ(u8, str);
    try self.info_storage.append(self.allocator, z);
    return z.ptr;
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
    self.last_install_result = .{
      .package_count = 0,
      .cache_hits = 0,
      .cache_misses = 0,
      .files_linked = 0,
      .files_copied = 0,
      .packages_installed = 0,
      .packages_skipped = 0,
      .lifecycle_builds = 0,
      .elapsed_ms = 0,
    };

    const timer = std.Io.Clock.Timestamp.now(io, .boot);
    var stage_start: u64 = @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds());
    var trace = debug.StageTrace{};

    debug.log("install start: lockfile={s} node_modules={s}", .{ lockfile_path, node_modules_path });

    var lf = lockfile.Lockfile.open(lockfile_path) catch {
      self.setError("Failed to open lockfile");
      return error.InvalidLockfile;
    };
    defer lf.close();
    if (!lf.header.matchesCurrentPlatform()) {
      self.setError("Lockfile was generated for a different platform");
      return error.InvalidLockfile;
    }
    const trust_installed = installStateMarkerMatches(arena_alloc, node_modules_path, lf.header.graph_hash);
    removeInstallStateMarker(arena_alloc, node_modules_path);

    const pkg_count = lf.header.package_count;
    stage_start = trace.mark("lockfile open", stage_start);
    debug.log("  packages in lockfile: {d}", .{pkg_count});

    var integrities = try arena_alloc.alloc([64]u8, pkg_count);
    for (lf.packages, 0..) |pkg, i| {
      integrities[i] = pkg.integrity;
    }

    const db = self.cache_db orelse return error.CacheError;
    var cache_hits = try db.batchLookup(integrities, arena_alloc);
    defer cache_hits.deinit();
    if (self.options.force) cache_hits.clear();
    stage_start = trace.mark("cache lookup", stage_start);

    var hit_set = std.AutoHashMap(u32, u32).init(arena_alloc);
    for (cache_hits.items) |hit| {
      try hit_set.put(hit.index, hit.file_count);
    }

    var misses = std.ArrayListUnmanaged(u32).empty;
    for (0..pkg_count) |i| {
      if (!hit_set.contains(@intCast(i))) {
        try misses.append(arena_alloc, @intCast(i));
      }
    }
    debug.log("  cache hits: {d}, misses: {d}", .{ cache_hits.items.len, misses.items.len });

    std.sort.heap(cache.CacheDB.BatchHit, cache_hits.items, &lf, batchHitLessThanParentFirst);

    var pkg_linker = linker.Linker.init(self.allocator);
    defer pkg_linker.deinit();
    try pkg_linker.setNodeModulesPath(node_modules_path);

    for (cache_hits.items, 0..) |hit, i| {
      const pkg = &lf.packages[hit.index];
      const pkg_name = pkg.name.slice(lf.string_table);
      const cache_path = try db.getPackagePath(&pkg.integrity, arena_alloc);
      const parent_path = pkg.parent_path.slice(lf.string_table);
      const file_count = if (hit.file_count != 0)
        hit.file_count
      else if (lf.header.hasInstallMetadata())
        pkg.fileCount()
      else
        0;
      const bins = lockfilePackageBins(arena_alloc, &lf, hit.index) catch &[_]linker.PackageBin{};

      const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{pkg_name}, 0) catch continue;
      self.reportProgress(.linking, @intCast(i), @intCast(cache_hits.items.len), msg);

      try pkg_linker.linkPackage(.{
        .cache_path = cache_path,
        .node_modules_path = node_modules_path,
        .name = pkg_name,
        .parent_path = if (parent_path.len > 0) parent_path else null,
        .file_count = file_count,
        .has_bin = pkg.flags.has_bin,
        .bins = bins,
        .trust_installed = trust_installed,
        .allow_dir_symlink = (!pkg.flags.has_bin and pkg.deps_count == 0) or
          (pkg.flags.has_bin and file_count >= linker.DIR_CLONE_THRESHOLD and parent_path.len == 0),
      });

      if (pkg.flags.direct) {
        const version_str = pkg.versionString(arena_alloc, lf.string_table) catch continue;
        self.addPackageToResults(pkg_name, version_str, true) catch {};
      }
    }
    stage_start = trace.mark("link cache hits", stage_start);

    if (misses.items.len > 0) {
      const http = self.http orelse return error.NetworkError;
      const PkgExtractCtx = struct {
        ext: *extractor.Extractor,
        pkg_idx: u32,
        integrity: [64]u8,
        cache_path: []const u8,
        staging_path: ?[]const u8,
        pkg_name: []const u8,
        version_str: []const u8,
        direct: bool,
        parent_path: ?[]const u8,
        has_bin: bool,
        bins: []const linker.PackageBin,
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

        var staging_path: ?[]const u8 = null;
        if (self.options.force) {
          staging_path = std.fmt.allocPrint(arena_alloc, "{s}.force-tmp", .{cache_path}) catch continue;
          std.Io.Dir.cwd().deleteTree(io, staging_path.?) catch {};
        }

        const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{pkg_name}, 0) catch continue;
        self.reportProgress(.fetching, @intCast(i), @intCast(misses.items.len), msg);

        if (self.options.verbose) {
          debug.log("  queue: {s}@{s}", .{ pkg_name, version_str });
        }

        const ext = extractor.Extractor.init(self.allocator, staging_path orelse cache_path) catch continue;
        const parent_path_str = pkg.parent_path.slice(lf.string_table);
        const bins = lockfilePackageBins(arena_alloc, &lf, pkg_idx) catch &[_]linker.PackageBin{};

        extract_contexts[valid_count] = .{
          .ext = ext,
          .pkg_idx = pkg_idx,
          .integrity = pkg.integrity,
          .cache_path = cache_path,
          .staging_path = staging_path,
          .pkg_name = pkg_name,
          .version_str = version_str,
          .direct = pkg.flags.direct,
          .parent_path = if (parent_path_str.len > 0) parent_path_str else null,
          .has_bin = pkg.flags.has_bin,
          .bins = bins,
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
            fn onComplete(status: u16, user_data: ?*anyopaque) void {
              const ctx: *PkgExtractCtx = @ptrCast(@alignCast(user_data));
              if (status != 200) ctx.has_error = true;
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
      
      stage_start = trace.mark("queue fetches", stage_start);
      debug.log("running event loop for {d} fetches...", .{valid_count});
      
      http.run() catch {};
      stage_start = trace.mark("fetch + extract", stage_start);

      var success_count: usize = 0;
      var error_count: usize = 0;
      var link_error_count: usize = 0;
      std.sort.heap(PkgExtractCtx, extract_contexts[0..valid_count], {}, struct {
        fn lessThan(_: void, a: PkgExtractCtx, b: PkgExtractCtx) bool {
          const depth_a = linkPathDepth(a.parent_path);
          const depth_b = linkPathDepth(b.parent_path);
          if (depth_a != depth_b) return depth_a < depth_b;

          const parent_a = a.parent_path orelse "";
          const parent_b = b.parent_path orelse "";
          const parent_order = std.mem.order(u8, parent_a, parent_b);
          if (parent_order != .eq) return parent_order == .lt;
          return std.mem.order(u8, a.pkg_name, b.pkg_name) == .lt;
        }
      }.lessThan);

      for (extract_contexts[0..valid_count], 0..) |*ctx, i| {
        defer ctx.ext.deinit();

        if (ctx.has_error or !ctx.completed or !ctx.ext.isComplete()) {
          error_count += 1;
          if (ctx.staging_path) |staging| std.Io.Dir.cwd().deleteTree(io, staging) catch {};
          debug.log("  error: {s}", .{ctx.pkg_name});
          continue;
        }

        // --force: swap staging into the real store path now that it built cleanly.
        if (ctx.staging_path) |staging| {
          const from_z = arena_alloc.dupeZ(u8, staging) catch { error_count += 1; std.Io.Dir.cwd().deleteTree(io, staging) catch {}; continue; };
          const to_z = arena_alloc.dupeZ(u8, ctx.cache_path) catch { error_count += 1; continue; };
          std.Io.Dir.cwd().deleteTree(io, ctx.cache_path) catch {};
          if (std.c.rename(from_z.ptr, to_z.ptr) != 0) {
            db.delete(&ctx.integrity) catch {};
            std.Io.Dir.cwd().deleteTree(io, staging) catch {};
            error_count += 1;
            debug.log("  error: {s} (force swap failed)", .{ctx.pkg_name});
            continue;
          }
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
          .cached_at = std.Io.Timestamp.now(io, .real).toSeconds(),
        }, ctx.pkg_name, ctx.version_str) catch continue;

        self.addPackageToResults(ctx.pkg_name, ctx.version_str, ctx.direct) catch {};

        pkg_linker.linkPackage(.{
          .cache_path = ctx.cache_path,
          .node_modules_path = node_modules_path,
          .name = ctx.pkg_name,
          .parent_path = ctx.parent_path,
          .file_count = stats.files,
          .has_bin = ctx.has_bin,
          .bins = ctx.bins,
          .trust_installed = trust_installed,
        }) catch |err| {
          link_error_count += 1;
          debug.log("  link error: {s}: {s}", .{ ctx.pkg_name, @errorName(err) });
        };
      }
      stage_start = trace.mark("cache insert + link misses", stage_start);
      debug.log("  fetched: {d} success, {d} errors", .{ success_count, error_count });
      if (error_count > 0) {
        self.setErrorFmt("Failed to fetch or extract {d} package{s}", .{
          error_count,
          if (error_count == 1) "" else "s",
        });
        return error.IoError;
      }
      if (link_error_count > 0) return error.IoError;
    }

    db.sync();
    _ = trace.mark("cache sync", stage_start);
    writeInstallStateMarker(arena_alloc, node_modules_path, lf.header.graph_hash);

    const link_stats = pkg_linker.getStats();
    self.last_install_result = .{
      .package_count = pkg_count,
      .cache_hits = @intCast(cache_hits.items.len),
      .cache_misses = @intCast(misses.items.len),
      .files_linked = link_stats.files_linked,
      .files_copied = link_stats.files_copied,
      .packages_installed = link_stats.packages_installed,
      .packages_skipped = link_stats.packages_skipped,
      .lifecycle_builds = 0,
      .elapsed_ms = @intCast(timer.untilNow(io).raw.toMilliseconds()),
    };
    trace.summary("lockfile install");
    debug.trace("lockfile install total: packages={d} cached={d} fetched={d} files_linked={d} files_copied={d} total={d}ms", .{
      pkg_count,
      cache_hits.items.len,
      misses.items.len,
      self.last_install_result.files_linked,
      self.last_install_result.files_copied,
      self.last_install_result.elapsed_ms,
    });
  }
};

fn trySetHttpResolveError(c: *PkgContext, err: anyerror) bool {
  if (err != error.ResponseError) return false;

  const http = c.http orelse return false;
  const info = http.getLastHttpError() orelse return false;

  c.setErrorFmt("error: GET {s} - {d}", .{ info.url, info.status });
  return true;
}

fn setResolveError(c: *PkgContext, target: ?[]const u8, err: anyerror) void {
  if (trySetHttpResolveError(c, err)) return;

  if (target) |name| c.setErrorFmt("Failed to resolve {s}: {}", .{ name, err })
  else c.setErrorFmt("Failed to resolve dependencies: {}", .{ err });
}

fn linkPathDepth(parent_path: ?[]const u8) u32 {
  const parent = parent_path orelse return 0;
  if (parent.len == 0) return 0;

  var depth: u32 = 1;
  var it: usize = 0;
  while (std.mem.indexOfPos(u8, parent, it, "/node_modules/")) |idx| {
    depth += 1;
    it = idx + "/node_modules/".len;
  }
  return depth;
}

fn packageLinkLessThanParentFirst(_: void, a: linker.PackageLink, b: linker.PackageLink) bool {
  const a_depth = linkPathDepth(a.parent_path);
  const b_depth = linkPathDepth(b.parent_path);
  if (a_depth != b_depth) return a_depth < b_depth;

  const a_parent = a.parent_path orelse "";
  const b_parent = b.parent_path orelse "";
  const parent_order = std.mem.order(u8, a_parent, b_parent);
  if (parent_order != .eq) return parent_order == .lt;

  return std.mem.order(u8, a.name, b.name) == .lt;
}

fn resolvedPackageHasNestedChildren(res: *resolver.Resolver, pkg: *const resolver.ResolvedPackage, allocator: std.mem.Allocator) bool {
  const install_path = pkg.installPath(allocator) catch return true;
  defer allocator.free(install_path);

  var iter = res.resolved.valueIterator();
  while (iter.next()) |other_ptr| {
    const other = other_ptr.*;
    if (other == pkg) continue;
    if (other.parent_path) |parent| {
      if (std.mem.eql(u8, parent, install_path)) return true;
    }
  }

  return false;
}

const ResolutionCacheEntry = struct {
  section: u8,
  name: []const u8,
  version: []const u8,
};

fn resolutionCacheEntryLessThan(_: void, a: ResolutionCacheEntry, b: ResolutionCacheEntry) bool {
  if (a.section != b.section) return a.section < b.section;
  const name_order = std.mem.order(u8, a.name, b.name);
  if (name_order != .eq) return name_order == .lt;
  return std.mem.order(u8, a.version, b.version) == .lt;
}

fn isRegistryResolutionCacheable(version: []const u8) bool {
  if (version.len == 0) return false;
  const unsupported = [_][]const u8{
    "file:",
    "link:",
    "workspace:",
    "git:",
    "github:",
    "http://",
    "https://",
  };
  for (unsupported) |prefix| {
    if (std.mem.startsWith(u8, version, prefix)) return false;
  }
  return true;
}

fn appendResolutionCacheEntries(
  allocator: std.mem.Allocator,
  entries: *std.ArrayListUnmanaged(ResolutionCacheEntry),
  section: u8,
  deps: *const std.StringHashMap([]const u8),
) !bool {
  var iter = deps.iterator();
  while (iter.next()) |entry| {
    if (!isRegistryResolutionCacheable(entry.value_ptr.*)) return false;
    try entries.append(allocator, .{
      .section = section,
      .name = entry.key_ptr.*,
      .version = entry.value_ptr.*,
    });
  }
  return true;
}

fn resolutionCacheKey(
  allocator: std.mem.Allocator,
  pkg_json: *const json.PackageJson,
  registry: []const u8,
) ?u64 {
  if (pkg_json.peer_dependencies.count() > 0) return null;

  var entries = std.ArrayListUnmanaged(ResolutionCacheEntry).empty;
  defer entries.deinit(allocator);

  if (!(appendResolutionCacheEntries(allocator, &entries, 'd', &pkg_json.dependencies) catch return null)) return null;
  if (!(appendResolutionCacheEntries(allocator, &entries, 'D', &pkg_json.dev_dependencies) catch return null)) return null;
  if (!(appendResolutionCacheEntries(allocator, &entries, 'o', &pkg_json.optional_dependencies) catch return null)) return null;
  if (entries.items.len == 0) return null;

  std.sort.heap(ResolutionCacheEntry, entries.items, {}, resolutionCacheEntryLessThan);

  var hasher = std.hash.Wyhash.init(0);
  hasher.update("resolution-lock-v3");
  hasher.update(&[_]u8{0});
  hasher.update(registry);
  hasher.update(&[_]u8{0});
  hasher.update(@tagName(builtin.os.tag));
  hasher.update(&[_]u8{0});
  hasher.update(@tagName(builtin.cpu.arch));
  hasher.update(&[_]u8{0});
  hasher.update(platform.abiName());

  for (entries.items) |entry| {
    hasher.update(&[_]u8{0, entry.section, 0});
    hasher.update(entry.name);
    hasher.update(&[_]u8{0});
    hasher.update(entry.version);
  }

  return hasher.final();
}

fn resolutionCachePathForDir(cache_dir: []const u8, allocator: std.mem.Allocator, key: u64) ![]const u8 {
  return std.fmt.allocPrint(allocator, "{s}/resolution-lock/{x}.lockb", .{ cache_dir, key });
}

fn resolutionCachePath(c: *PkgContext, allocator: std.mem.Allocator, key: u64) ![]const u8 {
  return resolutionCachePathForDir(c.cache_dir, allocator, key);
}

fn lockfilePathForPackageJson(allocator: std.mem.Allocator, package_json_path: []const u8) ![]const u8 {
  if (std.fs.path.dirname(package_json_path)) |dir| {
    return std.fs.path.join(allocator, &.{ dir, "ant.lockb" });
  }
  return allocator.dupe(u8, "ant.lockb");
}

fn lockfileMatchesResolutionHash(lockfile_path: []const u8, resolution_hash: u64) bool {
  if (resolution_hash == 0) return false;
  var lf = lockfile.Lockfile.open(lockfile_path) catch return false;
  defer lf.close();
  return lf.header.hasInstallMetadata() and
    lf.header.matchesCurrentPlatform() and
    lf.header.resolution_hash == resolution_hash;
}

fn timestampWithinTtl(cached_at: i64, now: i64, ttl_seconds: i64, future_skew_seconds: i64) bool {
  const oldest = if (now > std.math.minInt(i64) + ttl_seconds)
    now - ttl_seconds
  else
    std.math.minInt(i64);
  const newest = if (now < std.math.maxInt(i64) - future_skew_seconds)
    now + future_skew_seconds
  else
    std.math.maxInt(i64);
  return cached_at >= oldest and cached_at <= newest;
}

fn cachedResolutionLockfileAvailable(
  allocator: std.mem.Allocator,
  cache_dir: []const u8,
  pkg_json: *const json.PackageJson,
  registry: []const u8,
) bool {
  const key = resolutionCacheKey(allocator, pkg_json, registry) orelse return false;
  const path = resolutionCachePathForDir(cache_dir, allocator, key) catch return false;
  defer allocator.free(path);

  const data = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(64 * 1024 * 1024)) catch return false;
  defer allocator.free(data);
  if (!cachedResolutionLockfileDataValid(data, key)) return false;

  var cached_at: i64 = undefined;
  @memcpy(std.mem.asBytes(&cached_at), data[0..@sizeOf(i64)]);
  const now = std.Io.Timestamp.now(io, .real).toSeconds();
  return timestampWithinTtl(cached_at, now, 24 * 60 * 60, 60);
}

fn cachedResolutionLockfileDataValid(data: []const u8, expected_resolution_hash: u64) bool {
  if (data.len < @sizeOf(i64) + @sizeOf(lockfile.Header)) return false;

  var header: lockfile.Header = undefined;
  const body = data[@sizeOf(i64)..];
  @memcpy(std.mem.asBytes(&header), body[0..@sizeOf(lockfile.Header)]);
  if (!header.validate() or !header.hasInstallMetadata() or !header.matchesCurrentPlatform()) return false;
  if (header.resolution_hash == 0 or header.resolution_hash != expected_resolution_hash) return false;

  const sections = struct {
    fn end(body_len: usize, offset: u32, count: u32, elem_size: usize) ?usize {
      const start: usize = @intCast(offset);
      const n: usize = @intCast(count);
      const bytes = std.math.mul(usize, n, elem_size) catch return null;
      if (start > body_len or bytes > body_len - start) return null;
      return start + bytes;
    }

    fn fits(body_len: usize, offset: u32, count: u32, elem_size: usize) bool {
      return end(body_len, offset, count, elem_size) != null;
    }

    fn alignUp(value: usize, alignment: usize) usize {
      const rem = value % alignment;
      return if (rem == 0) value else value + (alignment - rem);
    }
  };

  if (!sections.fits(body.len, header.string_table_offset, header.string_table_size, 1)) return false;
  if (!sections.fits(body.len, header.package_array_offset, header.package_count, @sizeOf(lockfile.Package))) return false;
  if (!sections.fits(body.len, header.dependency_array_offset, header.dependency_count, @sizeOf(lockfile.Dependency))) return false;
  const hash_end = sections.end(body.len, header.hash_table_offset, header.hash_table_size, @sizeOf(lockfile.HashBucket)) orelse return false;

  const bin_end = if (header.bin_entry_count > 0) blk: {
    const bin_offset: usize = @intCast(header.bin_entry_offset);
    if (bin_offset < hash_end) return false;
    break :blk sections.end(body.len, header.bin_entry_offset, header.bin_entry_count, @sizeOf(lockfile.BinEntry)) orelse return false;
  } else if (header.bin_entry_offset != 0) blk: {
    const offset: usize = @intCast(header.bin_entry_offset);
    if (offset < hash_end or offset > body.len) return false;
    break :blk offset;
  } else hash_end;

  const disabled_start = sections.alignUp(bin_end, @alignOf(lockfile.DisabledDependency));
  if (disabled_start > body.len) return false;
  const disabled_count: usize = @intCast(header.disabled_dependency_count);
  const disabled_bytes = std.math.mul(usize, disabled_count, @sizeOf(lockfile.DisabledDependency)) catch return false;
  if (disabled_bytes > body.len - disabled_start) return false;
  const disabled_end = disabled_start + disabled_bytes;

  if (header.platform_entry_count > 0) {
    const platform_start = if (header.platform_entry_offset != 0)
      @as(usize, @intCast(header.platform_entry_offset))
    else
      sections.alignUp(disabled_end, @alignOf(lockfile.PlatformEntry));
    const min_platform_start = sections.alignUp(disabled_end, @alignOf(lockfile.PlatformEntry));
    if (platform_start < min_platform_start or platform_start > body.len) return false;
    const platform_count: usize = @intCast(header.platform_entry_count);
    const platform_bytes = std.math.mul(usize, platform_count, @sizeOf(lockfile.PlatformEntry)) catch return false;
    if (platform_bytes > body.len - platform_start) return false;
  }

  return true;
}

fn readCachedResolutionLockfile(
  c: *PkgContext,
  allocator: std.mem.Allocator,
  pkg_json: *const json.PackageJson,
  registry: []const u8,
  lockfile_path: []const u8,
) bool {
  const key = resolutionCacheKey(allocator, pkg_json, registry) orelse return false;
  const path = resolutionCachePath(c, allocator, key) catch return false;
  defer allocator.free(path);

  const data = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(64 * 1024 * 1024)) catch return false;
  defer allocator.free(data);
  if (!cachedResolutionLockfileDataValid(data, key)) return false;

  var cached_at: i64 = undefined;
  @memcpy(std.mem.asBytes(&cached_at), data[0..@sizeOf(i64)]);
  const now = std.Io.Timestamp.now(io, .real).toSeconds();
  if (!timestampWithinTtl(cached_at, now, 24 * 60 * 60, 60)) return false;

  std.Io.Dir.cwd().writeFile(io, .{
    .sub_path = lockfile_path,
    .data = data[@sizeOf(i64)..],
  }) catch return false;
  return true;
}

fn storeCachedResolutionLockfile(
  c: *PkgContext,
  allocator: std.mem.Allocator,
  pkg_json: *const json.PackageJson,
  registry: []const u8,
  lockfile_path: []const u8,
) void {
  const key = resolutionCacheKey(allocator, pkg_json, registry) orelse return;
  const path = resolutionCachePath(c, allocator, key) catch return;
  defer allocator.free(path);
  if (std.fs.path.dirname(path)) |dir| {
    std.Io.Dir.cwd().createDirPath(io, dir) catch return;
  }

  const lock_data = std.Io.Dir.cwd().readFileAlloc(io, lockfile_path, allocator, .limited(64 * 1024 * 1024)) catch return;
  defer allocator.free(lock_data);

  const value = allocator.alloc(u8, @sizeOf(i64) + lock_data.len) catch return;
  defer allocator.free(value);
  const now: i64 = std.Io.Timestamp.now(io, .real).toSeconds();
  @memcpy(value[0..@sizeOf(i64)], std.mem.asBytes(&now));
  @memcpy(value[@sizeOf(i64)..], lock_data);

  std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = value }) catch {};
}

fn installStateMarkerPath(allocator: std.mem.Allocator, node_modules_path: []const u8) ![]const u8 {
  return std.fmt.allocPrint(allocator, "{s}/.ant/install-state", .{node_modules_path});
}

fn installStateMarkerContent(allocator: std.mem.Allocator, graph_hash: u64) ![]const u8 {
  return std.fmt.allocPrint(allocator,
    "ant-install-state-v1\n{x}\n{s}\n{s}\n{s}\n",
    .{ graph_hash, @tagName(builtin.os.tag), @tagName(builtin.cpu.arch), platform.abiName() },
  );
}

fn installStateMarkerMatches(allocator: std.mem.Allocator, node_modules_path: []const u8, graph_hash: u64) bool {
  if (graph_hash == 0) return false;
  const marker_path = installStateMarkerPath(allocator, node_modules_path) catch return false;
  defer allocator.free(marker_path);
  const expected = installStateMarkerContent(allocator, graph_hash) catch return false;
  defer allocator.free(expected);
  const actual = std.Io.Dir.cwd().readFileAlloc(io, marker_path, allocator, .limited(1024)) catch return false;
  defer allocator.free(actual);
  return std.mem.eql(u8, actual, expected);
}

fn removeInstallStateMarker(allocator: std.mem.Allocator, node_modules_path: []const u8) void {
  const marker_path = installStateMarkerPath(allocator, node_modules_path) catch return;
  defer allocator.free(marker_path);
  std.Io.Dir.cwd().deleteFile(io, marker_path) catch {};
}

fn writeInstallStateMarker(allocator: std.mem.Allocator, node_modules_path: []const u8, graph_hash: u64) void {
  if (graph_hash == 0) return;
  const marker_path = installStateMarkerPath(allocator, node_modules_path) catch return;
  defer allocator.free(marker_path);
  if (std.fs.path.dirname(marker_path)) |dir| {
    std.Io.Dir.cwd().createDirPath(io, dir) catch return;
  }
  const content = installStateMarkerContent(allocator, graph_hash) catch return;
  defer allocator.free(content);
  std.Io.Dir.cwd().writeFile(io, .{ .sub_path = marker_path, .data = content }) catch {};
}

fn batchHitLessThanParentFirst(lf: *const lockfile.Lockfile, a: cache.CacheDB.BatchHit, b: cache.CacheDB.BatchHit) bool {
  const pkg_a = &lf.packages[a.index];
  const pkg_b = &lf.packages[b.index];

  const parent_a = pkg_a.parent_path.slice(lf.string_table);
  const parent_b = pkg_b.parent_path.slice(lf.string_table);
  const depth_a = linkPathDepth(if (parent_a.len > 0) parent_a else null);
  const depth_b = linkPathDepth(if (parent_b.len > 0) parent_b else null);
  if (depth_a != depth_b) return depth_a < depth_b;

  const name_a = pkg_a.name.slice(lf.string_table);
  const name_b = pkg_b.name.slice(lf.string_table);
  const parent_order = std.mem.order(u8, parent_a, parent_b);
  if (parent_order != .eq) return parent_order == .lt;
  return std.mem.order(u8, name_a, name_b) == .lt;
}

export fn pkg_init(options: *const PkgOptions) ?*PkgContext {
  return PkgContext.init(global_allocator, options.*) catch null;
}

export fn pkg_set_trace_enabled(enabled: bool) void {
  debug.enabled = enabled;
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
    if (err == error.InvalidLockfile) {
      c.clearError();
      return pkg_resolve_and_install(ctx, package_json_path, lockfile_path, node_modules_path);
    }
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
    if (!runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc)) return .io_error;
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

export fn pkg_get_latest_available_version(
  ctx: ?*PkgContext,
  package_name: [*:0]const u8,
  installed_version: [*:0]const u8,
  out_version: [*]u8,
  out_version_len: usize,
) c_int {
  const c = ctx orelse return 0;
  if (out_version_len == 0) return 0;
  out_version[0] = 0;

  const name = std.mem.span(package_name);
  const installed_str = std.mem.span(installed_version);
  const installed = resolver.Version.parse(installed_str) catch return 0;
  const meta = c.metadata_cache.get(name) orelse return 0;

  var best: ?*const resolver.VersionInfo = null;
  for (meta.versions.items) |*v| {
    if (v.version.prerelease != null) continue;
    if (!v.matchesPlatform()) continue;
    if (best == null or v.version.order(best.?.version) == .gt) best = v;
  }
  if (best == null) {
    for (meta.versions.items) |*v| {
      if (!v.matchesPlatform()) continue;
      if (best == null or v.version.order(best.?.version) == .gt) best = v;
    }
  }

  const latest = best orelse return 0;
  if (latest.version.order(installed) != .gt) return 0;

  if (latest.version_str.len + 1 > out_version_len) return 0;
  @memcpy(out_version[0..latest.version_str.len], latest.version_str);
  out_version[latest.version_str.len] = 0;
  return 1;
}

export fn pkg_count_installed(node_modules_path: [*:0]const u8) u32 {
  const nm_path = std.mem.span(node_modules_path);
  if (!std.mem.endsWith(u8, nm_path, "node_modules")) return 0;
  
  const base = nm_path[0 .. nm_path.len - "node_modules".len];
  var buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const lp = std.fmt.bufPrint(&buf, "{s}ant.lockb", .{base}) catch return 0;
  
  var lf = lockfile.Lockfile.open(lp) catch return 0;
  defer lf.close();
  return lf.header.package_count;
}

export fn pkg_discover_lifecycle_scripts(
  ctx: ?*PkgContext,
  node_modules_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  c.clearLifecycleScripts();

  const nm_path = std.mem.span(node_modules_path);
  var nm_dir = std.Io.Dir.cwd().openDir(io, nm_path, .{ .iterate = true }) catch return .io_error;
  defer nm_dir.close(io);

  var iter = nm_dir.iterate();
  while (iter.next(io) catch null) |entry| {
    if (entry.kind != .directory) continue;
    if (entry.name[0] == '@') {
      var scope_dir = nm_dir.openDir(io, entry.name, .{ .iterate = true }) catch continue;
      defer scope_dir.close(io);
      var scope_iter = scope_dir.iterate();
      while (scope_iter.next(io) catch null) |scoped_entry| {
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

fn discoverPackageScript(ctx: *PkgContext, nm_path: []const u8, pkg_name: []const u8, parent_dir: std.Io.Dir, dir_name: []const u8) void {
  var pkg_dir = parent_dir.openDir(io, dir_name, .{}) catch return;
  defer pkg_dir.close(io);

  const content = pkg_dir.readFileAlloc(io, "package.json", ctx.allocator, .limited(1024 * 1024)) catch return;
  defer ctx.allocator.free(content);

  var doc = json.JsonDoc.parse(content) catch return;
  defer doc.deinit();

  const root = doc.root();
  if (root.getObject("scripts")) |scripts| {
    const install_script = scripts.getString("install");
    const postinstall_script = scripts.getString("postinstall");
    if (install_script == null and postinstall_script == null) return;

    if (std.mem.eql(u8, pkg_name, "esbuild")) return;

    var commands = std.ArrayListUnmanaged(LifecycleCommand).empty;
    defer freeLifecycleCommands(ctx.allocator, &commands);
    if (install_script) |script| {
      appendLifecycleCommand(ctx.allocator, &commands, "install", script) catch return;
    }
    if (postinstall_script) |script| {
      appendLifecycleCommand(ctx.allocator, &commands, "postinstall", script) catch return;
    }

    const marker_path = std.fmt.allocPrint(ctx.allocator, "{s}/{s}/.postinstall", .{ nm_path, pkg_name }) catch return;
    defer ctx.allocator.free(marker_path);
    if (lifecycleMarkerMatches(ctx.allocator, marker_path, commands.items)) return;

    if (install_script) |install| {
      if (postinstall_script) |postinstall| {
        const combined = std.fmt.allocPrint(ctx.allocator, "{s} && {s}", .{ install, postinstall }) catch return;
        defer ctx.allocator.free(combined);
        ctx.addLifecycleScript(pkg_name, combined) catch return;
        return;
      }
      ctx.addLifecycleScript(pkg_name, install) catch return;
      return;
    }
    ctx.addLifecycleScript(pkg_name, postinstall_script.?) catch return;
  }
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

  if (!runTrustedPostinstall(c, &trusted, std.mem.span(node_modules_path), arena_alloc)) return .io_error;
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

  const content = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(10 * 1024 * 1024)) catch |err| {
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

  var string_copies = std.ArrayListUnmanaged([:0]u8).empty;
  defer {
    for (string_copies.items) |s| allocator.free(s);
    string_copies.deinit(allocator);
  }

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
          exists = true; break;
        }
      }
    }

    if (!exists) {
      const name_copy = allocator.dupeZ(u8, pkg_name) catch continue;
      string_copies.append(allocator, name_copy) catch {
        allocator.free(name_copy);
        continue;
      };
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

const InterleavedExtractCtx = struct {
  ext: *extractor.Extractor,
  integrity: [64]u8,
  cache_path: []const u8,
  staging_path: ?[]const u8,
  pkg_name: []const u8,
  version_str: []const u8,
  direct: bool,
  parent_path: ?[]const u8,
  has_bin: bool,
  bins: []const linker.PackageBin,
  completed: bool,
  has_error: bool,
  queued: bool,
  parent: *InterleavedContext,
};

fn populateResolvedFileCountsFromCache(res: *resolver.Resolver, db: *cache.CacheDB) void {
  var iter = res.resolved.valueIterator();
  while (iter.next()) |pkg_ptr| {
    const pkg = pkg_ptr.*;
    if (db.lookup(&pkg.integrity)) |cache_entry| {
      var entry = cache_entry;
      defer entry.deinit();
      pkg.file_count = entry.file_count;
    }
  }
}

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
      .extract_contexts = .empty,
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

  fn onPackageResolved(pkg: *resolver.ResolvedPackage, user_data: ?*anyopaque) void {
    const self: *InterleavedContext = @ptrCast(@alignCast(user_data));
    self.callbacks_received += 1;

    const pkg_name = pkg.name.slice();
    const current: u32 = @intCast(self.callbacks_received);
    const msg = std.fmt.allocPrintSentinel(self.arena_alloc, "{s}", .{pkg_name}, 0) catch return;
    self.pkg_ctx.reportProgress(.resolving, current, current, msg);

    if (self.queued_integrities.contains(pkg.integrity)) {
      self.integrity_duplicates += 1; return;
    } self.queued_integrities.put(pkg.integrity, {}) catch return;

    if (!self.pkg_ctx.options.force and self.db.hasIntegrity(&pkg.integrity)) {
      self.cache_hits += 1; return;
    } self.tarballs_queued += 1;

    const cache_path = self.db.getPackagePath(&pkg.integrity, self.arena_alloc) catch return;
    const version_str = pkg.version.format(self.arena_alloc) catch return;

    var staging_path: ?[]const u8 = null;
    if (self.pkg_ctx.options.force) {
      staging_path = std.fmt.allocPrint(self.arena_alloc, "{s}.force-tmp", .{cache_path}) catch return;
      std.Io.Dir.cwd().deleteTree(io, staging_path.?) catch {};
    }

    const ext = extractor.Extractor.init(self.allocator, staging_path orelse cache_path) catch return;
    const ctx = self.arena_alloc.create(InterleavedExtractCtx) catch {
      ext.deinit(); return;
    };

    ctx.* = .{
      .ext = ext,
      .integrity = pkg.integrity,
      .cache_path = cache_path,
      .staging_path = staging_path,
      .pkg_name = pkg.name.slice(),
      .version_str = version_str,
      .direct = pkg.direct,
      .parent_path = pkg.parent_path,
      .has_bin = pkg.has_bin,
      .bins = resolvedPackageBins(self.arena_alloc, pkg) catch &[_]linker.PackageBin{},
      .completed = false,
      .has_error = false,
      .queued = false,
      .parent = self,
    };

    self.http.fetchTarball(pkg.tarball_url, fetcher.StreamHandler.init(
      struct {
        fn onData(data: []const u8, ud: ?*anyopaque) void {
          const c: *InterleavedExtractCtx = @ptrCast(@alignCast(ud));
          if (c.has_error) return;
          c.ext.feedCompressed(data) catch { c.has_error = true; };
        }
      }.onData,
      struct {
        fn onComplete(status: u16, ud: ?*anyopaque) void {
          const c: *InterleavedExtractCtx = @ptrCast(@alignCast(ud));
          if (status != 200) c.has_error = true;
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

fn installToolPackageNoLifecycle(
  c: *PkgContext,
  allocator: std.mem.Allocator,
  package_json_path: []const u8,
  lockfile_path: []const u8,
  node_modules_path: []const u8,
) !void {
  const http = c.http orelse return error.NetworkError;
  const db = c.cache_db orelse return error.CacheError;
  http.resetMetaClients();

  var interleaved = InterleavedContext.init(c.allocator, allocator, db, http, c);
  defer interleaved.deinit();

  var res = resolver.Resolver.init(
    allocator,
    c.allocator,
    &c.string_pool,
    http,
    db,
    if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org",
    &c.metadata_cache,
  );
  defer res.deinit();
  res.force_refresh = c.options.force;

  res.setOnPackageResolved(InterleavedContext.onPackageResolved, &interleaved);
  try res.resolveFromPackageJson(package_json_path);

  if (interleaved.tarballs_queued > 0) try http.run();

  var pkg_linker = linker.Linker.init(c.allocator);
  defer pkg_linker.deinit();
  try pkg_linker.setNodeModulesPath(node_modules_path);

  for (interleaved.extract_contexts.items) |ectx| {
    defer ectx.ext.deinit();
    if (ectx.has_error or !ectx.completed or !ectx.ext.isComplete()) continue;

    const stats = ectx.ext.stats();
    db.insert(&.{
      .integrity = ectx.integrity,
      .path = ectx.cache_path,
      .unpacked_size = stats.bytes,
      .file_count = stats.files,
      .cached_at = std.Io.Timestamp.now(io, .real).toSeconds(),
    }, ectx.pkg_name, ectx.version_str) catch continue;

    pkg_linker.linkPackage(.{
      .cache_path = ectx.cache_path,
      .node_modules_path = node_modules_path,
      .name = ectx.pkg_name,
      .parent_path = ectx.parent_path,
      .file_count = stats.files,
      .has_bin = ectx.has_bin,
      .bins = ectx.bins,
    }) catch |err| {
      debug.log("  link error: {s}: {s}", .{ ectx.pkg_name, @errorName(err) });
      return err;
    };
  }

  var resolved_iter = res.resolved.valueIterator();
  while (resolved_iter.next()) |pkg_ptr| {
    const pkg = pkg_ptr.*;
    if (db.lookup(&pkg.integrity)) |cache_entry| {
      var entry = cache_entry;
      defer entry.deinit();
      const pkg_cache_path = allocator.dupe(u8, entry.path) catch continue;
      pkg_linker.linkPackage(.{
        .cache_path = pkg_cache_path,
        .node_modules_path = node_modules_path,
        .name = pkg.name.slice(),
        .parent_path = pkg.parent_path,
        .file_count = entry.file_count,
        .has_bin = pkg.has_bin,
        .bins = resolvedPackageBins(allocator, pkg) catch &[_]linker.PackageBin{},
      }) catch continue;
    }
  }

  populateResolvedFileCountsFromCache(&res, db);
  _ = res.writeLockfile(lockfile_path) catch {};
  db.sync();
}

export fn pkg_resolve_and_install(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  lockfile_path: [*:0]const u8,
  node_modules_path: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const timer = std.Io.Clock.Timestamp.now(io, .boot);
  var stage_start: u64 = @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds());
  var trace = debug.StageTrace{};

  debug.log("resolve+install (interleaved): package_json={s} lockfile={s} node_modules={s}", .{
    std.mem.span(package_json_path),
    std.mem.span(lockfile_path),
    std.mem.span(node_modules_path),
  });

  const http = c.http orelse return .network_error;
  http.resetMetaClients();
  const db = c.cache_db orelse return .cache_error;

  const pkg_json_path_z = arena_alloc.dupeZ(u8, std.mem.span(package_json_path)) catch return .out_of_memory;
  var pkg_json = json.PackageJson.parse(c.allocator, pkg_json_path_z) catch {
    c.setError("Failed to parse package.json");
    return .io_error;
  };  defer pkg_json.deinit(c.allocator);
  stage_start = trace.mark("parse package.json", stage_start);

  if (pkg_json.trusted_dependencies.count() > 0) {
    debug.log("  trusted dependencies: {d}", .{pkg_json.trusted_dependencies.count()});
  }
  const registry = if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org";
  const resolution_hash = resolutionCacheKey(arena_alloc, &pkg_json, registry) orelse 0;

  if (lockfileMatchesResolutionHash(std.mem.span(lockfile_path), resolution_hash)) {
    c.install(std.mem.span(lockfile_path), std.mem.span(node_modules_path)) catch |err| {
      debug.trace("clean lockfile install failed: {s}", .{@errorName(err)});
    };
    if (c.last_install_result.package_count > 0) {
      _ = trace.mark("clean lockfile install", stage_start);
      trace.summary("resolve+install");
      debug.trace("resolve+install total: source=clean-lockfile packages={d} cached={d} fetched={d} files_linked={d} files_copied={d} total={d}ms", .{
        c.last_install_result.package_count,
        c.last_install_result.cache_hits,
        c.last_install_result.cache_misses,
        c.last_install_result.files_linked,
        c.last_install_result.files_copied,
        c.last_install_result.elapsed_ms,
      });
      if (pkg_json.trusted_dependencies.count() > 0) {
        if (!runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc)) return .io_error;
      }
      return .ok;
    }
  }

  if (readCachedResolutionLockfile(c, arena_alloc, &pkg_json, registry, std.mem.span(lockfile_path))) {
    stage_start = trace.mark("resolution lock cache hit", stage_start);
    c.install(std.mem.span(lockfile_path), std.mem.span(node_modules_path)) catch |err| {
      debug.trace("resolution lock cache install failed: {s}", .{@errorName(err)});
    };
    if (c.last_install_result.package_count > 0) {
      _ = trace.mark("resolution lock cache install", stage_start);
      trace.summary("resolve+install");
      debug.trace("resolve+install total: source=resolution-cache packages={d} cached={d} fetched={d} files_linked={d} files_copied={d} total={d}ms", .{
        c.last_install_result.package_count,
        c.last_install_result.cache_hits,
        c.last_install_result.cache_misses,
        c.last_install_result.files_linked,
        c.last_install_result.files_copied,
        c.last_install_result.elapsed_ms,
      });
      if (pkg_json.trusted_dependencies.count() > 0) {
        if (!runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc)) return .io_error;
      }
      return .ok;
    }
  }
  stage_start = trace.mark("resolution lock cache lookup", stage_start);
  removeInstallStateMarker(arena_alloc, std.mem.span(node_modules_path));

  var interleaved = InterleavedContext.init(c.allocator, arena_alloc, db, http, c);
  defer interleaved.deinit();

  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    db,
    registry,
    &c.metadata_cache,
  ); defer res.deinit();
  res.force_refresh = c.options.force;
  res.setLockResolutionHash(resolution_hash);

  res.setOnPackageResolved(InterleavedContext.onPackageResolved, &interleaved);
  res.resolveFromPackageJson(std.mem.span(package_json_path)) catch |err| {
    setResolveError(c, null, err);
    return .resolve_error;
  };
  
  stage_start = trace.mark("resolve + queue tarballs", stage_start);
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
      const version_str = pkg.version.format(arena_alloc) catch continue;
      c.addPackageToResults(pkg.name.slice(), version_str, true) catch {};
    }
  }

  var pkg_linker = linker.Linker.init(c.allocator);
  defer pkg_linker.deinit();
  pkg_linker.setNodeModulesPath(std.mem.span(node_modules_path)) catch return .io_error;

  var cache_hit_jobs = std.ArrayListUnmanaged(linker.PackageLink).empty;
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

    var cache_entry = db.lookup(&pkg.integrity) orelse continue;
    defer cache_entry.deinit();
    const cache_path = arena_alloc.dupe(u8, cache_entry.path) catch continue;
    
    cache_hit_jobs.append(arena_alloc, .{
      .cache_path = cache_path,
      .node_modules_path = std.mem.span(node_modules_path),
      .name = pkg.name.slice(),
      .parent_path = pkg.parent_path,
      .file_count = cache_entry.file_count,
      .has_bin = pkg.has_bin,
      .bins = resolvedPackageBins(arena_alloc, pkg) catch &[_]linker.PackageBin{},
      .allow_dir_symlink = (!pkg.has_bin and pkg.dependencies.items.len == 0 and !resolvedPackageHasNestedChildren(&res, pkg, arena_alloc)) or
        (pkg.has_bin and cache_entry.file_count >= linker.DIR_CLONE_THRESHOLD and pkg.parent_path == null),
    }) catch continue;
  }
  stage_start = trace.mark("plan cache-hit links", stage_start);

  var tarball_thread: ?std.Thread = null;
  if (interleaved.tarballs_queued > 0) {
    debug.log("finishing {d} tarball downloads (pending={d})...", .{
      interleaved.tarballs_queued,
      http.pending.items.len,
    });
    tarball_thread = std.Thread.spawn(.{}, struct {
      fn work(h: *fetcher.Fetcher) void { h.run() catch {}; }
    }.work, .{http}) catch |err| blk: {
      debug.log("warning: failed to spawn tarball thread: {}, running synchronously", .{err});
      http.run() catch {};
      break :blk null;
    };
  }

  std.sort.heap(linker.PackageLink, cache_hit_jobs.items, {}, packageLinkLessThanParentFirst);

  var linked_count: usize = 0;
  var link_error_count: usize = 0;
  for (cache_hit_jobs.items, 0..) |job, i| {
    const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{job.name}, 0) catch continue;
    c.reportProgress(.linking, @intCast(i), @intCast(cache_hit_jobs.items.len), msg);
    pkg_linker.linkPackage(job) catch |err| {
      link_error_count += 1;
      debug.log("  link error: {s}: {s}", .{ job.name, @errorName(err) });
      continue;
    };
    linked_count += 1;
  }

  if (tarball_thread) |t| {
    t.join();
    stage_start = trace.mark("finish tarballs + link cache hits", stage_start);
  } else stage_start = trace.mark("link cache hits", stage_start);
  debug.log("  linked {d} from cache", .{linked_count});

  var success_count: usize = 0;
  var error_count: usize = 0;

  const LinkJobWithSize = struct {
    job: linker.PackageLink,
    size: u64,
  };

  var cache_entries = std.ArrayListUnmanaged(cache.CacheDB.NamedCacheEntry).empty;
  var link_jobs = std.ArrayListUnmanaged(LinkJobWithSize).empty;
  const current_time = std.Io.Timestamp.now(io, .real).toSeconds();
  const nm_path = std.mem.span(node_modules_path);

  for (interleaved.extract_contexts.items) |ext_ctx| {
    if (ext_ctx.has_error or !ext_ctx.completed or !ext_ctx.ext.isComplete()) {
      error_count += 1;
      if (ext_ctx.staging_path) |staging| std.Io.Dir.cwd().deleteTree(io, staging) catch {};
      debug.log("  error: {s}", .{ext_ctx.pkg_name}); continue;
    }

    if (ext_ctx.staging_path) |staging| {
      const from_z = arena_alloc.dupeZ(u8, staging) catch { error_count += 1; std.Io.Dir.cwd().deleteTree(io, staging) catch {}; continue; };
      const to_z = arena_alloc.dupeZ(u8, ext_ctx.cache_path) catch { error_count += 1; continue; };
      std.Io.Dir.cwd().deleteTree(io, ext_ctx.cache_path) catch {};
      if (std.c.rename(from_z.ptr, to_z.ptr) != 0) {
        interleaved.db.delete(&ext_ctx.integrity) catch {};
        std.Io.Dir.cwd().deleteTree(io, staging) catch {};
        error_count += 1;
        debug.log("  error: {s} (force swap failed)", .{ext_ctx.pkg_name}); continue;
      }
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
        .has_bin = ext_ctx.has_bin,
        .bins = ext_ctx.bins,
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
  stage_start = trace.mark("cache insert (batch)", stage_start);

  const total_jobs: u32 = @intCast(link_jobs.items.len);
  var link_counter = std.atomic.Value(u32).init(0);
  std.sort.heap(LinkJobWithSize, link_jobs.items, {}, struct {
    fn lessThan(_: void, a: LinkJobWithSize, b: LinkJobWithSize) bool {
      const depth_a = linkPathDepth(a.job.parent_path);
      const depth_b = linkPathDepth(b.job.parent_path);
      if (depth_a != depth_b) return depth_a < depth_b;
      if (a.size != b.size) return a.size > b.size;

      const parent_a = a.job.parent_path orelse "";
      const parent_b = b.job.parent_path orelse "";
      const parent_order = std.mem.order(u8, parent_a, parent_b);
      if (parent_order != .eq) return parent_order == .lt;
      return std.mem.order(u8, a.job.name, b.job.name) == .lt;
    }
  }.lessThan);

  var slow_link_count = std.atomic.Value(u32).init(0);
  var download_link_error_count = std.atomic.Value(u32).init(0);
  var max_link_ms = std.atomic.Value(u64).init(0);
  var slow_link_names = std.ArrayListUnmanaged([]const u8).empty;
  defer slow_link_names.deinit(c.allocator);
  var slow_link_lock = std.Io.Mutex.init;

  var copy_link_mode = false;
  for (link_jobs.items) |job_with_size| {
    if (linker.Linker.pathsAreCrossDevice(job_with_size.job.cache_path, std.mem.span(node_modules_path))) {
      copy_link_mode = true;
      pkg_linker.enableCopyMode();
      break;
    }
  }
  if (copy_link_mode and c.options.verbose) {
    debug.log("  linker: cache and node_modules are on different devices; using copy mode", .{});
  }

  var depth_start: usize = 0;
  while (depth_start < link_jobs.items.len) {
    const depth = linkPathDepth(link_jobs.items[depth_start].job.parent_path);
    var depth_end = depth_start + 1;
    while (depth_end < link_jobs.items.len and
      linkPathDepth(link_jobs.items[depth_end].job.parent_path) == depth) : (depth_end += 1) {}

    const depth_jobs = link_jobs.items[depth_start..depth_end];
    const max_threads: usize = if (copy_link_mode) 2 else 8;
    const num_threads = @min(max_threads, depth_jobs.len);
    var depth_has_root_bins = false;
    for (depth_jobs) |job_with_size| {
      if (job_with_size.job.parent_path == null and job_with_size.job.has_bin) {
        depth_has_root_bins = true;
        break;
      }
    }
    if (c.options.verbose and depth_jobs.len > 1) {
      debug.log("  linking depth {d} ({d} items)", .{ depth, depth_jobs.len });
    }

    if (!depth_has_root_bins and num_threads > 1 and depth_jobs.len > 4) {
      var threads: [8]?std.Thread = .{null} ** 8;
      var next_job = std.atomic.Value(usize).init(0);
      var spawned_count: usize = 0;

      for (0..num_threads) |t| {
        threads[t] = std.Thread.spawn(.{}, struct {
          fn work(lnk: *linker.Linker, jobs: []const LinkJobWithSize, next: *std.atomic.Value(usize), pkg_ctx: *PkgContext, total: u32, counter: *std.atomic.Value(u32), link_errors: *std.atomic.Value(u32), slow_count: *std.atomic.Value(u32), max_ms: *std.atomic.Value(u64), names: *std.ArrayListUnmanaged([]const u8), lock: *std.Io.Mutex, alloc: std.mem.Allocator) void {
            while (true) {
              const idx = next.fetchAdd(1, .monotonic);
              if (idx >= jobs.len) break;
              const job = jobs[idx].job;
              const current = counter.fetchAdd(1, .monotonic) + 1;
              var msg_buf: [256]u8 = undefined;
              const msg_len = std.fmt.bufPrint(&msg_buf, "{s}", .{job.name}) catch continue;
              msg_buf[msg_len.len] = 0;
              pkg_ctx.reportProgress(.linking, current, total, msg_buf[0..msg_len.len :0]);
              const start = std.Io.Timestamp.now(io, .boot).toNanoseconds();
              lnk.linkPackage(job) catch |err| {
                _ = link_errors.fetchAdd(1, .monotonic);
                debug.log("  link error: {s}: {s}", .{ job.name, @errorName(err) });
                continue;
              };
              const delta = std.Io.Timestamp.now(io, .boot).toNanoseconds() - start;
              const elapsed_ms: u64 = if (delta < 0) 0 else @intCast(@as(u128, @intCast(delta)) / 1_000_000);
              if (elapsed_ms > 100) {
                _ = slow_count.fetchAdd(1, .monotonic);
                lock.lockUncancelable(io);
                const entry = std.fmt.allocPrint(alloc, "{s} {d}ms", .{ job.name, elapsed_ms }) catch null;
                if (entry) |val| {
                  names.append(alloc, val) catch {};
                }
                lock.unlock(io);
                var current_max = max_ms.load(.monotonic);
                while (elapsed_ms > current_max) : (current_max = max_ms.load(.monotonic)) {
                  if (max_ms.cmpxchgWeak(current_max, elapsed_ms, .monotonic, .monotonic) == null) break;
                }
              }
            }
          }
        }.work, .{ &pkg_linker, depth_jobs, &next_job, c, total_jobs, &link_counter, &download_link_error_count, &slow_link_count, &max_link_ms, &slow_link_names, &slow_link_lock, c.allocator }) catch null;
        if (threads[t] != null) spawned_count += 1;
      }

      if (spawned_count == 0) {
        for (depth_jobs) |job_with_size| {
          const job = job_with_size.job;
          const current = link_counter.fetchAdd(1, .monotonic) + 1;
          const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{job.name}, 0) catch continue;
          c.reportProgress(.linking, current, total_jobs, msg);
          const start = std.Io.Timestamp.now(io, .boot).toNanoseconds();
          pkg_linker.linkPackage(job) catch |err| {
            _ = download_link_error_count.fetchAdd(1, .monotonic);
            debug.log("  link error: {s}: {s}", .{ job.name, @errorName(err) });
            continue;
          };
          const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds())) - @as(u64, @intCast(start))) / 1_000_000);
          if (elapsed_ms > 100 and c.options.verbose) {
            debug.log("  link slow: {s} {d}ms", .{ job.name, elapsed_ms });
          }
        }
      } else {
        for (&threads) |*t| {
          if (t.*) |thread| thread.join();
        }
      }
    } else {
      for (depth_jobs) |job_with_size| {
        const job = job_with_size.job;
        const current = link_counter.fetchAdd(1, .monotonic) + 1;
        const msg = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{job.name}, 0) catch continue;
        c.reportProgress(.linking, current, total_jobs, msg);
        const start = std.Io.Timestamp.now(io, .boot).toNanoseconds();
        pkg_linker.linkPackage(job) catch |err| {
          _ = download_link_error_count.fetchAdd(1, .monotonic);
          debug.log("  link error: {s}: {s}", .{ job.name, @errorName(err) });
          continue;
        };
        const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds())) - @as(u64, @intCast(start))) / 1_000_000);
        if (elapsed_ms > 100 and c.options.verbose) {
          debug.log("  link slow: {s} {d}ms", .{ job.name, elapsed_ms });
        }
      }
    }

    depth_start = depth_end;
  }

  if (c.options.verbose) {
    debug.log("  link slow (>100ms): {d} max={d}ms", .{ slow_link_count.load(.monotonic), max_link_ms.load(.monotonic) });
    for (slow_link_names.items) |entry| {
      debug.log("  link slow: {s}", .{entry});
    }
  }
  
  for (slow_link_names.items) |entry| c.allocator.free(entry);  
  stage_start = trace.mark("link downloads (parallel)", stage_start);
  
  debug.log("  downloaded: {d} success, {d} errors", .{ success_count, error_count });
  for (interleaved.extract_contexts.items) |ext_ctx| ext_ctx.ext.deinit();
  if (error_count > 0) {
    c.setErrorFmt("Failed to fetch or extract {d} package{s}", .{
      error_count,
      if (error_count == 1) "" else "s",
    });
    return .extract_error;
  }
  link_error_count += download_link_error_count.load(.monotonic);
  if (link_error_count > 0) {
    c.setErrorFmt("Failed to link {d} package{s}", .{
      link_error_count,
      if (link_error_count == 1) "" else "s",
    });
    return .io_error;
  }

  populateResolvedFileCountsFromCache(&res, db);
  const graph_hash = res.writeLockfile(std.mem.span(lockfile_path)) catch |err| {
    c.setErrorFmt("Failed to write lockfile: {}", .{err});
    return .io_error;
  };
  storeCachedResolutionLockfile(c, arena_alloc, &pkg_json, registry, std.mem.span(lockfile_path));
  stage_start = trace.mark("write lockfile", stage_start);

  db.sync();
  _ = trace.mark("cache sync", stage_start);
  writeInstallStateMarker(arena_alloc, std.mem.span(node_modules_path), graph_hash);

  const link_stats = pkg_linker.getStats();
  c.last_install_result = .{
    .package_count = @intCast(res.resolved.count()),
    .cache_hits = @intCast(interleaved.cache_hits),
    .cache_misses = @intCast(interleaved.tarballs_queued),
    .files_linked = link_stats.files_linked,
    .files_copied = link_stats.files_copied,
    .packages_installed = link_stats.packages_installed,
    .packages_skipped = link_stats.packages_skipped,
    .lifecycle_builds = 0,
    .elapsed_ms = @intCast(timer.untilNow(io).raw.toMilliseconds()),
  };

  trace.summary("resolve+install");
  debug.trace("resolve+install total: packages={d} installed={d} cached={d} fetched={d} files_linked={d} files_copied={d} total={d}ms", .{
    res.resolved.count(),
    c.last_install_result.packages_installed,
    interleaved.cache_hits,
    interleaved.tarballs_queued,
    c.last_install_result.files_linked,
    c.last_install_result.files_copied,
    c.last_install_result.elapsed_ms,
  });
  debug.log("total: {d} packages in {d}ms", .{ res.resolved.count(), c.last_install_result.elapsed_ms });

  if (pkg_json.trusted_dependencies.count() > 0) {
    if (!runTrustedPostinstall(c, &pkg_json.trusted_dependencies, std.mem.span(node_modules_path), arena_alloc)) return .io_error;
  }

  return .ok;
}

const PostinstallJob = struct {
  pkg_name: []const u8,
  pkg_dir: []const u8,
  commands: std.ArrayListUnmanaged(LifecycleCommand),
  failed: bool = false,
};

const LifecycleCommand = struct {
  name: []const u8,
  script: []const u8,
};

fn appendLifecycleCommand(
  allocator: std.mem.Allocator,
  commands: *std.ArrayListUnmanaged(LifecycleCommand),
  name: []const u8,
  script: []const u8,
) !void {
  try commands.append(allocator, .{
    .name = name,
    .script = try allocator.dupe(u8, script),
  });
}

fn freeLifecycleCommands(allocator: std.mem.Allocator, commands: *std.ArrayListUnmanaged(LifecycleCommand)) void {
  for (commands.items) |cmd| allocator.free(cmd.script);
  commands.deinit(allocator);
}

fn lifecycleMarkerContent(allocator: std.mem.Allocator, commands: []const LifecycleCommand) ![]u8 {
  var content = std.ArrayListUnmanaged(u8).empty;
  errdefer content.deinit(allocator);
  try content.appendSlice(allocator, "ant lifecycle v1\n");
  for (commands) |cmd| {
    try content.appendSlice(allocator, cmd.name);
    try content.append(allocator, '\n');
  }
  return content.toOwnedSlice(allocator);
}

fn lifecycleMarkerMatches(allocator: std.mem.Allocator, marker_path: []const u8, commands: []const LifecycleCommand) bool {
  const existing = std.Io.Dir.cwd().readFileAlloc(io, marker_path, allocator, .limited(64 * 1024)) catch return false;
  defer allocator.free(existing);

  const expected = lifecycleMarkerContent(allocator, commands) catch return false;
  defer allocator.free(expected);

  return std.mem.eql(u8, existing, expected);
}

fn writeLifecycleMarker(allocator: std.mem.Allocator, marker_path: []const u8, commands: []const LifecycleCommand) void {
  const content = lifecycleMarkerContent(allocator, commands) catch return;
  defer allocator.free(content);

  const file = std.Io.Dir.cwd().createFile(io, marker_path, .{}) catch return;
  defer file.close(io);
  file.writeStreamingAll(io, content) catch {};
}

fn pathDelimiter() u8 {
  return if (builtin.os.tag == .windows) ';' else ':';
}

fn findExecutableInPath(allocator: std.mem.Allocator, path_env: []const u8, name: []const u8) !?[]u8 {
  var path_iter = std.mem.splitScalar(u8, path_env, pathDelimiter());
  while (path_iter.next()) |dir| {
    if (dir.len == 0) continue;
    const candidate = try std.fs.path.join(allocator, &.{ dir, name });
    std.Io.Dir.cwd().access(io, candidate, .{}) catch {
      allocator.free(candidate);
      continue;
    };
    return candidate;
  }
  return null;
}

fn findNpmNodeGypInPrefix(allocator: std.mem.Allocator, prefix: []const u8) !?[]u8 {
  const candidate = try std.fs.path.join(allocator, &.{
    prefix,
    "lib",
    "node_modules",
    "npm",
    "node_modules",
    "node-gyp",
    "bin",
    "node-gyp.js",
  });
  std.Io.Dir.cwd().access(io, candidate, .{}) catch {
    allocator.free(candidate);
    return null;
  };
  return candidate;
}

fn findNpmNodeGypInNpmRoot(allocator: std.mem.Allocator, npm_root: []const u8) !?[]u8 {
  const candidate = try std.fs.path.join(allocator, &.{
    npm_root,
    "node_modules",
    "node-gyp",
    "bin",
    "node-gyp.js",
  });
  std.Io.Dir.cwd().access(io, candidate, .{}) catch {
    allocator.free(candidate);
    return null;
  };
  return candidate;
}

fn findNpmNodeGypNearExecutable(allocator: std.mem.Allocator, executable_path: []const u8) !?[]u8 {
  if (std.fs.path.dirname(executable_path)) |bin_dir| {
    if (std.fs.path.dirname(bin_dir)) |prefix| {
      if (try findNpmNodeGypInPrefix(allocator, prefix)) |node_gyp| return node_gyp;
    }

    if (std.fs.path.dirname(bin_dir)) |maybe_npm_root| {
      if (std.mem.eql(u8, std.fs.path.basename(bin_dir), "bin")) {
        if (try findNpmNodeGypInNpmRoot(allocator, maybe_npm_root)) |node_gyp| return node_gyp;
      }
    }
  }

  const real_path = std.Io.Dir.cwd().realPathFileAlloc(io, executable_path, allocator) catch return null;
  defer allocator.free(real_path);
  if (!std.mem.eql(u8, real_path, executable_path)) {
    if (try findNpmNodeGypNearExecutable(allocator, real_path)) |node_gyp| return node_gyp;
  }

  return null;
}

fn findNpmNodeGyp(allocator: std.mem.Allocator, env_map: *std.process.Environ.Map) !?[]u8 {
  const path_env = env_map.get("PATH") orelse return null;

  const node_name = if (builtin.os.tag == .windows) "node.exe" else "node";
  if (try findExecutableInPath(allocator, path_env, node_name)) |node_path| {
    defer allocator.free(node_path);
    if (try findNpmNodeGypNearExecutable(allocator, node_path)) |node_gyp| return node_gyp;
  }

  const npm_name = if (builtin.os.tag == .windows) "npm.cmd" else "npm";
  if (try findExecutableInPath(allocator, path_env, npm_name)) |npm_path| {
    defer allocator.free(npm_path);
    if (try findNpmNodeGypNearExecutable(allocator, npm_path)) |node_gyp| return node_gyp;
  }

  return null;
}

fn appendShellSingleQuoted(allocator: std.mem.Allocator, out: *std.ArrayListUnmanaged(u8), text: []const u8) !void {
  try out.append(allocator, '\'');
  for (text) |ch| {
    if (ch == '\'') {
      try out.appendSlice(allocator, "'\\''");
    } else {
      try out.append(allocator, ch);
    }
  }
  try out.append(allocator, '\'');
}

fn ensureLifecycleNodeGypShim(ctx: *PkgContext, allocator: std.mem.Allocator, env_map: *std.process.Environ.Map) !?[]u8 {
  const node_gyp_path = (try findNpmNodeGyp(allocator, env_map)) orelse return null;
  defer allocator.free(node_gyp_path);

  const bin_dir = try std.fs.path.join(allocator, &.{ ctx.cache_dir, "tools", "node-gyp", "npm-bundled", "bin" });
  errdefer allocator.free(bin_dir);
  try std.Io.Dir.cwd().createDirPath(io, bin_dir);

  const shim_name = if (builtin.os.tag == .windows) "node-gyp.cmd" else "node-gyp";
  const shim_path = try std.fs.path.join(allocator, &.{ bin_dir, shim_name });
  defer allocator.free(shim_path);

  var content = std.ArrayListUnmanaged(u8).empty;
  defer content.deinit(allocator);
  if (builtin.os.tag == .windows) {
    try content.appendSlice(allocator, "@echo off\r\nnode \"");
    try content.appendSlice(allocator, node_gyp_path);
    try content.appendSlice(allocator, "\" %*\r\n");
  } else {
    try content.appendSlice(allocator, "#!/bin/sh\nexec node ");
    try appendShellSingleQuoted(allocator, &content, node_gyp_path);
    try content.appendSlice(allocator, " \"$@\"\n");
  }

  const file = if (builtin.os.tag == .windows)
    try std.Io.Dir.cwd().createFile(io, shim_path, .{})
  else
    try std.Io.Dir.cwd().createFile(io, shim_path, .{ .permissions = .executable_file });
  defer file.close(io);
  try file.writeStreamingAll(io, content.items);

  debug.log("prepared lifecycle node-gyp shim: {s}", .{node_gyp_path});
  return bin_dir;
}

const ant_managed_node_gyp_version = "12.2.0";
const lifecycle_output_tail_limit = 1024 * 1024;

fn ensureAntManagedNodeGypBin(ctx: *PkgContext, allocator: std.mem.Allocator) !?[]u8 {
  const tool_root = try std.fs.path.join(allocator, &.{
    ctx.cache_dir,
    "tools",
    "node-gyp",
    ant_managed_node_gyp_version,
  });
  defer allocator.free(tool_root);

  const node_modules_path = try std.fs.path.join(allocator, &.{ tool_root, "node_modules" });
  defer allocator.free(node_modules_path);

  const bin_dir = try std.fs.path.join(allocator, &.{ node_modules_path, ".bin" });
  errdefer allocator.free(bin_dir);

  const bin_name = if (builtin.os.tag == .windows) "node-gyp.cmd" else "node-gyp";
  const bin_path = try std.fs.path.join(allocator, &.{ bin_dir, bin_name });
  defer allocator.free(bin_path);

  if (std.Io.Dir.cwd().access(io, bin_path, .{})) |_| {
    return bin_dir;
  } else |_| {}

  try std.Io.Dir.cwd().createDirPath(io, tool_root);
  const package_json_path = try std.fs.path.join(allocator, &.{ tool_root, "package.json" });
  defer allocator.free(package_json_path);
  const lockfile_path = try std.fs.path.join(allocator, &.{ tool_root, "ant.lockb" });
  defer allocator.free(lockfile_path);

  const package_json = try std.fmt.allocPrint(
    allocator,
    \\{{"dependencies":{{"node-gyp":"{s}"}}}}
  , .{ant_managed_node_gyp_version});
  defer allocator.free(package_json);

  {
    const file = try std.Io.Dir.cwd().createFile(io, package_json_path, .{});
    defer file.close(io);
    try file.writeStreamingAll(io, package_json);
  }

  installToolPackageNoLifecycle(ctx, allocator, package_json_path, lockfile_path, node_modules_path) catch |err| {
    debug.log("failed to prepare Ant-managed node-gyp: {}", .{err});
    allocator.free(bin_dir);
    return null;
  };

  std.Io.Dir.cwd().access(io, bin_path, .{}) catch {
    debug.log("Ant-managed node-gyp did not create {s}", .{bin_path});
    allocator.free(bin_dir);
    return null;
  };

  debug.log("prepared Ant-managed node-gyp {s}", .{ant_managed_node_gyp_version});
  return bin_dir;
}

fn replayLifecycleOutputFile(allocator: std.mem.Allocator, path: []const u8, label: []const u8) void {
  var file = std.Io.Dir.cwd().openFile(io, path, .{}) catch return;
  defer file.close(io);

  const stat = file.stat(io) catch return;
  if (stat.size == 0) return;

  const read_len_u64 = @min(stat.size, lifecycle_output_tail_limit);
  const read_len: usize = @intCast(read_len_u64);
  const offset = stat.size - read_len_u64;
  const bytes = allocator.alloc(u8, read_len) catch return;
  defer allocator.free(bytes);

  const n = file.readPositionalAll(io, bytes, offset) catch return;
  if (n == 0) return;

  const stderr = std.Io.File.stderr();
  if (offset > 0) {
    const msg = std.fmt.allocPrint(allocator, "\n[{s}: output truncated to last {d} bytes]\n", .{ label, read_len }) catch return;
    defer allocator.free(msg);
    stderr.writeStreamingAll(io, msg) catch {};
  } else {
    const msg = std.fmt.allocPrint(allocator, "\n[{s}]\n", .{label}) catch return;
    defer allocator.free(msg);
    stderr.writeStreamingAll(io, msg) catch {};
  }
  stderr.writeStreamingAll(io, bytes[0..n]) catch {};
  if (bytes[n - 1] != '\n') stderr.writeStreamingAll(io, "\n") catch {};
}

fn replayLifecycleOutput(allocator: std.mem.Allocator, stdout_path: []const u8, stderr_path: []const u8) void {
  replayLifecycleOutputFile(allocator, stdout_path, "stdout");
  replayLifecycleOutputFile(allocator, stderr_path, "stderr");
}

fn runTrustedPostinstall(
  ctx: *PkgContext,
  trusted: *std.StringHashMap(void),
  node_modules_path: []const u8,
  allocator: std.mem.Allocator,
) bool {
  var env_map = currentEnvMap(allocator) catch {
    ctx.setError("Failed to prepare lifecycle environment");
    return false;
  };
  defer env_map.deinit();

  const cwd = std.Io.Dir.cwd();
  const abs_nm_path = cwd.realPathFileAlloc(io, node_modules_path, allocator) catch {
    ctx.setError("Failed to resolve node_modules for lifecycle scripts");
    return false;
  };
  defer allocator.free(abs_nm_path);

  const bin_path = std.fmt.allocPrint(allocator, "{s}/.bin", .{abs_nm_path}) catch return false;
  defer allocator.free(bin_path);

  const current_path = env_map.get("PATH") orelse "";
  const npm_node_gyp_bin = ensureLifecycleNodeGypShim(ctx, allocator, &env_map) catch null;
  defer if (npm_node_gyp_bin) |path| allocator.free(path);
  const ant_node_gyp_bin = if (npm_node_gyp_bin == null)
    ensureAntManagedNodeGypBin(ctx, allocator) catch null
  else
    null;
  defer if (ant_node_gyp_bin) |path| allocator.free(path);

  const new_path = if (npm_node_gyp_bin) |npm_bin|
    std.fmt.allocPrint(allocator, "{s}{c}{s}{c}{s}", .{ bin_path, pathDelimiter(), npm_bin, pathDelimiter(), current_path }) catch return false
  else if (ant_node_gyp_bin) |ant_bin|
    std.fmt.allocPrint(allocator, "{s}{c}{s}{c}{s}", .{ bin_path, pathDelimiter(), ant_bin, pathDelimiter(), current_path }) catch return false
  else
    std.fmt.allocPrint(allocator, "{s}{c}{s}", .{ bin_path, pathDelimiter(), current_path }) catch return false;
  defer allocator.free(new_path);

  env_map.put("PATH", new_path) catch return false;

  var jobs = std.ArrayListUnmanaged(PostinstallJob).empty;
  defer {
    for (jobs.items) |*job| {
      allocator.free(job.pkg_dir);
      freeLifecycleCommands(allocator, &job.commands);
    }
    jobs.deinit(allocator);
  }

  var key_iter = trusted.keyIterator();
  while (key_iter.next()) |pkg_name_ptr| {
    const pkg_name = pkg_name_ptr.*;

    const pkg_json_path = std.fmt.allocPrint(allocator, "{s}/{s}/package.json", .{
      node_modules_path, pkg_name,
    }) catch continue;
    defer allocator.free(pkg_json_path);

    const content = std.Io.Dir.cwd().readFileAlloc(io, pkg_json_path, allocator, .limited(1024 * 1024)) catch continue;
    defer allocator.free(content);

    var doc = json.JsonDoc.parse(content) catch continue;
    defer doc.deinit();

    const root = doc.root();

    if (root.getObject("scripts")) |scripts| {
      var commands = std.ArrayListUnmanaged(LifecycleCommand).empty;
      if (scripts.getString("install")) |script| {
        appendLifecycleCommand(allocator, &commands, "install", script) catch {};
      }
      if (scripts.getString("postinstall")) |script| {
        appendLifecycleCommand(allocator, &commands, "postinstall", script) catch {};
      }
      if (commands.items.len == 0) {
        commands.deinit(allocator);
        continue;
      }

      if (std.mem.eql(u8, pkg_name, "esbuild")) {
        debug.log("ignoring esbuild lifecycle scripts", .{});
        freeLifecycleCommands(allocator, &commands);
        continue;
      }

      const pkg_dir = std.fmt.allocPrint(allocator, "{s}/{s}", .{
        node_modules_path, pkg_name,
      }) catch continue;

      const marker_path = std.fmt.allocPrint(allocator, "{s}/.postinstall", .{pkg_dir}) catch continue;
      defer allocator.free(marker_path);
      if (lifecycleMarkerMatches(allocator, marker_path, commands.items)) {
        debug.log("postinstall already done: {s}", .{pkg_name});
        allocator.free(pkg_dir);
        freeLifecycleCommands(allocator, &commands);
        continue;
      }

      jobs.append(allocator, .{
        .pkg_name = pkg_name,
        .pkg_dir = pkg_dir,
        .commands = commands,
      }) catch continue;
    }
  }

  if (jobs.items.len == 0) return true;
  for (jobs.items) |job| debug.log("starting lifecycle scripts: {s}", .{job.pkg_name});

  const output_dir = std.fmt.allocPrint(allocator, "{s}/lifecycle-output", .{ctx.cache_dir}) catch return false;
  defer allocator.free(output_dir);
  std.Io.Dir.cwd().createDirPath(io, output_dir) catch {
    ctx.setError("Failed to prepare lifecycle output capture");
    return false;
  };

  var process_io_state = process_env.initThreaded(allocator);
  defer process_io_state.deinit();
  const process_io = process_io_state.io();

  var scripts_run: u32 = 0;
  var failed_count: u32 = 0;
  for (jobs.items, 0..) |*job, i| {
    const msg = std.fmt.allocPrintSentinel(allocator, "Building {s}", .{job.pkg_name}, 0) catch continue;
    ctx.reportProgress(.postinstall, @intCast(i + 1), @intCast(jobs.items.len), msg);
    for (job.commands.items, 0..) |cmd, j| {
      debug.log("running {s}: {s}", .{ cmd.name, job.pkg_name });

      const shell_argv: []const []const u8 = if (builtin.os.tag == .windows)
        &[_][]const u8{ scriptShell(), "/c", cmd.script }
      else
        &[_][]const u8{ scriptShell(), "-c", cmd.script };

      const output_nonce: i128 = @intCast(std.Io.Timestamp.now(io, .boot).toNanoseconds());
      const stdout_path = std.fmt.allocPrint(allocator, "{s}/{d}-{d}-{d}.out", .{ output_dir, output_nonce, i, j }) catch {
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to capture output", .{ cmd.name, job.pkg_name });
        job.failed = true;
        break;
      };
      defer allocator.free(stdout_path);
      const stderr_path = std.fmt.allocPrint(allocator, "{s}/{d}-{d}-{d}.err", .{ output_dir, output_nonce, i, j }) catch {
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to capture output", .{ cmd.name, job.pkg_name });
        job.failed = true;
        break;
      };
      defer allocator.free(stderr_path);

      var stdout_file = std.Io.Dir.cwd().createFile(io, stdout_path, .{}) catch |err| {
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to capture stdout: {}", .{ cmd.name, job.pkg_name, err });
        job.failed = true;
        break;
      };
      var stderr_file = std.Io.Dir.cwd().createFile(io, stderr_path, .{}) catch |err| {
        stdout_file.close(io);
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to capture stderr: {}", .{ cmd.name, job.pkg_name, err });
        job.failed = true;
        break;
      };
      var output_files_closed = false;
      defer if (!output_files_closed) {
        stdout_file.close(io);
        stderr_file.close(io);
      };
      defer std.Io.Dir.cwd().deleteFile(io, stdout_path) catch {};
      defer std.Io.Dir.cwd().deleteFile(io, stderr_path) catch {};

      var child = std.process.spawn(process_io, .{
        .argv = shell_argv,
        .cwd = .{ .path = job.pkg_dir },
        .environ_map = &env_map,
        .expand_arg0 = .no_expand,
        .stdin = .inherit,
        .stdout = .{ .file = stdout_file },
        .stderr = .{ .file = stderr_file },
      }) catch |err| {
        stdout_file.close(io);
        stderr_file.close(io);
        output_files_closed = true;
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to spawn: {}", .{ cmd.name, job.pkg_name, err });
        job.failed = true;
        break;
      };
      const term = child.wait(process_io) catch |err| {
        stdout_file.close(io);
        stderr_file.close(io);
        output_files_closed = true;
        if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: failed to wait: {}", .{ cmd.name, job.pkg_name, err });
        job.failed = true;
        break;
      };
      stdout_file.close(io);
      stderr_file.close(io);
      output_files_closed = true;

      switch (term) {
        .exited => |code| {
          if (code != 0) {
            debug.log("  {s} failed for {s}: exit code {d}", .{ cmd.name, job.pkg_name, code });
            replayLifecycleOutput(allocator, stdout_path, stderr_path);
            if (ctx.last_error == null) {
              ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}: exit code {d}", .{ cmd.name, job.pkg_name, code });
            }
            job.failed = true;
            break;
          } else if (ctx.options.verbose) {
            replayLifecycleOutput(allocator, stdout_path, stderr_path);
          }
        },
        .signal => |sig| {
          job.failed = true;
          replayLifecycleOutput(allocator, stdout_path, stderr_path);
          if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' killed by signal {d}: {s}", .{ cmd.name, @intFromEnum(sig), job.pkg_name });
          debug.log("  {s} killed by signal {d}: {s}", .{ cmd.name, @intFromEnum(sig), job.pkg_name });
          break;
        },
        else => {
          job.failed = true;
          replayLifecycleOutput(allocator, stdout_path, stderr_path);
          if (ctx.last_error == null) ctx.setErrorFmt("Lifecycle script '{s}' failed for {s}", .{ cmd.name, job.pkg_name });
          break;
        },
      }
    }

    if (job.failed) {
      failed_count += 1;
    } else {
      const marker_path = std.fmt.allocPrint(allocator, "{s}/.postinstall", .{job.pkg_dir}) catch continue;
      defer allocator.free(marker_path);
      writeLifecycleMarker(allocator, marker_path, job.commands.items);
      ctx.last_install_result.lifecycle_builds += 1;
      scripts_run += @intCast(job.commands.items.len);
    }
  }

  if (scripts_run > 0) debug.log("ran {d} postinstall scripts", .{scripts_run});
  return failed_count == 0;
}

export fn pkg_add(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  package_spec: [*:0]const u8,
  dev: bool,
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
  res.force_refresh = c.options.force;

  const resolved_pkg = res.resolve(pkg_name, version_constraint, 0) catch |err| {
    setResolveError(c, pkg_name, err);
    return .resolve_error;
  };

  const content = blk: {
    break :blk std.Io.Dir.cwd().readFileAlloc(io, pkg_json_str, arena_alloc, .limited(10 * 1024 * 1024)) catch |err| {
      if (err == error.FileNotFound) break :blk "{}";
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

  const dependency_value = dependencyValueForResolved(arena_alloc, c, pkg_name, version_str, resolved_pkg) catch {
    return .out_of_memory;
  };

  const target_key = if (dev) "devDependencies" else "dependencies";
  
  var deps: std.json.ObjectMap = if (parsed.value.object.get(target_key)) |d|
    if (d == .object) d.object else .empty
  else .empty;

  deps.put(arena_alloc, pkg_name, .{ .string = dependency_value }) catch {
    return .out_of_memory;
  };

  var writer = json.JsonWriter.init() catch {
    return .out_of_memory;
  }; defer writer.deinit();

  const root_obj = writer.createObject();
  writer.setRoot(root_obj);

  var found_target = false;
  for (parsed.value.object.keys(), parsed.value.object.values()) |key, value| {
    if (std.mem.eql(u8, key, target_key)) {
      found_target = true;
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

  if (!found_target) {
    const deps_obj = writer.createObject();
    writer.objectAdd(deps_obj, pkg_name, writer.createString(dependency_value));
    writer.objectAdd(root_obj, target_key, deps_obj);
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

export fn pkg_resolve_check_many(
  ctx: ?*PkgContext,
  package_specs: [*]const [*:0]const u8,
  count: u32,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const http = c.http orelse return .network_error;
  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    null,
    if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org",
    &c.metadata_cache,
  );
  defer res.deinit();
  res.force_refresh = c.options.force;

  for (0..count) |i| {
    const spec_str = std.mem.span(package_specs[i]);
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

    _ = res.resolve(pkg_name, version_constraint, 0) catch |err| {
      setResolveError(c, pkg_name, err);
      return .resolve_error;
    };
  }

  return .ok;
}

const ParsedPackageSpec = struct {
  name: []const u8,
  constraint: []const u8,
};

fn parsePackageSpec(spec_str: []const u8) ParsedPackageSpec {
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

  return .{ .name = pkg_name, .constraint = version_constraint };
}

fn metadataSatisfies(allocator: std.mem.Allocator, json_data: []const u8, constraint_str: []const u8) bool {
  var metadata = resolver.PackageMetadata.parseFromJson(allocator, json_data) catch return false;
  defer metadata.deinit();

  const constraint = resolver.Constraint.parse(constraint_str) catch return false;
  if (constraint.kind == .any) {
    if (metadata.dist_tag_latest) |latest| {
      for (metadata.versions.items) |*v| {
        if (v.version.order(latest) == .eq and v.matchesPlatform()) return true;
      }
    }
  }

  for (metadata.versions.items) |*v| {
    if (metadata.constraintMatches(constraint, v.version) and v.matchesPlatform()) return true;
  }

  return false;
}

fn metadataResultSatisfies(allocator: std.mem.Allocator, result: *const fetcher.Fetcher.MetadataResult, constraint: []const u8) bool {
  if (result.has_error or result.status_code != 200) return false;
  const data = result.data orelse return false;
  return metadataSatisfies(allocator, data, constraint);
}

fn registryChoiceFromByte(choice: u8) ?RegistryChoice {
  return switch (choice) {
    @intFromEnum(RegistryChoice.primary) => .primary,
    @intFromEnum(RegistryChoice.fallback) => .fallback,
    else => null,
  };
}

fn registryChoiceCacheHash(
  package_specs: [*]const [*:0]const u8,
  count: u32,
  primary_registry: []const u8,
  fallback_registry: []const u8,
) u64 {
  var hasher = std.hash.Wyhash.init(0);
  hasher.update(primary_registry);
  hasher.update(&[_]u8{0});
  hasher.update(fallback_registry);
  for (0..count) |i| {
    hasher.update(&[_]u8{0});
    hasher.update(std.mem.span(package_specs[i]));
  }
  return hasher.final();
}

fn registryPrimaryMissCacheHash(primary_registry: []const u8, package_name: []const u8) u64 {
  var hasher = std.hash.Wyhash.init(0);
  hasher.update(primary_registry);
  hasher.update(&[_]u8{0});
  hasher.update(package_name);
  return hasher.final();
}

fn registryChoiceCachePath(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  package_specs: [*]const [*:0]const u8,
  count: u32,
  primary_registry: []const u8,
  fallback_registry: []const u8,
) ![]const u8 {
  const default_cache_dir = if (cache_dir_override == null)
    try PkgContext.getDefaultCacheDir(allocator)
  else
    null;
  defer if (default_cache_dir) |dir| allocator.free(dir);
  const cache_dir = cache_dir_override orelse default_cache_dir.?;
  const hash = registryChoiceCacheHash(package_specs, count, primary_registry, fallback_registry);
  return std.fmt.allocPrint(allocator, "{s}/registry-choice/{x}.choice", .{ cache_dir, hash });
}

fn registryPrimaryMissCachePath(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  primary_registry: []const u8,
  package_name: []const u8,
) ![]const u8 {
  const default_cache_dir = if (cache_dir_override == null)
    try PkgContext.getDefaultCacheDir(allocator)
  else
    null;
  defer if (default_cache_dir) |dir| allocator.free(dir);
  const cache_dir = cache_dir_override orelse default_cache_dir.?;
  const hash = registryPrimaryMissCacheHash(primary_registry, package_name);
  return std.fmt.allocPrint(allocator, "{s}/registry-miss/{x}.miss", .{ cache_dir, hash });
}

fn lookupCachedRegistryChoice(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  package_specs: [*]const [*:0]const u8,
  count: u32,
  primary_registry: []const u8,
  fallback_registry: []const u8,
) ?RegistryChoice {
  const path = registryChoiceCachePath(allocator, cache_dir_override, package_specs, count, primary_registry, fallback_registry) catch return null;
  defer allocator.free(path);

  const data = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(64)) catch return null;
  defer allocator.free(data);
  if (data.len < @sizeOf(i64) + 1) return null;

  var cached_at: i64 = undefined;
  @memcpy(std.mem.asBytes(&cached_at), data[0..@sizeOf(i64)]);
  const now = std.Io.Timestamp.now(io, .real).toSeconds();
  if (!timestampWithinTtl(cached_at, now, 24 * 60 * 60, 60)) return null;
  return registryChoiceFromByte(data[@sizeOf(i64)]);
}

fn lookupCachedRegistryPrimaryMiss(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  primary_registry: []const u8,
  package_name: []const u8,
) bool {
  const path = registryPrimaryMissCachePath(allocator, cache_dir_override, primary_registry, package_name) catch return false;
  defer allocator.free(path);

  const data = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(64)) catch return false;
  defer allocator.free(data);
  if (data.len < @sizeOf(i64)) return false;

  var cached_at: i64 = undefined;
  @memcpy(std.mem.asBytes(&cached_at), data[0..@sizeOf(i64)]);
  const now = std.Io.Timestamp.now(io, .real).toSeconds();
  return timestampWithinTtl(cached_at, now, 24 * 60 * 60, 60);
}

fn storeRegistryChoice(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  package_specs: [*]const [*:0]const u8,
  count: u32,
  primary_registry: []const u8,
  fallback_registry: []const u8,
  choice: RegistryChoice,
) void {
  const path = registryChoiceCachePath(allocator, cache_dir_override, package_specs, count, primary_registry, fallback_registry) catch return;
  defer allocator.free(path);
  if (std.fs.path.dirname(path)) |dir| {
    std.Io.Dir.cwd().createDirPath(io, dir) catch return;
  }

  var value: [@sizeOf(i64) + 1]u8 = undefined;
  const now: i64 = std.Io.Timestamp.now(io, .real).toSeconds();
  @memcpy(value[0..@sizeOf(i64)], std.mem.asBytes(&now));
  value[@sizeOf(i64)] = @intCast(@intFromEnum(choice));
  std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = &value }) catch {};
}

fn storeRegistryPrimaryMiss(
  allocator: std.mem.Allocator,
  cache_dir_override: ?[]const u8,
  primary_registry: []const u8,
  package_name: []const u8,
) void {
  const path = registryPrimaryMissCachePath(allocator, cache_dir_override, primary_registry, package_name) catch return;
  defer allocator.free(path);
  if (std.fs.path.dirname(path)) |dir| {
    std.Io.Dir.cwd().createDirPath(io, dir) catch return;
  }

  var value: [@sizeOf(i64)]u8 = undefined;
  const now: i64 = std.Io.Timestamp.now(io, .real).toSeconds();
  @memcpy(value[0..@sizeOf(i64)], std.mem.asBytes(&now));
  std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = &value }) catch {};
}

export fn pkg_choose_registry_many(
  package_specs: [*]const [*:0]const u8,
  count: u32,
  cache_dir_override: ?[*:0]const u8,
  primary_registry: [*:0]const u8,
  fallback_registry: [*:0]const u8,
) RegistryChoice {
  if (count == 0) return .unknown;

  const total_start = debug.nowNs();
  var trace = debug.StageTrace{};
  var stage_start = total_start;
  const primary_registry_str = std.mem.span(primary_registry);
  const fallback_registry_str = std.mem.span(fallback_registry);
  const registry_choice_cache_dir = if (cache_dir_override) |dir| std.mem.span(dir) else null;
  debug.trace("registry choose start: specs={d} primary={s} fallback={s}", .{
    count,
    primary_registry_str,
    fallback_registry_str,
  });

  if (lookupCachedRegistryChoice(global_allocator, registry_choice_cache_dir, package_specs, count, primary_registry_str, fallback_registry_str)) |choice| {
    _ = trace.mark("registry choice cache lookup", stage_start);
    trace.summary("registry choose");
    debug.trace("registry choose done: choice={s} source=cache total={d}us", .{
      if (choice == .primary) "primary" else "fallback",
      debug.elapsedUsSince(total_start),
    });
    return choice;
  }
  stage_start = trace.mark("registry choice cache lookup", stage_start);

  var arena_state = std.heap.ArenaAllocator.init(global_allocator);
  defer arena_state.deinit();
  const arena_alloc = arena_state.allocator();

  var primary_fetcher = fetcher.Fetcher.init(global_allocator, std.mem.span(primary_registry)) catch return .unknown;
  defer primary_fetcher.deinit();
  stage_start = trace.mark("registry primary init", stage_start);

  var fallback_fetcher = fetcher.Fetcher.init(global_allocator, std.mem.span(fallback_registry)) catch return .unknown;
  defer fallback_fetcher.deinit();
  stage_start = trace.mark("registry fallback init", stage_start);

  const names = arena_alloc.alloc([]const u8, count) catch return .unknown;
  const constraints = arena_alloc.alloc([]const u8, count) catch return .unknown;
  for (0..count) |i| {
    const spec = parsePackageSpec(std.mem.span(package_specs[i]));
    names[i] = spec.name;
    constraints[i] = spec.constraint;
  }
  stage_start = trace.mark("registry spec parse", stage_start);

  var primary_miss_cache_hit = true;
  for (names) |name| {
    if (!lookupCachedRegistryPrimaryMiss(global_allocator, registry_choice_cache_dir, primary_registry_str, name)) {
      primary_miss_cache_hit = false;
      break;
    }
  }

  if (primary_miss_cache_hit) {
    stage_start = trace.mark("registry primary miss cache lookup", stage_start);
    debug.trace("registry primary miss cache hit: count={d}", .{count});

    const fallback_results = fallback_fetcher.fetchMetadataBatch(names, arena_alloc) catch return .unknown;
    stage_start = trace.mark("registry fallback metadata", stage_start);

    var fallback_ok: u32 = 0;
    var fallback_miss: u32 = 0;
    for (fallback_results, 0..) |*result, i| {
      if (metadataResultSatisfies(arena_alloc, result, constraints[i])) {
        fallback_ok += 1;
      } else {
        fallback_miss += 1;
      }
    }

    _ = trace.mark("registry fallback evaluate", stage_start);
    debug.trace("registry fallback result: ok={d} miss={d}", .{ fallback_ok, fallback_miss });
    trace.summary("registry choose");

    if (fallback_miss > 0) {
      debug.trace("registry choose done: choice=unknown source=primary-miss-cache total={d}us", .{debug.elapsedUsSince(total_start)});
      return .unknown;
    }

    storeRegistryChoice(global_allocator, registry_choice_cache_dir, package_specs, count, primary_registry_str, fallback_registry_str, .fallback);
    debug.trace("registry choose done: choice=fallback source=primary-miss-cache total={d}us", .{debug.elapsedUsSince(total_start)});
    return .fallback;
  }
  stage_start = trace.mark("registry primary miss cache lookup", stage_start);

  const registry_results = fetcher.Fetcher.fetchMetadataDualBatch(primary_fetcher, fallback_fetcher, names, arena_alloc) catch return .unknown;
  stage_start = trace.mark("registry metadata", stage_start);

  var primary_all_available = true;
  var primary_had_not_found = false;
  var primary_had_non_not_found_error = false;
  var primary_ok: u32 = 0;
  var primary_not_found: u32 = 0;
  var primary_unsatisfied: u32 = 0;
  var primary_error: u32 = 0;

  for (registry_results.primary, 0..) |*result, i| {
    const primary_available = metadataResultSatisfies(arena_alloc, result, constraints[i]);
    if (primary_available) {
      primary_ok += 1;
      continue;
    }

    if (!primary_available) {
      primary_all_available = false;
      if (result.status_code == 404) {
        storeRegistryPrimaryMiss(global_allocator, registry_choice_cache_dir, primary_registry_str, names[i]);
        primary_had_not_found = true;
        primary_not_found += 1;
      } else if (result.status_code == 200) {
        primary_had_non_not_found_error = true;
        primary_unsatisfied += 1;
      } else {
        primary_had_non_not_found_error = true;
        primary_error += 1;
      }
    }
  }
  stage_start = trace.mark("registry primary evaluate", stage_start);
  debug.trace("registry primary result: ok={d} not_found={d} unsatisfied={d} error={d}", .{
    primary_ok,
    primary_not_found,
    primary_unsatisfied,
    primary_error,
  });

  if (primary_all_available) {
    storeRegistryChoice(global_allocator, registry_choice_cache_dir, package_specs, count, primary_registry_str, fallback_registry_str, .primary);
    trace.summary("registry choose");
    debug.trace("registry choose done: choice=primary total={d}us", .{debug.elapsedUsSince(total_start)});
    return .primary;
  }
  if (!primary_had_not_found or primary_had_non_not_found_error) {
    trace.summary("registry choose");
    debug.trace("registry choose done: choice=unknown total={d}us", .{debug.elapsedUsSince(total_start)});
    return .unknown;
  }
  var fallback_ok: u32 = 0;
  var fallback_miss: u32 = 0;
  var fallback_covers_primary_misses = true;
  for (registry_results.fallback, 0..) |*result, i| {
    if (metadataResultSatisfies(arena_alloc, result, constraints[i])) {
      fallback_ok += 1;
    } else {
      fallback_miss += 1;
      if (registry_results.primary[i].status_code == 404) {
        fallback_covers_primary_misses = false;
      }
    }
  }
  _ = trace.mark("registry fallback evaluate", stage_start);
  debug.trace("registry fallback result: ok={d} miss={d}", .{ fallback_ok, fallback_miss });
  trace.summary("registry choose");

  if (!fallback_covers_primary_misses) {
    debug.trace("registry choose done: choice=unknown total={d}us", .{debug.elapsedUsSince(total_start)});
    return .unknown;
  }
  if (primary_ok > 0) {
    storeRegistryChoice(global_allocator, registry_choice_cache_dir, package_specs, count, primary_registry_str, fallback_registry_str, .primary);
    debug.trace("registry choose done: choice=primary source=mixed total={d}us", .{debug.elapsedUsSince(total_start)});
    return .primary;
  }
  storeRegistryChoice(global_allocator, registry_choice_cache_dir, package_specs, count, primary_registry_str, fallback_registry_str, .fallback);
  debug.trace("registry choose done: choice=fallback total={d}us", .{debug.elapsedUsSince(total_start)});
  return .fallback;
}

export fn pkg_cached_resolution_registry_choice(
  package_json_path: [*:0]const u8,
  cache_dir_override: ?[*:0]const u8,
  primary_registry: [*:0]const u8,
  fallback_registry: [*:0]const u8,
) RegistryChoice {
  var arena_state = std.heap.ArenaAllocator.init(global_allocator);
  defer arena_state.deinit();
  const arena_alloc = arena_state.allocator();

  const default_cache_dir = if (cache_dir_override == null)
    PkgContext.getDefaultCacheDir(arena_alloc) catch return .unknown
  else
    null;
  const cache_dir = if (cache_dir_override) |dir| std.mem.span(dir) else default_cache_dir.?;
  const pkg_json_path_z = arena_alloc.dupeZ(u8, std.mem.span(package_json_path)) catch return .unknown;
  var pkg_json = json.PackageJson.parse(arena_alloc, pkg_json_path_z) catch return .unknown;
  defer pkg_json.deinit(arena_alloc);

  const lockfile_path = lockfilePathForPackageJson(arena_alloc, std.mem.span(package_json_path)) catch null;
  if (lockfile_path) |path| {
    defer arena_alloc.free(path);
    const primary_key = resolutionCacheKey(arena_alloc, &pkg_json, std.mem.span(primary_registry)) orelse 0;
    if (lockfileMatchesResolutionHash(path, primary_key)) return .primary;
    const fallback_key = resolutionCacheKey(arena_alloc, &pkg_json, std.mem.span(fallback_registry)) orelse 0;
    if (lockfileMatchesResolutionHash(path, fallback_key)) return .fallback;
  }

  if (cachedResolutionLockfileAvailable(arena_alloc, cache_dir, &pkg_json, std.mem.span(primary_registry))) {
    return .primary;
  }
  if (cachedResolutionLockfileAvailable(arena_alloc, cache_dir, &pkg_json, std.mem.span(fallback_registry))) {
    return .fallback;
  }
  return .unknown;
}

export fn pkg_add_many(
  ctx: ?*PkgContext,
  package_json_path: [*:0]const u8,
  package_specs: [*]const [*:0]const u8,
  count: u32,
  dev: bool,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const pkg_json_str = std.mem.span(package_json_path);

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
  res.force_refresh = c.options.force;

  res.resolve_shallow = true;
  const ResolvedEntry = struct {
    name: []const u8,
    dependency_value: []const u8,
  };

  const resolved = arena_alloc.alloc(ResolvedEntry, count) catch return .out_of_memory;

  for (0..count) |i| {
    const spec_str = std.mem.span(package_specs[i]);
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

    const resolved_pkg = res.resolve(pkg_name, version_constraint, 0) catch |err| {
      setResolveError(c, pkg_name, err);
      return .resolve_error;
    };

    const version_str = resolved_pkg.version.format(arena_alloc) catch return .out_of_memory;
    const dependency_value = dependencyValueForResolved(arena_alloc, c, pkg_name, version_str, resolved_pkg) catch return .out_of_memory;
    resolved[i] = .{ .name = pkg_name, .dependency_value = dependency_value };
  }

  const content = blk: {
    break :blk std.Io.Dir.cwd().readFileAlloc(io, pkg_json_str, arena_alloc, .limited(10 * 1024 * 1024)) catch |err| {
      if (err == error.FileNotFound) break :blk "{}";
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

  const target_key = if (dev) "devDependencies" else "dependencies";

  var deps: std.json.ObjectMap = if (parsed.value.object.get(target_key)) |d|
    if (d == .object) d.object else .empty
  else .empty;

  for (resolved) |entry| {
    deps.put(arena_alloc, entry.name, .{ .string = entry.dependency_value }) catch return .out_of_memory;
  }

  var writer = json.JsonWriter.init() catch return .out_of_memory;
  defer writer.deinit();

  const root_obj = writer.createObject();
  writer.setRoot(root_obj);

  var found_target = false;
  for (parsed.value.object.keys(), parsed.value.object.values()) |key, value| {
    if (std.mem.eql(u8, key, target_key)) {
      found_target = true;
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

  if (!found_target) {
    const deps_obj = writer.createObject();
    for (resolved) |entry| {
      writer.objectAdd(deps_obj, entry.name, writer.createString(entry.dependency_value));
    }
    writer.objectAdd(root_obj, target_key, deps_obj);
  }

  const pkg_json_z = arena_alloc.dupeZ(u8, pkg_json_str) catch return .out_of_memory;

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

  const content = std.Io.Dir.cwd().readFileAlloc(io, pkg_json_str, arena_alloc, .limited(10 * 1024 * 1024)) catch {
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
    .total_size = stats.cache_size,
    .db_size = stats.db_size,
    .package_count = @intCast(stats.entries),
  };

  return .ok;
}

export fn pkg_cache_prune(ctx: ?*PkgContext, max_age_days: u32) i32 {
  const c = ctx orelse return @intFromEnum(PkgError.invalid_argument);
  const db = c.cache_db orelse return @intFromEnum(PkgError.cache_error);
  
  const pruned = db.prune(max_age_days) catch return @intFromEnum(PkgError.cache_error);
  return @intCast(pruned);
}

export fn pkg_get_bin_path(
  node_modules_path: [*:0]const u8,
  bin_name: [*:0]const u8,
  out_path: [*]u8,
  out_path_len: usize,
) c_int {
  const nm_path = std.mem.span(node_modules_path);
  const full = std.mem.span(bin_name);

  const start: usize = if (full.len > 0 and full[0] == '@')
    (std.mem.indexOfScalar(u8, full[1..], '/') orelse return -1) + 2
  else 0;
  const name, const constraint_str = if (std.mem.indexOfScalar(u8, full[start..], '@')) |i|
    .{ full[0..start + i], full[start + i + 1..] }
  else
    .{ full, @as([]const u8, "") };

  var path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const bin_path = std.fmt.bufPrint(&path_buf, "{s}/.bin/{s}", .{ nm_path, name }) catch return -1;

  std.Io.Dir.cwd().access(io, bin_path, .{}) catch return -1;

  if (constraint_str.len > 0) {
    const constraint = resolver.Constraint.parse(constraint_str) catch return -1;
    if (constraint.kind != .any) {
      var pkg_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
      const pkg_path = std.fmt.bufPrint(&pkg_buf, "{s}/{s}/package.json", .{ nm_path, name }) catch return -1;
      const pkg_path_z = pkg_buf[0..pkg_path.len :0];
      var doc = json.JsonDoc.parseFile(pkg_path_z) catch return -1;
      defer doc.deinit();
      const version_str = doc.root().getString("version") orelse return -1;
      const installed = resolver.Version.parse(version_str) catch return -1;
      if (!constraint.satisfies(installed)) return -1;
    }
  }

  var real_path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const real_path_len = std.Io.Dir.cwd().realPathFile(io, bin_path, &real_path_buf) catch return -1;

  if (real_path_len >= out_path_len) return -1;

  @memcpy(out_path[0..real_path_len], real_path_buf[0..real_path_len]);
  out_path[real_path_len] = 0;

  return @intCast(real_path_len);
}

export fn pkg_list_bins(
  node_modules_path: [*:0]const u8,
  callback: ?*const fn ([*:0]const u8, ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) c_int {
  const nm_path = std.mem.span(node_modules_path);

  var path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const bin_dir_path = std.fmt.bufPrint(&path_buf, "{s}/.bin", .{nm_path}) catch return -1;

  var dir = std.Io.Dir.cwd().openDir(io, bin_dir_path, .{ .iterate = true }) catch return -1;
  defer dir.close(io);

  var count: c_int = 0;
  var iter = dir.iterate();
  while (iter.next(io) catch null) |entry| {
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

  var path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const pkg_json_path = std.fmt.bufPrint(&path_buf, "{s}/{s}/package.json", .{ nm_path, pkg_name }) catch return -1;

  const content = std.Io.Dir.cwd().readFileAlloc(io, pkg_json_path, global_allocator, .limited(1024 * 1024)) catch return -1;
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

    if (std.Io.Dir.cwd().access(io, "server.js", .{})) |_| {
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
  env_map: *std.process.Environ.Map,
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
    &[_][]const u8{ scriptShell(), "/c", script_z }
  else &[_][]const u8{ scriptShell(), "-c", script_z };

  var process_io_state = process_env.initThreaded(allocator);
  defer process_io_state.deinit();
  const process_io = process_io_state.io();

  var child = try std.process.spawn(process_io, .{
    .argv = shell_argv,
    .environ_map = env_map,
    .expand_arg0 = .no_expand,
    .stdin = .inherit,
    .stdout = .inherit,
    .stderr = .inherit,
  });
  const term = try child.wait(process_io);

  return switch (term) {
    .exited => |code| .{ .exit_code = code, .signal = 0 },
    .signal => |sig| .{ .exit_code = -1, .signal = @intCast(@intFromEnum(sig)) },
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

  var env_map = currentEnvMap(allocator) catch return .out_of_memory;
  defer env_map.deinit(); const nm_path = std.mem.span(node_modules_path);

  const cwd = std.Io.Dir.cwd();
  const abs_nm_path = cwd.realPathFileAlloc(io, nm_path, allocator) catch nm_path;
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
    const pre_event = std.fmt.allocPrint(allocator, "pre{s}", .{name}) catch name;
    defer if (pre_event.ptr != name.ptr) allocator.free(pre_event);
    env_map.put("npm_lifecycle_event", pre_event) catch {};
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
    const post_event = std.fmt.allocPrint(allocator, "post{s}", .{name}) catch name;
    defer if (post_event.ptr != name.ptr) allocator.free(post_event);
    env_map.put("npm_lifecycle_event", post_event) catch {};
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

export fn pkg_info(
  ctx: ?*PkgContext,
  package_spec: [*:0]const u8,
  out: *PkgInfo,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  c.clearInfo();
  _ = c.arena_state.reset(.retain_capacity);
  const arena = c.arena_state.allocator();

  const http = c.http orelse return .network_error;
  const spec = std.mem.span(package_spec);
  
  var name: []const u8 = spec;
  var requested_version: ?[]const u8 = null;
  if (std.mem.lastIndexOf(u8, spec, "@")) |at_idx| {
    if (at_idx > 0) {
      name = spec[0..at_idx];
      requested_version = spec[at_idx + 1..];
    }
  }

  const data = http.fetchMetadataFull(name, true, c.allocator) catch |err| {
    c.setErrorFmt("Failed to fetch package info: {}", .{err});
    return .network_error;
  }; defer c.allocator.free(data);

  var doc = json.JsonDoc.parse(data) catch {
    c.setError("Failed to parse package metadata");
    return .resolve_error;
  };
  
  defer doc.deinit();
  const root = doc.root();
  
  const versions_obj = root.getObject("versions") orelse {
    c.setError("No versions found");
    return .not_found;
  };

  var versions_iter = versions_obj.objectIterator() orelse return .resolve_error;
  defer versions_iter.deinit();
  var version_count: u32 = 0;
  while (versions_iter.next()) |_| version_count += 1;

  var version_str: []const u8 = "";
  if (requested_version) |rv| {
    version_str = rv;
  } else if (root.getObject("dist-tags")) |tags| {
    version_str = tags.getString("latest") orelse "";
  }

  const version_z = arena.dupeZ(u8, version_str) catch return .out_of_memory;

  const version_obj = versions_obj.getObject(version_z) orelse {
    c.setErrorFmt("Version {s} not found", .{version_str});
    return .not_found;
  };

  var dep_count: u32 = 0;
  if (version_obj.getObject("dependencies")) |deps| {
    var deps_iter = deps.objectIterator() orelse return .resolve_error;
    defer deps_iter.deinit();
    while (deps_iter.next()) |entry| {
      dep_count += 1;
      if (entry.value.asString()) |ver| {
        c.info_dependencies.append(c.allocator, .{
          .name = c.storeInfoString(entry.key) catch continue,
          .version = c.storeInfoString(ver) catch continue,
        }) catch continue;
      }
    }
  }

  const dist = version_obj.getObject("dist");
  const unpacked_size: u64 = if (dist) |d| @as(u64, @intCast(d.getInt("unpackedSize") orelse 0)) else 0;

  var keywords_buf = std.ArrayListUnmanaged(u8).empty;
  defer keywords_buf.deinit(c.allocator);
  if (version_obj.getArray("keywords")) |kw_arr| {
    var kw_iter = kw_arr.arrayIterator() orelse return .resolve_error;
    defer kw_iter.deinit();
    var first = true;
    while (kw_iter.next()) |kw_val| {
      if (kw_val.asString()) |kw| {
        if (!first) keywords_buf.appendSlice(c.allocator, ", ") catch {};
        keywords_buf.appendSlice(c.allocator, kw) catch {};
        first = false;
      }
    }
  }

  out.* = .{
    .name = c.storeInfoString(root.getString("name") orelse name) catch return .out_of_memory,
    .version = c.storeInfoString(version_str) catch return .out_of_memory,
    .description = c.storeInfoString(version_obj.getString("description") orelse "") catch return .out_of_memory,
    .license = c.storeInfoString(version_obj.getString("license") orelse "") catch return .out_of_memory,
    .homepage = c.storeInfoString(version_obj.getString("homepage") orelse "") catch return .out_of_memory,
    .tarball = c.storeInfoString(if (dist) |d| d.getString("tarball") orelse "" else "") catch return .out_of_memory,
    .shasum = c.storeInfoString(if (dist) |d| d.getString("shasum") orelse "" else "") catch return .out_of_memory,
    .integrity = c.storeInfoString(if (dist) |d| d.getString("integrity") orelse "" else "") catch return .out_of_memory,
    .keywords = c.storeInfoString(keywords_buf.items) catch return .out_of_memory,
    .published = c.storeInfoString(if (root.getObject("time")) |t| t.getString(version_z) orelse "" else "") catch return .out_of_memory,
    .dep_count = dep_count,
    .version_count = version_count,
    .unpacked_size = unpacked_size,
  };

  if (root.getObject("dist-tags")) |tags| {
    var tags_iter = tags.objectIterator() orelse return .ok;
    defer tags_iter.deinit();
    while (tags_iter.next()) |entry| {
      if (entry.value.asString()) |ver| {
        c.info_dist_tags.append(c.allocator, .{
          .tag = c.storeInfoString(entry.key) catch continue,
          .version = c.storeInfoString(ver) catch continue,
        }) catch continue;
      }
    }
  }

  if (root.getArray("maintainers")) |maint_arr| {
    var maint_iter = maint_arr.arrayIterator() orelse return .ok;
    defer maint_iter.deinit();
    while (maint_iter.next()) |maint_val| {
      const maint_name = maint_val.getString("name") orelse continue;
      const maint_email = maint_val.getString("email") orelse "";
      c.info_maintainers.append(c.allocator, .{
        .name = c.storeInfoString(maint_name) catch continue,
        .email = c.storeInfoString(maint_email) catch continue,
      }) catch continue;
    }
  }

  return .ok;
}

export fn pkg_info_dist_tag_count(ctx: ?*const PkgContext) u32 {
  const c = ctx orelse return 0;
  return @intCast(c.info_dist_tags.items.len);
}

export fn pkg_info_get_dist_tag(ctx: ?*const PkgContext, index: u32, out: *DistTag) PkgError {
  const c = ctx orelse return .invalid_argument;
  if (index >= c.info_dist_tags.items.len) return .invalid_argument;
  out.* = c.info_dist_tags.items[index];
  return .ok;
}

export fn pkg_info_maintainer_count(ctx: ?*const PkgContext) u32 {
  const c = ctx orelse return 0;
  return @intCast(c.info_maintainers.items.len);
}

export fn pkg_info_get_maintainer(ctx: ?*const PkgContext, index: u32, out: *Maintainer) PkgError {
  const c = ctx orelse return .invalid_argument;
  if (index >= c.info_maintainers.items.len) return .invalid_argument;
  out.* = c.info_maintainers.items[index];
  return .ok;
}

export fn pkg_info_dependency_count(ctx: ?*const PkgContext) u32 {
  const c = ctx orelse return 0;
  return @intCast(c.info_dependencies.items.len);
}

export fn pkg_info_get_dependency(ctx: ?*const PkgContext, index: u32, out: *Dependency) PkgError {
  const c = ctx orelse return .invalid_argument;
  if (index >= c.info_dependencies.items.len) return .invalid_argument;
  out.* = c.info_dependencies.items[index];
  return .ok;
}

const BinSelection = struct {
  err: PkgError,
  name: []const u8 = "",
};

fn packageSimpleName(pkg_name: []const u8) []const u8 {
  if (std.mem.lastIndexOfScalar(u8, pkg_name, '/')) |slash| {
    return pkg_name[slash + 1 ..];
  }
  return pkg_name;
}

fn normalizedBinTarget(path: []const u8) []const u8 {
  if (std.mem.startsWith(u8, path, "./")) return path[2..];
  return path;
}

fn appendBinName(list: *std.ArrayList(u8), allocator: std.mem.Allocator, name: []const u8) !void {
  if (list.items.len > 0) try list.appendSlice(allocator, ", ");
  try list.appendSlice(allocator, name);
}

fn selectPackageBinName(
  c: *PkgContext,
  allocator: std.mem.Allocator,
  node_modules_path: []const u8,
  pkg_name: []const u8,
) BinSelection {
  const simple_name = packageSimpleName(pkg_name);

  const pkg_json_path = std.fmt.allocPrint(allocator, "{s}/{s}/package.json", .{
    node_modules_path,
    pkg_name,
  }) catch return .{ .err = .out_of_memory };

  defer allocator.free(pkg_json_path);

  const content = std.Io.Dir.cwd().readFileAlloc(io, pkg_json_path, allocator, .limited(1024 * 1024)) catch {
    c.setErrorFmt("Package '{s}' was installed, but its package.json could not be read", .{pkg_name});
    return .{ .err = .io_error };
  };
  defer allocator.free(content);

  var doc = json.JsonDoc.parse(content) catch {
    c.setErrorFmt("Package '{s}' was installed, but its package.json could not be parsed", .{pkg_name});
    return .{ .err = .io_error };
  };
  defer doc.deinit();

  const root_val = doc.root();

  if (root_val.getString("bin")) |_| {
    const selected = allocator.dupe(u8, simple_name) catch return .{ .err = .out_of_memory };
    return .{ .err = .ok, .name = selected };
  }

  const bin_obj = root_val.getObject("bin") orelse {
    c.setErrorFmt("Package '{s}' does not declare any binaries", .{pkg_name});
    return .{ .err = .not_found };
  };

  var iter = bin_obj.objectIterator() orelse {
    c.setErrorFmt("Package '{s}' does not declare any usable binaries", .{pkg_name});
    return .{ .err = .not_found };
  };

  var valid_count: usize = 0;
  var first_name: []const u8 = "";
  var first_target: []const u8 = "";
  var package_named_bin: ?[]const u8 = null;
  var all_targets_same = true;
  var names: std.ArrayList(u8) = .empty;

  while (iter.next()) |entry| {
    const target = entry.value.asString() orelse continue;

    if (valid_count == 0) {
      first_name = entry.key;
      first_target = normalizedBinTarget(target);
    } else if (!std.mem.eql(u8, normalizedBinTarget(target), first_target)) {
      all_targets_same = false;
    }

    appendBinName(&names, allocator, entry.key) catch return .{ .err = .out_of_memory };
    valid_count += 1;

    if (std.mem.eql(u8, entry.key, simple_name)) {
      package_named_bin = entry.key;
    }
  }

  const selected = package_named_bin orelse if (valid_count == 1 or all_targets_same) first_name else "";
  if (selected.len > 0) {
    const selected_copy = allocator.dupe(u8, selected) catch return .{ .err = .out_of_memory };
    return .{ .err = .ok, .name = selected_copy };
  }

  if (valid_count == 0) {
    c.setErrorFmt("Package '{s}' does not declare any usable binaries", .{pkg_name});
  } else {
    c.setErrorFmt("Package '{s}' exposes multiple binaries ({s}) and no default could be inferred", .{
      pkg_name,
      names.items,
    });
  }

  return .{ .err = .not_found };
}

export fn pkg_exec_temp(
  ctx: ?*PkgContext,
  package_spec: [*:0]const u8,
  out_bin_path: [*]u8,
  out_bin_path_len: usize,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  _ = c.arena_state.reset(.retain_capacity);
  const arena_alloc = c.arena_state.allocator();

  const spec_str = std.mem.span(package_spec);
  
  var pkg_name: []const u8 = spec_str;
  var bin_name: []const u8 = spec_str;
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

  if (std.mem.lastIndexOfScalar(u8, pkg_name, '/')) |slash| {
    bin_name = pkg_name[slash + 1 ..];
  } else {
    bin_name = pkg_name;
  }

  const exec_base = std.fmt.allocPrint(arena_alloc, "{s}/exec", .{c.cache_dir}) catch return .out_of_memory;
  const temp_nm_path = std.fmt.allocPrint(arena_alloc, "{s}/{s}", .{exec_base, pkg_name}) catch return .out_of_memory;
  const temp_pkg_json = std.fmt.allocPrint(arena_alloc, "{s}/package.json", .{temp_nm_path}) catch return .out_of_memory;
  const temp_nm_dir = std.fmt.allocPrint(arena_alloc, "{s}/node_modules", .{temp_nm_path}) catch return .out_of_memory;
  const temp_lockfile = std.fmt.allocPrint(arena_alloc, "{s}/ant.lockb", .{temp_nm_path}) catch return .out_of_memory;

  if (std.Io.Dir.cwd().openDir(io, exec_base, .{ .iterate = true })) |dir| {
    var d = dir;
    defer d.close(io);
    
    const stat = d.statFile(io, pkg_name, .{}) catch null;
    if (stat) |s| {
      const now: i128 = std.Io.Timestamp.now(io, .boot).toNanoseconds();
      const mtime: i128 = s.mtime.nanoseconds;
      const age_ns = now - mtime;
      const hours_24_ns: i128 = 24 * 60 * 60 * 1_000_000_000;
      
      if (age_ns > hours_24_ns) {
        debug.log("exec: cleaning stale cache for {s} (age: {d}h)", .{
          pkg_name, @divFloor(age_ns, 60 * 60 * 1_000_000_000),
        });
        d.deleteTree(io, pkg_name) catch {};
      }
    }
  } else |_| {}

  std.Io.Dir.cwd().createDirPath(io, temp_nm_path) catch {};

  const pkg_json_content = std.fmt.allocPrint(arena_alloc, 
    \\{{"dependencies":{{"{s}":"{s}"}}}}
  , .{pkg_name, version_constraint}) catch return .out_of_memory;

  const pkg_json_file = std.Io.Dir.cwd().createFile(io, temp_pkg_json, .{}) catch {
    c.setError("Failed to create temp package.json");
    return .io_error;
  };
  pkg_json_file.writeStreamingAll(io, pkg_json_content) catch {
    pkg_json_file.close(io);
    c.setError("Failed to write temp package.json");
    return .io_error;
  };
  pkg_json_file.close(io);

  const db = c.cache_db orelse return .cache_error;
  const configured_registry = if (c.options.registry_url) |url| std.mem.span(url) else "https://registry.npmjs.org";
  var effective_registry = configured_registry;
  var fallback_http: ?*fetcher.Fetcher = null;
  defer if (fallback_http) |h| h.deinit();

  var http = c.http orelse return .network_error;
  if (isAntsLandRegistry(configured_registry)) {
    var specs = [_][*:0]const u8{package_spec};
    const cache_dir_z = std.fmt.allocPrintSentinel(arena_alloc, "{s}", .{c.cache_dir}, 0) catch return .out_of_memory;
    const choice = pkg_choose_registry_many(
      &specs, 1,
      cache_dir_z,
      "npm.ants.land",
      "registry.npmjs.org",
    );
    if (choice == .fallback) {
      effective_registry = "registry.npmjs.org";
      fallback_http = fetcher.Fetcher.init(c.allocator, effective_registry) catch return .network_error;
      http = fallback_http orelse unreachable;
      debug.log("exec: using npm registry fallback for {s}", .{pkg_name});
    }
  }

  var interleaved = InterleavedContext.init(c.allocator, arena_alloc, db, http, c);
  defer interleaved.deinit();

  var res = resolver.Resolver.init(
    arena_alloc,
    c.allocator,
    &c.string_pool,
    http,
    db,
    effective_registry,
    &c.metadata_cache,
  ); defer res.deinit();
  res.force_refresh = c.options.force;

  res.setOnPackageResolved(InterleavedContext.onPackageResolved, &interleaved);
  res.resolveFromPackageJson(temp_pkg_json) catch |err| {
    setResolveError(c, pkg_name, err);
    return .resolve_error;
  };

  debug.log("exec: resolved {d} packages, queued {d} tarballs", .{
    interleaved.callbacks_received, interleaved.tarballs_queued,
  });

  http.run() catch {};

  var pkg_linker = linker.Linker.init(c.allocator);
  defer pkg_linker.deinit();

  pkg_linker.setNodeModulesPath(temp_nm_dir) catch |err| {
    c.setErrorFmt("Failed to set up exec directory: {}", .{err});
    return .io_error;
  };

  for (interleaved.extract_contexts.items) |ectx| {
    defer ectx.ext.deinit();
    if (ectx.has_error or !ectx.completed or !ectx.ext.isComplete()) continue;

    const stats = ectx.ext.stats();
    db.insert(&.{
      .integrity = ectx.integrity,
      .path = ectx.cache_path,
      .unpacked_size = stats.bytes,
      .file_count = stats.files,
      .cached_at = std.Io.Timestamp.now(io, .real).toSeconds(),
    }, ectx.pkg_name, ectx.version_str) catch continue;

    pkg_linker.linkPackage(.{
      .cache_path = ectx.cache_path,
      .node_modules_path = temp_nm_dir,
      .name = ectx.pkg_name,
      .parent_path = ectx.parent_path,
      .file_count = stats.files,
      .has_bin = ectx.has_bin,
      .bins = ectx.bins,
    }) catch continue;
  }

  var resolved_iter = res.resolved.valueIterator();
  while (resolved_iter.next()) |pkg_ptr| {
    const pkg = pkg_ptr.*;
    if (db.lookup(&pkg.integrity)) |cache_entry| {
      var entry = cache_entry;
      defer entry.deinit();
      const pkg_cache_path = arena_alloc.dupe(u8, entry.path) catch continue;
      pkg_linker.linkPackage(.{
        .cache_path = pkg_cache_path,
        .node_modules_path = temp_nm_dir,
        .name = pkg.name.slice(),
        .parent_path = pkg.parent_path,
        .file_count = entry.file_count,
        .has_bin = pkg.has_bin,
        .bins = resolvedPackageBins(arena_alloc, pkg) catch &[_]linker.PackageBin{},
      }) catch continue;
    }
  }

  populateResolvedFileCountsFromCache(&res, db);
  _ = res.writeLockfile(temp_lockfile) catch {};

  var trusted = std.StringHashMap(void).init(arena_alloc);
  var resolved_iter2 = res.resolved.valueIterator();
  while (resolved_iter2.next()) |pkg_ptr| {
    trusted.put(pkg_ptr.*.name.slice(), {}) catch continue;
  }
  if (!runTrustedPostinstall(c, &trusted, temp_nm_dir, arena_alloc)) return .io_error;

  const selected_bin = selectPackageBinName(c, arena_alloc, temp_nm_dir, pkg_name);
  if (selected_bin.err != .ok) return selected_bin.err;
  bin_name = selected_bin.name;

  var bin_path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const bin_link_path = std.fmt.bufPrint(&bin_path_buf, "{s}/.bin/{s}", .{ temp_nm_dir, bin_name }) catch return .io_error;

  debug.log("exec: looking for bin at {s}", .{bin_link_path});

  std.Io.Dir.cwd().access(io, bin_link_path, .{}) catch {
    c.setErrorFmt("Binary '{s}' not found in package", .{bin_name});
    return .not_found;
  };

  var real_path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
  const real_path_len = std.Io.Dir.cwd().realPathFile(io, bin_link_path, &real_path_buf) catch return .io_error;

  if (real_path_len >= out_bin_path_len) return .io_error;

  @memcpy(out_bin_path[0..real_path_len], real_path_buf[0..real_path_len]);
  out_bin_path[real_path_len] = 0;

  return .ok;
}

fn getGlobalDir(allocator: std.mem.Allocator) ![]const u8 {
  if (builtin.os.tag != .windows) {
    if (try getLegacyAntDirIfExists(allocator)) |dir| {
      defer allocator.free(dir);
      return std.fmt.allocPrint(allocator, "{s}/pkg/global", .{dir});
    }
    if (getAbsoluteEnv("XDG_DATA_HOME")) |base| {
      return std.fmt.allocPrint(allocator, "{s}/ant/pkg/global", .{base});
    }
    const home = try getHomeDir(allocator);
    defer allocator.free(home);
    return std.fmt.allocPrint(allocator, "{s}/.local/share/ant/pkg/global", .{home});
  }

  const home = try getHomeDir(allocator);
  defer allocator.free(home);
  return std.fmt.allocPrint(allocator, "{s}/.ant/pkg/global", .{home});
}

fn getGlobalBinDir(allocator: std.mem.Allocator) ![]const u8 {
  const home = try getHomeDir(allocator);
  defer allocator.free(home);
  if (builtin.os.tag != .windows) {
    if (try getLegacyAntDirIfExists(allocator)) |dir| {
      defer allocator.free(dir);
      return std.fmt.allocPrint(allocator, "{s}/bin", .{dir});
    }
    return std.fmt.allocPrint(allocator, "{s}/.local/bin", .{home});
  }
  return std.fmt.allocPrint(allocator, "{s}/.ant/bin", .{home});
}

fn ensureGlobalPackageJson(allocator: std.mem.Allocator, global_dir: []const u8) !void {
  const pkg_json_path = try std.fmt.allocPrint(allocator, "{s}/package.json", .{global_dir});
  defer allocator.free(pkg_json_path);
  
  std.Io.Dir.cwd().access(io, pkg_json_path, .{}) catch {
    std.Io.Dir.cwd().createDirPath(io, global_dir) catch {};
    const file = try std.Io.Dir.cwd().createFile(io, pkg_json_path, .{});
    defer file.close(io);
    try file.writeStreamingAll(io, "{\"dependencies\":{}}\n");
  };
}

fn linkGlobalBins(allocator: std.mem.Allocator, nm_path: []const u8, pkg_name: []const u8) void {
  const bin_dir = getGlobalBinDir(allocator) catch return;
  defer allocator.free(bin_dir);
  
  std.Io.Dir.cwd().createDirPath(io, bin_dir) catch return;
  
  const pkg_bin_dir = std.fmt.allocPrint(allocator, "{s}/{s}", .{nm_path, pkg_name}) catch return;
  defer allocator.free(pkg_bin_dir);
  
  const pkg_json_path = std.fmt.allocPrint(allocator, "{s}/package.json", .{pkg_bin_dir}) catch return;
  defer allocator.free(pkg_json_path);
  
  const content = std.Io.Dir.cwd().readFileAlloc(io, pkg_json_path, allocator, .limited(1024 * 1024)) catch return;
  defer allocator.free(content);
  
  const parsed = std.json.parseFromSlice(std.json.Value, allocator, content, .{}) catch return;
  defer parsed.deinit();
  
  const bin_val = parsed.value.object.get("bin") orelse return;
  
  switch (bin_val) {
    .string => |s| {
      const base_name = if (std.mem.lastIndexOfScalar(u8, pkg_name, '/')) |idx|
        pkg_name[idx + 1..] else pkg_name;
      linkSingleBin(allocator, bin_dir, nm_path, pkg_name, base_name, s);
    },
    .object => |obj| {
      for (obj.keys(), obj.values()) |bin_name, path_val| {
        if (path_val == .string) linkSingleBin(allocator, bin_dir, nm_path, pkg_name, bin_name, path_val.string);
      }
    },
    else => {},
  }
}

fn linkSingleBin(allocator: std.mem.Allocator, bin_dir: []const u8, nm_path: []const u8, pkg_name: []const u8, bin_name: []const u8, bin_rel_path: []const u8) void {
  const target = std.fmt.allocPrint(allocator, "{s}/{s}/{s}", .{nm_path, pkg_name, bin_rel_path}) catch return;
  defer allocator.free(target);
  
  const link_path = std.fmt.allocPrint(allocator, "{s}/{s}", .{bin_dir, bin_name}) catch return;
  defer allocator.free(link_path);
  
  std.Io.Dir.cwd().deleteFile(io, link_path) catch {};
  linker.createSymlinkAbsolute(target, link_path);
  
  debug.log("linked global bin: {s} -> {s}", .{link_path, target});
}

fn unlinkGlobalBins(allocator: std.mem.Allocator, pkg_name: []const u8) void {
  const bin_dir = getGlobalBinDir(allocator) catch return;
  defer allocator.free(bin_dir);
  
  var dir = std.Io.Dir.cwd().openDir(io, bin_dir, .{ .iterate = true }) catch return;
  defer dir.close(io);
  
  var iter = dir.iterate();
  while (iter.next(io) catch null) |entry| {
    if (entry.kind != .sym_link) continue;
    
    var target_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const target_len = dir.readLink(io, entry.name, &target_buf) catch continue;
    const target = target_buf[0..target_len];

    const pattern = std.fmt.allocPrint(allocator, "/{s}/", .{pkg_name}) catch continue;
    defer allocator.free(pattern);
    const pattern_end = std.fmt.allocPrint(allocator, "/{s}", .{pkg_name}) catch continue;
    defer allocator.free(pattern_end);

    if (std.mem.indexOf(u8, target, pattern) != null or std.mem.endsWith(u8, target, pattern_end)) {
      dir.deleteFile(io, entry.name) catch continue;
      debug.log("unlinked global bin: {s}", .{entry.name});
    }
  }
}

export fn pkg_add_global(
  ctx: ?*PkgContext,
  package_spec: [*:0]const u8,
) PkgError {
  const specs = [_][*:0]const u8{package_spec};
  return pkg_add_global_many(ctx, &specs, 1);
}

export fn pkg_add_global_many(
  ctx: ?*PkgContext,
  package_specs: [*]const [*:0]const u8,
  count: u32,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  const allocator = c.allocator;

  const global_dir = getGlobalDir(allocator) catch {
    c.setError("HOME not set");
    return .invalid_argument;
  };
  defer allocator.free(global_dir);
  
  ensureGlobalPackageJson(allocator, global_dir) catch {
    c.setError("Failed to create global package.json");
    return .io_error;
  };
  
  const pkg_json_path = std.fmt.allocPrintSentinel(allocator, "{s}/package.json", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(pkg_json_path);
  const lockfile_path = std.fmt.allocPrintSentinel(allocator, "{s}/ant.lockb", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(lockfile_path);
  const nm_path = std.fmt.allocPrintSentinel(allocator, "{s}/node_modules", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(nm_path);
  
  const add_result = pkg_add_many(c, pkg_json_path.ptr, package_specs, count, false);
  if (add_result != .ok) return add_result;
  
  const install_result = pkg_resolve_and_install(c, pkg_json_path.ptr, lockfile_path.ptr, nm_path.ptr);
  if (install_result != .ok) return install_result;
  
  for (0..count) |i| {
    const spec_str = std.mem.span(package_specs[i]);
    var pkg_name: []const u8 = spec_str;
    
    if (std.mem.indexOf(u8, spec_str, "@")) |at_idx| {
      if (at_idx == 0) {
        if (std.mem.indexOfPos(u8, spec_str, 1, "@")) |second_at| pkg_name = spec_str[0..second_at];
      } else pkg_name = spec_str[0..at_idx];
    }
    
    linkGlobalBins(allocator, nm_path, pkg_name);
  }
  
  return .ok;
}

export fn pkg_remove_global(
  ctx: ?*PkgContext,
  package_name: [*:0]const u8,
) PkgError {
  const c = ctx orelse return .invalid_argument;
  const allocator = c.allocator;

  const global_dir = getGlobalDir(allocator) catch {
    c.setError("HOME not set");
    return .invalid_argument;
  };
  defer allocator.free(global_dir);
  
  const pkg_json_path = std.fmt.allocPrintSentinel(allocator, "{s}/package.json", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(pkg_json_path);
  const lockfile_path = std.fmt.allocPrintSentinel(allocator, "{s}/ant.lockb", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(lockfile_path);
  const nm_path = std.fmt.allocPrintSentinel(allocator, "{s}/node_modules", .{global_dir}, 0) catch return .out_of_memory;
  defer allocator.free(nm_path);
  
  const name_str = std.mem.span(package_name);
  
  unlinkGlobalBins(allocator, name_str);
  
  const remove_result = pkg_remove(c, pkg_json_path.ptr, package_name);
  if (remove_result != .ok and remove_result != .not_found) return remove_result;
  if (remove_result == .not_found) return .not_found;
  
  const install_result = pkg_resolve_and_install(c, pkg_json_path.ptr, lockfile_path.ptr, nm_path.ptr);
  if (install_result != .ok) return install_result;
  
  return .ok;
}

export fn pkg_count_local(ctx: ?*PkgContext) u32 {
  var pd = cli.get_dependencies(ctx, null, true) orelse return 0;
  defer pd.deinit(); return pd.count();
}

export fn pkg_count_global(ctx: ?*PkgContext) u32 {
  const global_dir = getGlobalDir(global_allocator) catch return 0;
  var pd = cli.get_dependencies(ctx, global_dir, false) orelse return 0;
  defer pd.deinit(); return pd.count();
}

export fn pkg_list_local(
  ctx: ?*PkgContext,
  callback: ?*const fn (name: [*:0]const u8, version: [*:0]const u8, user_data: ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) PkgError {
  var pd = cli.get_dependencies(ctx, null, true) orelse return .ok;
  defer pd.deinit();
  if (callback) |cb| cli.list_dependencies(&pd, cb, user_data);
  return .ok;
}

export fn pkg_list_global(
  ctx: ?*PkgContext,
  callback: ?*const fn (name: [*:0]const u8, version: [*:0]const u8, user_data: ?*anyopaque) callconv(.c) void,
  user_data: ?*anyopaque,
) PkgError {
  const global_dir = getGlobalDir(global_allocator) catch return .invalid_argument;
  var pd = cli.get_dependencies(ctx, global_dir, false) orelse return .ok;
  defer pd.deinit();
  if (callback) |cb| cli.list_dependencies(&pd, cb, user_data);
  return .ok;
}
