const std = @import("std");
const builtin = @import("builtin");
const lockfile = @import("lockfile.zig");
const intern = @import("intern.zig");
const fetcher = @import("fetcher.zig");
const json = @import("json.zig");
const debug = @import("debug.zig");
const cache = @import("cache.zig");

pub const ResolveError = error{
  InvalidPackageJson,
  NetworkError,
  NoMatchingVersion,
  CyclicDependency,
  OutOfMemory,
  ParseError,
  IoError,
};

pub const Version = struct {
  major: u64,
  minor: u64,
  patch: u64,
  prerelease: ?[]const u8,
  build: ?[]const u8,

  pub fn parse(str: []const u8) !Version {
    var remaining = str;

    if (remaining.len > 0 and remaining[0] == 'v') {
      remaining = remaining[1..];
    }

    const major_end = std.mem.indexOfScalar(u8, remaining, '.') orelse return error.InvalidVersion;
    const major = try std.fmt.parseInt(u64, remaining[0..major_end], 10);
    remaining = remaining[major_end + 1 ..];

    const minor_end = std.mem.indexOfScalar(u8, remaining, '.') orelse return error.InvalidVersion;
    const minor = try std.fmt.parseInt(u64, remaining[0..minor_end], 10);
    remaining = remaining[minor_end + 1 ..];

    var patch_end = remaining.len;
    var prerelease: ?[]const u8 = null;
    var build: ?[]const u8 = null;

    if (std.mem.indexOfScalar(u8, remaining, '-')) |dash| {
      patch_end = dash;
      const after_patch = remaining[dash + 1 ..];
      if (std.mem.indexOfScalar(u8, after_patch, '+')) |plus| {
        prerelease = after_patch[0..plus];
        build = after_patch[plus + 1 ..];
      } else prerelease = after_patch;
    } else if (std.mem.indexOfScalar(u8, remaining, '+')) |plus| {
      patch_end = plus;
      build = remaining[plus + 1 ..];
    }

    const patch = try std.fmt.parseInt(u64, remaining[0..patch_end], 10);

    return .{
      .major = major,
      .minor = minor,
      .patch = patch,
      .prerelease = prerelease,
      .build = build,
    };
  }

  pub fn order(a: Version, b: Version) std.math.Order {
    if (a.major != b.major) return std.math.order(a.major, b.major);
    if (a.minor != b.minor) return std.math.order(a.minor, b.minor);
    if (a.patch != b.patch) return std.math.order(a.patch, b.patch);

    if (a.prerelease == null and b.prerelease != null) return .gt;
    if (a.prerelease != null and b.prerelease == null) return .lt;

    return .eq;
  }

  pub fn format(self: Version, allocator: std.mem.Allocator) ![]u8 {
    if (self.prerelease) |pre| {
      return std.fmt.allocPrint(allocator, "{d}.{d}.{d}-{s}", .{
        self.major, self.minor, self.patch, pre,
      });
    }
    return std.fmt.allocPrint(allocator, "{d}.{d}.{d}", .{
      self.major, self.minor, self.patch,
    });
  }
};

pub const Constraint = struct {
  kind: Kind,
  version: Version,

  pub const Kind = enum {
    exact, // 1.2.3
    caret, // ^1.2.3 (>=1.2.3 <2.0.0)
    tilde, // ~1.2.3 (>=1.2.3 <1.3.0)
    gte, // >=1.2.3
    gt, // >1.2.3
    lte, // <=1.2.3
    lt, // <1.2.3
    any, // *
  };

  pub fn parse(str: []const u8) !Constraint {
    if (str.len == 0 or std.mem.eql(u8, str, "*") or std.mem.eql(u8, str, "latest")) {
      return .{ .kind = .any, .version = .{ .major = 0, .minor = 0, .patch = 0, .prerelease = null, .build = null } };
    }

    var remaining = str;
    var kind: Kind = .exact;

    if (std.mem.lastIndexOf(u8, remaining, "||")) |or_idx| {
      remaining = std.mem.trim(u8, remaining[or_idx + 2 ..], " ");
    }

    if (std.mem.indexOf(u8, remaining, " ")) |space| {
      remaining = remaining[0..space];
    }

    if (std.mem.startsWith(u8, remaining, "^")) {
      kind = .caret;
      remaining = remaining[1..];
    } else if (std.mem.startsWith(u8, remaining, "~")) {
      kind = .tilde;
      remaining = remaining[1..];
    } else if (std.mem.startsWith(u8, remaining, ">=")) {
      kind = .gte;
      remaining = remaining[2..];
    } else if (std.mem.startsWith(u8, remaining, ">")) {
      kind = .gt;
      remaining = remaining[1..];
    } else if (std.mem.startsWith(u8, remaining, "<=")) {
      kind = .lte;
      remaining = remaining[2..];
    } else if (std.mem.startsWith(u8, remaining, "<")) {
      kind = .lt;
      remaining = remaining[1..];
    } else if (std.mem.startsWith(u8, remaining, "=")) {
      remaining = remaining[1..];
    }

    const dot_count = std.mem.count(u8, remaining, ".");
    if (dot_count == 0) {
      const major = std.fmt.parseInt(u64, remaining, 10) catch return .{
        .kind = .any,
        .version = .{ .major = 0, .minor = 0, .patch = 0, .prerelease = null, .build = null },
      };
      return .{
        .kind = if (kind == .exact) .caret else kind,
        .version = .{ .major = major, .minor = 0, .patch = 0, .prerelease = null, .build = null },
      };
    } else if (dot_count == 1) {
      var parts = std.mem.splitScalar(u8, remaining, '.');
      const major = std.fmt.parseInt(u64, parts.next().?, 10) catch 0;
      const minor = std.fmt.parseInt(u64, parts.next().?, 10) catch 0;
      return .{
        .kind = if (kind == .exact) .tilde else kind,
        .version = .{ .major = major, .minor = minor, .patch = 0, .prerelease = null, .build = null },
      };
    }

    const version = try Version.parse(remaining);
    return .{ .kind = kind, .version = version };
  }

  pub fn satisfies(self: Constraint, v: Version) bool {
    switch (self.kind) {
      .any => return true,
      .exact => return v.major == self.version.major and
        v.minor == self.version.minor and
        v.patch == self.version.patch,
      .caret => {
        // ^1.2.3 means >=1.2.3 <2.0.0 (for major > 0)
        // ^0.2.3 means >=0.2.3 <0.3.0 (for major = 0)
        // ^0.0.3 means >=0.0.3 <0.0.4 (for major = 0, minor = 0)
        if (v.order(self.version) == .lt) return false;
        if (self.version.major > 0) {
          return v.major == self.version.major;
        } else if (self.version.minor > 0) {
          return v.major == 0 and v.minor == self.version.minor;
        } else {
          return v.major == 0 and v.minor == 0 and v.patch == self.version.patch;
        }
      },
      .tilde => {
        // ~1.2.3 means >=1.2.3 <1.3.0
        if (v.order(self.version) == .lt) return false;
        return v.major == self.version.major and v.minor == self.version.minor;
      },
      .gte => return v.order(self.version) != .lt,
      .gt => return v.order(self.version) == .gt,
      .lte => return v.order(self.version) != .gt,
      .lt => return v.order(self.version) == .lt,
    }
  }
};

pub const VersionInfo = struct {
  version: Version,
  version_str: []const u8,
  integrity: [64]u8,
  tarball_url: []const u8,
  dependencies: std.StringHashMap([]const u8),
  optional_dependencies: std.StringHashMap([]const u8),
  peer_dependencies: std.StringHashMap([]const u8),
  peer_dependencies_meta: std.StringHashMap(bool),
  os: ?[]const u8, cpu: ?[]const u8,
  allocator: std.mem.Allocator,

  pub fn deinit(self: *VersionInfo) void {
    self.allocator.free(self.version_str);
    self.allocator.free(self.tarball_url);
    var iter = self.dependencies.iterator();
    while (iter.next()) |entry| {
      self.allocator.free(entry.key_ptr.*);
      self.allocator.free(entry.value_ptr.*);
    }
    self.dependencies.deinit();
    var opt_iter = self.optional_dependencies.iterator();
    while (opt_iter.next()) |entry| {
      self.allocator.free(entry.key_ptr.*);
      self.allocator.free(entry.value_ptr.*);
    }
    self.optional_dependencies.deinit();
    var peer_iter = self.peer_dependencies.iterator();
    while (peer_iter.next()) |entry| {
      self.allocator.free(entry.key_ptr.*);
      self.allocator.free(entry.value_ptr.*);
    }
    self.peer_dependencies.deinit();
    self.peer_dependencies_meta.deinit();
    if (self.os) |o| self.allocator.free(o);
    if (self.cpu) |c| self.allocator.free(c);
  }

  pub fn matchesPlatform(self: *const VersionInfo) bool {
    const current_os = comptime switch (builtin.os.tag) {
      .macos => "darwin",
      .linux => "linux",
      .windows => "win32",
      .freebsd => "freebsd",
      else => "unknown",
    };
    const current_cpu = comptime switch (builtin.cpu.arch) {
      .aarch64 => "arm64",
      .x86_64 => "x64",
      .x86 => "ia32",
      .arm => "arm",
      else => "unknown",
    };

    if (self.os) |os_filter| {
      if (!matchesFilter(os_filter, current_os)) return false;
    }

    if (self.cpu) |cpu_filter| {
      if (!matchesFilter(cpu_filter, current_cpu)) return false;
    }

    return true;
  }

  fn matchesFilter(filter: []const u8, value: []const u8) bool {
    var has_positive = false;
    var matches = false;

    var iter = std.mem.splitScalar(u8, filter, ',');
    while (iter.next()) |part| {
      const trimmed = std.mem.trim(u8, part, " ");
      if (trimmed.len == 0) continue;

      if (trimmed[0] == '!') {
        if (std.mem.eql(u8, trimmed[1..], value)) return false;
      } else {
        has_positive = true;
        if (std.mem.eql(u8, trimmed, value)) matches = true;
      }
    }

    return if (has_positive) matches else true;
  }
};

 fn parseDepsMap(
  allocator: std.mem.Allocator,
  maybe_obj: ?std.json.Value,
) std.StringHashMap([]const u8) {
  var map = std.StringHashMap([]const u8).init(allocator);

  const deps_obj = maybe_obj orelse return map;
  if (deps_obj != .object) return map;

  for (deps_obj.object.keys(), deps_obj.object.values()) |dep_name, dep_ver| {
    if (dep_ver != .string) continue;
    
    const key = allocator.dupe(u8, dep_name) catch continue;
    const val = allocator.dupe(u8, dep_ver.string) catch {
      allocator.free(key);
      continue;
    };
    
    map.put(key, val) catch {
    allocator.free(key);
    allocator.free(val);
    };
  }
  
  return map;
}

fn parsePeerMeta(
  allocator: std.mem.Allocator,
  maybe_obj: ?std.json.Value,
) std.StringHashMap(bool) {
  var map = std.StringHashMap(bool).init(allocator);
    
  const meta_obj = maybe_obj orelse return map;
  if (meta_obj != .object) return map;
  
  for (meta_obj.object.keys(), meta_obj.object.values()) |dep_name, meta_val| {
    if (meta_val != .object) continue;
    
    const is_optional = if (meta_val.object.get("optional")) |opt| (opt == .bool and opt.bool)
    else false;
    
    if (is_optional) {
      const key = allocator.dupe(u8, dep_name) catch continue;
      map.put(key, true) catch allocator.free(key);
    }
  }
  return map;
}

pub const PackageMetadata = struct {
  allocator: std.mem.Allocator,
  name: []const u8,
  versions: std.ArrayListUnmanaged(VersionInfo),

  pub fn init(allocator: std.mem.Allocator, name: []const u8) !PackageMetadata {
    return .{
      .allocator = allocator,
      .name = try allocator.dupe(u8, name),
      .versions = .{},
    };
  }

  pub fn deinit(self: *PackageMetadata) void {
    for (self.versions.items) |*v| {
      v.deinit();
    }
    self.versions.deinit(self.allocator);
    self.allocator.free(self.name);
  }

  pub fn parseFromJson(allocator: std.mem.Allocator, json_data: []const u8) !PackageMetadata {
    const parsed = std.json.parseFromSlice(std.json.Value, allocator, json_data, .{}) catch {
      return error.ParseError;
    }; defer parsed.deinit();

    const root = parsed.value;
    if (root != .object) return error.ParseError;

    const name = if (root.object.get("name")) |n| switch (n) {
      .string => |s| s,
      else => return error.ParseError,
    } else return error.ParseError;

    var metadata = try PackageMetadata.init(allocator, name);
    errdefer metadata.deinit();

    const versions_obj = root.object.get("versions") orelse return metadata;
    if (versions_obj != .object) return metadata;

    for (versions_obj.object.keys(), versions_obj.object.values()) |version_str, version_data| {
      if (version_data != .object) continue;

      const version = Version.parse(version_str) catch continue;
      const dist = version_data.object.get("dist") orelse continue;
      if (dist != .object) continue;

      const tarball = if (dist.object.get("tarball")) |t| switch (t) {
        .string => |s| s,
        else => continue,
      } else continue;

      var integrity: [64]u8 = std.mem.zeroes([64]u8);
      if (dist.object.get("integrity")) |i| {
        if (i == .string) {
          const int_str = i.string;
          if (std.mem.startsWith(u8, int_str, "sha512-")) {
            const b64 = int_str[7..];
            _ = std.base64.standard.Decoder.decode(&integrity, b64) catch {};
          }
        }
      } else if (dist.object.get("shasum")) |s| {
        if (s == .string) {
          const hex = s.string;
          if (hex.len >= 40) {
            for (0..20) |i| integrity[i] = std.fmt.parseInt(u8, hex[i * 2 ..][0..2], 16) catch 0;
          }
        }
      }

      const deps = parseDepsMap(allocator, version_data.object.get("dependencies"));
      const opt_deps = parseDepsMap(allocator, version_data.object.get("optionalDependencies"));
      const peer_deps = parseDepsMap(allocator, version_data.object.get("peerDependencies"));
      const peer_meta = parsePeerMeta(allocator, version_data.object.get("peerDependenciesMeta"));

      var os_filter: ?[]const u8 = null;
      var cpu_filter: ?[]const u8 = null;

      if (version_data.object.get("os")) |os_arr| {
        if (os_arr == .array) {
          var os_buf = std.ArrayListUnmanaged(u8){};
          for (os_arr.array.items, 0..) |item, i| {
            if (item == .string) {
              if (i > 0) os_buf.append(allocator, ',') catch {};
              os_buf.appendSlice(allocator, item.string) catch {};
            }
          }
          if (os_buf.items.len > 0) {
            os_filter = os_buf.toOwnedSlice(allocator) catch null;
          } else os_buf.deinit(allocator);
        }
      }

      if (version_data.object.get("cpu")) |cpu_arr| {
        if (cpu_arr == .array) {
          var cpu_buf = std.ArrayListUnmanaged(u8){};
          for (cpu_arr.array.items, 0..) |item, i| {
            if (item == .string) {
              if (i > 0) cpu_buf.append(allocator, ',') catch {};
              cpu_buf.appendSlice(allocator, item.string) catch {};
            }
          }
          if (cpu_buf.items.len > 0) {
            cpu_filter = cpu_buf.toOwnedSlice(allocator) catch null;
          } else cpu_buf.deinit(allocator);
        }
      }

      try metadata.versions.append(allocator, .{
        .version = version,
        .version_str = try allocator.dupe(u8, version_str),
        .integrity = integrity,
        .tarball_url = try allocator.dupe(u8, tarball),
        .dependencies = deps,
        .optional_dependencies = opt_deps,
        .peer_dependencies = peer_deps,
        .peer_dependencies_meta = peer_meta,
        .os = os_filter,
        .cpu = cpu_filter,
        .allocator = allocator,
      });
    }

    return metadata;
  }
};

pub const ResolvedPackage = struct {
  name: intern.InternedString,
  version: Version,
  integrity: [64]u8,
  tarball_url: []const u8,
  dependencies: std.ArrayListUnmanaged(Dep),
  depth: u32,
  direct: bool,
  parent_path: ?[]const u8,
  allocator: std.mem.Allocator,

  pub const DepFlags = struct {
    peer: bool = false,
    dev: bool = false,
    optional: bool = false,
  };

  pub const Dep = struct {
    name: intern.InternedString,
    constraint: []const u8,
    flags: DepFlags = .{},
  };

  pub fn deinit(self: *ResolvedPackage) void {
    self.allocator.free(self.tarball_url);
    if (self.parent_path) |p| self.allocator.free(p);
    for (self.dependencies.items) |dep| {
      self.allocator.free(dep.constraint);
    }
    self.dependencies.deinit(self.allocator);
  }

  pub fn installPath(self: *const ResolvedPackage, allocator: std.mem.Allocator) ![]const u8 {
    if (self.parent_path) |parent| {
      return std.fmt.allocPrint(allocator, "{s}/node_modules/{s}", .{ parent, self.name.slice() });
    } return allocator.dupe(u8, self.name.slice());
  }
};


pub const OnPackageResolvedFn = *const fn (
  pkg: *const ResolvedPackage,
  user_data: ?*anyopaque
) void;

pub const Resolver = struct {
  allocator: std.mem.Allocator,
  cache_allocator: std.mem.Allocator,
  string_pool: *intern.StringPool,
  http: *fetcher.Fetcher,
  cache_db: ?*cache.CacheDB,
  resolved: std.StringHashMap(*ResolvedPackage),
  constraints: std.StringHashMap(std.ArrayListUnmanaged(Constraint)),
  in_progress: std.StringHashMap(void),
  registry_url: []const u8,
  metadata_cache: *std.StringHashMap(PackageMetadata),
  on_package_resolved: ?OnPackageResolvedFn,
  on_package_resolved_data: ?*anyopaque,

  pub fn init(
    allocator: std.mem.Allocator,
    cache_allocator: std.mem.Allocator,
    string_pool: *intern.StringPool,
    http: *fetcher.Fetcher,
    cache_db: ?*cache.CacheDB,
    registry_url: []const u8,
    metadata_cache: *std.StringHashMap(PackageMetadata),
  ) Resolver {
    return .{
      .allocator = allocator,
      .cache_allocator = cache_allocator,
      .string_pool = string_pool,
      .http = http,
      .cache_db = cache_db,
      .resolved = std.StringHashMap(*ResolvedPackage).init(allocator),
      .constraints = std.StringHashMap(std.ArrayListUnmanaged(Constraint)).init(allocator),
      .in_progress = std.StringHashMap(void).init(allocator),
      .registry_url = registry_url,
      .metadata_cache = metadata_cache,
      .on_package_resolved = null,
      .on_package_resolved_data = null,
    };
  }

  pub fn setOnPackageResolved(self: *Resolver, callback: OnPackageResolvedFn, user_data: ?*anyopaque) void {
    self.on_package_resolved = callback;
    self.on_package_resolved_data = user_data;
  }

  pub fn deinit(self: *Resolver) void {
    var key_iter = self.resolved.keyIterator();
    while (key_iter.next()) |key| {
      self.allocator.free(key.*);
    }
    
    var iter = self.resolved.valueIterator();
    while (iter.next()) |pkg| {
      pkg.*.deinit();
      self.allocator.destroy(pkg.*);
    } self.resolved.deinit();

    var cons_key_iter = self.constraints.keyIterator();
    while (cons_key_iter.next()) |key| {
      self.allocator.free(key.*);
    }
    
    var cons_iter = self.constraints.valueIterator();
    while (cons_iter.next()) |list| {
      list.deinit(self.allocator);
    }
    self.constraints.deinit();
    self.in_progress.deinit();
  }

  pub fn resolveFromPackageJson(self: *Resolver, path: []const u8) !void {
    const path_z = try self.allocator.dupeZ(u8, path);
    defer self.allocator.free(path_z);

    var pkg_json = try json.PackageJson.parse(self.allocator, path_z);
    defer pkg_json.deinit(self.allocator);

    debug.log("pass 1: collecting constraints", .{});
    var pass1_start: u64 = @intCast(std.time.nanoTimestamp());
    self.http.initiateTarballConnectionsAsync();

    const ConstraintInfo = struct {
      constraint: Constraint,
      constraint_str: []const u8,
      requester: []const u8,
      depth: u32,
    };

    var all_constraints = std.StringHashMap(std.ArrayListUnmanaged(ConstraintInfo)).init(self.allocator);
    defer {
      var iter = all_constraints.iterator();
      while (iter.next()) |entry| {
        for (entry.value_ptr.items) |info| {
          self.allocator.free(info.constraint_str);
          self.allocator.free(info.requester);
        }
        entry.value_ptr.deinit(self.allocator);
        self.allocator.free(entry.key_ptr.*);
      } all_constraints.deinit();
    }

    const CollectItem = struct {
      name: []const u8,
      constraint_str: []const u8,
      requester: []const u8,
      depth: u32,
    };

    var collect_queue = std.ArrayListUnmanaged(CollectItem){};
    defer collect_queue.deinit(self.allocator);

    var seen_collect = std.StringHashMap(void).init(self.allocator);
    defer {
      var key_iter = seen_collect.keyIterator();
      while (key_iter.next()) |k| self.allocator.free(k.*);
      seen_collect.deinit();
    }

    var dep_iter = pkg_json.dependencies.iterator();
    while (dep_iter.next()) |entry| {
      try collect_queue.append(self.allocator, .{
        .name = entry.key_ptr.*,
        .constraint_str = entry.value_ptr.*,
        .requester = "root",
        .depth = 0,
      });
    }
    
    var dev_iter = pkg_json.dev_dependencies.iterator();
    while (dev_iter.next()) |entry| {
      try collect_queue.append(self.allocator, .{
        .name = entry.key_ptr.*,
        .constraint_str = entry.value_ptr.*,
        .requester = "root",
        .depth = 0,
      });
    }

    var collect_level: u32 = 0;
    while (collect_queue.items.len > 0) {
      debug.log("  pass1 level {d}: {d} packages", .{ collect_level, collect_queue.items.len });

      var to_fetch = std.ArrayListUnmanaged([]const u8){};
      defer to_fetch.deinit(self.allocator);

      for (collect_queue.items) |item| {
        if (!self.metadata_cache.contains(item.name)) {
          var loaded_from_disk = false;
          if (self.cache_db) |db| {
            if (db.lookupMetadata(item.name, self.allocator)) |json_data| {
              const metadata = PackageMetadata.parseFromJson(self.cache_allocator, json_data) catch {
                self.allocator.free(json_data);
                continue;
              };
              self.allocator.free(json_data);
              const cache_key = self.cache_allocator.dupe(u8, item.name) catch continue;
              self.metadata_cache.put(cache_key, metadata) catch {
                self.cache_allocator.free(cache_key);
                continue;
              };
              loaded_from_disk = true;
            }
          }
          if (!loaded_from_disk) {
            var already_listed = false;
            for (to_fetch.items) |f| {
              if (std.mem.eql(u8, f, item.name)) {
                already_listed = true;
                break;
              }
            }
            if (!already_listed) try to_fetch.append(self.allocator, item.name);
          }
        }
      }

      const StreamContext = struct {
        resolver: *Resolver,
        prefetch_queue: *std.ArrayListUnmanaged([]const u8),
        collect_queue_items: []const CollectItem,
        allocator: std.mem.Allocator,

        fn onMetadata(name: []const u8, data: ?[]const u8, has_error: bool, userdata: ?*anyopaque) void {
          const ctx: *@This() = @ptrCast(@alignCast(userdata));
          if (has_error or data == null) return;

          if (ctx.resolver.cache_db) |db| {
            db.insertMetadata(name, data.?) catch {};
          }

          const metadata = PackageMetadata.parseFromJson(ctx.resolver.cache_allocator, data.?) catch return;
          const cache_key = ctx.resolver.cache_allocator.dupe(u8, name) catch return;
          ctx.resolver.metadata_cache.put(cache_key, metadata) catch {
            ctx.resolver.cache_allocator.free(cache_key);
            return;
          };

          for (ctx.collect_queue_items) |item| {
            if (!std.mem.eql(u8, item.name, name)) continue;

            const constraint = Constraint.parse(item.constraint_str) catch continue;
            const best = ctx.resolver.selectBestVersion(&metadata, constraint) orelse continue;
            if (!best.matchesPlatform()) continue;

            var dep_it = best.dependencies.iterator();
            while (dep_it.next()) |entry| {
              const dep_name = entry.key_ptr.*;
              if (ctx.resolver.metadata_cache.contains(dep_name)) continue;

              var already_queued = false;
              for (ctx.prefetch_queue.items) |q| {
                if (std.mem.eql(u8, q, dep_name)) {
                  already_queued = true;  break;
                }
              }
              if (!already_queued) ctx.prefetch_queue.append(ctx.allocator, dep_name) catch {};
            } break;
          }
        }
      };

      var next_collect = std.ArrayListUnmanaged(CollectItem){};
      errdefer next_collect.deinit(self.allocator);

      var prefetch_queue = std.ArrayListUnmanaged([]const u8){};
      defer prefetch_queue.deinit(self.allocator);

      if (to_fetch.items.len > 0) {
        var stream_ctx = StreamContext{
          .resolver = self,
          .prefetch_queue = &prefetch_queue,
          .collect_queue_items = collect_queue.items,
          .allocator = self.allocator,
        };

        try self.http.fetchMetadataStreaming(
          to_fetch.items,
          self.allocator,
          StreamContext.onMetadata,
          &stream_ctx,
        );

        if (prefetch_queue.items.len > 0) {
          debug.log("  prefetch: queued {d} next-level packages", .{prefetch_queue.items.len});
          self.http.fetchMetadataStreaming(
            prefetch_queue.items,
            self.allocator,
            StreamContext.onMetadata,
            &stream_ctx,
          ) catch {};
        }
      }

      for (collect_queue.items) |item| {
        const seen_key = std.fmt.allocPrint(self.allocator, "{s}@{s}@{s}", .{ item.name, item.constraint_str, item.requester }) catch continue;
        if (seen_collect.contains(seen_key)) {
          self.allocator.free(seen_key);
          continue;
        }
        try seen_collect.put(seen_key, {});

        const constraint = Constraint.parse(item.constraint_str) catch continue;
        const gop = try all_constraints.getOrPut(item.name);
        if (!gop.found_existing) {
          gop.key_ptr.* = try self.allocator.dupe(u8, item.name);
          gop.value_ptr.* = .{};
        }
        try gop.value_ptr.append(self.allocator, .{
          .constraint = constraint,
          .constraint_str = try self.allocator.dupe(u8, item.constraint_str),
          .requester = try self.allocator.dupe(u8, item.requester),
          .depth = item.depth,
        });

        if (self.metadata_cache.get(item.name)) |metadata| {
          const best = self.selectBestVersion(&metadata, constraint) orelse continue;
          if (!best.matchesPlatform()) continue;

          var dep_it = best.dependencies.iterator();
          while (dep_it.next()) |entry| {
            try next_collect.append(self.allocator, .{
              .name = entry.key_ptr.*,
              .constraint_str = entry.value_ptr.*,
              .requester = item.name,
              .depth = item.depth + 1,
            });
          }

          var opt_it = best.optional_dependencies.iterator();
          while (opt_it.next()) |entry| {
            if (self.metadata_cache.get(entry.key_ptr.*)) |opt_meta| {
              const opt_con = Constraint.parse(entry.value_ptr.*) catch continue;
              const opt_best = self.selectBestVersion(&opt_meta, opt_con) orelse continue;
              if (!opt_best.matchesPlatform()) continue;
            }
            try next_collect.append(self.allocator, .{
              .name = entry.key_ptr.*,
              .constraint_str = entry.value_ptr.*,
              .requester = item.name,
              .depth = item.depth + 1,
            });
          }

          var peer_it = best.peer_dependencies.iterator();
          while (peer_it.next()) |entry| {
            if (best.peer_dependencies_meta.contains(entry.key_ptr.*)) continue;
            try next_collect.append(self.allocator, .{
              .name = entry.key_ptr.*,
              .constraint_str = entry.value_ptr.*,
              .requester = item.name,
              .depth = item.depth + 1,
            });
          }
        }
      }

      collect_queue.deinit(self.allocator);
      collect_queue = next_collect;
      collect_level += 1;
    }

    pass1_start = debug.timer("pass 1 complete", pass1_start);
    debug.log("  collected constraints for {d} packages", .{all_constraints.count()});
    debug.log("computing optimal versions", .{});

    var optimal_versions = std.StringHashMap(*const VersionInfo).init(self.allocator);
    defer optimal_versions.deinit();

    var pkg_iter = all_constraints.iterator();
    while (pkg_iter.next()) |entry| {
      const pkg_name = entry.key_ptr.*;
      const constraint_list = entry.value_ptr.items;

      if (self.metadata_cache.get(pkg_name)) |metadata| {
        var plain_constraints = try self.allocator.alloc(Constraint, constraint_list.len);
        defer self.allocator.free(plain_constraints);
        for (constraint_list, 0..) |info, i| {
          plain_constraints[i] = info.constraint;
        }

        if (self.selectBestVersionForConstraints(&metadata, plain_constraints)) |best| {
          try optimal_versions.put(pkg_name, best);
        } else {
          const best = self.selectVersionSatisfyingMost(&metadata, constraint_list);
          if (best) |b| {
            debug.log("  {s}: optimal={d}.{d}.{d} (satisfies {d}/{d} constraints)", .{
              pkg_name,
              b.version.major,
              b.version.minor,
              b.version.patch,
              self.countSatisfied(&metadata, b, constraint_list),
              constraint_list.len,
            });
            try optimal_versions.put(pkg_name, b);
          }
        }
      }
    }

    pass1_start = debug.timer("optimal versions computed", pass1_start);
    debug.log("pass 2: resolving with optimal versions", .{});

    const WorkItem = struct {
      name: []const u8,
      constraint: []const u8,
      depth: u32,
      direct: bool,
      parent_name: ?[]const u8,
    };

    var queue = std.ArrayListUnmanaged(WorkItem){};
    defer queue.deinit(self.allocator);

    dep_iter = pkg_json.dependencies.iterator();
    while (dep_iter.next()) |entry| {
      try queue.append(self.allocator, .{
        .name = entry.key_ptr.*,
        .constraint = entry.value_ptr.*,
        .depth = 0,
        .direct = true,
        .parent_name = null,
      });
    }
    dev_iter = pkg_json.dev_dependencies.iterator();
    while (dev_iter.next()) |entry| {
      try queue.append(self.allocator, .{
        .name = entry.key_ptr.*,
        .constraint = entry.value_ptr.*,
        .depth = 0,
        .direct = true,
        .parent_name = null,
      });
    }

    var processed = std.StringHashMap(void).init(self.allocator);
    defer {
      var key_iter = processed.keyIterator();
      while (key_iter.next()) |k| self.allocator.free(k.*);
      processed.deinit();
    }

    var level: u32 = 0;
    while (queue.items.len > 0) {
      const level_start: u64 = @intCast(std.time.nanoTimestamp());
      debug.log("  pass2 level {d}: {d} packages", .{ level, queue.items.len });

      var next_queue = std.ArrayListUnmanaged(WorkItem){};
      errdefer next_queue.deinit(self.allocator);

      for (queue.items) |item| {
        const key = std.fmt.allocPrint(self.allocator, "{s}@{s}", .{ item.name, item.constraint }) catch continue;
        if (processed.contains(key)) {
          self.allocator.free(key);
          continue;
        }
        try processed.put(key, {});

        const pkg = self.resolveSingleWithOptimal(item.name, item.constraint, item.depth, item.direct, item.parent_name, &optimal_versions) catch |err| {
          debug.log("  failed to resolve {s}: {}", .{ item.name, err });
          continue;
        };

        const pkg_install_path = pkg.installPath(self.allocator) catch continue;
        defer self.allocator.free(pkg_install_path);

        for (pkg.dependencies.items) |dep| {
          const dep_key = std.fmt.allocPrint(self.allocator, "{s}@{s}", .{ dep.name.slice(), dep.constraint }) catch continue;
          defer self.allocator.free(dep_key);
          if (!processed.contains(dep_key)) {
            try next_queue.append(self.allocator, .{
              .name = dep.name.slice(),
              .constraint = dep.constraint,
              .depth = item.depth + 1,
              .direct = false,
              .parent_name = try self.allocator.dupe(u8, pkg_install_path),
            });
          }
        }
      }
      _ = debug.timer("  resolve + queue next", level_start);

      const completed = self.http.tick();
      if (completed > 0) {
        debug.log("  tarballs: {d} completed, {d} in flight", .{ completed, self.http.pendingTarballCount() });
      }

      queue.deinit(self.allocator);
      queue = next_queue;
      level += 1;
    }
  }

  fn countSatisfied(_: *Resolver, _: *const PackageMetadata, version_info: *const VersionInfo, constraint_list: anytype) usize {
    var count: usize = 0;
    for (constraint_list) |info| {
      if (info.constraint.satisfies(version_info.version)) count += 1;
    }
    return count;
  }

  fn selectVersionSatisfyingMost(_: *Resolver, metadata: *const PackageMetadata, constraint_list: anytype) ?*const VersionInfo {
    var best: ?*const VersionInfo = null;
    var best_score: i64 = -1;

    var want_prerelease = false;
    for (constraint_list) |info| {
      if (info.constraint.version.prerelease != null) {
        want_prerelease = true;
        break;
      }
    }

    for (metadata.versions.items) |*v| {
      if (v.version.prerelease != null and !want_prerelease) continue;
      if (!v.matchesPlatform()) continue;

      var score: i64 = 0;
      for (constraint_list) |info| {
        if (info.constraint.satisfies(v.version)) {
          const weight: i64 = @intCast(1000 / (info.depth + 1));
          score += weight;
        }
      }

      if (score > best_score or (score == best_score and best != null and v.version.order(best.?.version) == .gt)) {
        best = v;
        best_score = score;
      }
    }

    return best;
  }

  fn resolveSingleWithOptimal(
    self: *Resolver,
    name: []const u8,
    constraint_str: []const u8,
    depth: u32,
    direct: bool,
    parent_name: ?[]const u8,
    optimal_versions: *std.StringHashMap(*const VersionInfo),
  ) !*ResolvedPackage {
    const constraint = try Constraint.parse(constraint_str);

    if (self.resolved.get(name)) |existing_pkg| {
      if (constraint.satisfies(existing_pkg.version)) {
        if (direct) existing_pkg.direct = true;
        if (depth < existing_pkg.depth) existing_pkg.depth = depth;
        return existing_pkg;
      }

      if (parent_name) |parent| {
        var metadata = try self.fetchMetadata(name);
        const nested_best = self.selectBestVersion(&metadata, constraint) orelse return existing_pkg;
        if (!nested_best.matchesPlatform()) return existing_pkg;

        debug.log("  nested: {s}@{d}.{d}.{d} under {s} (hoisted: {d}.{d}.{d})", .{
          name,
          nested_best.version.major,
          nested_best.version.minor,
          nested_best.version.patch,
          parent,
          existing_pkg.version.major,
          existing_pkg.version.minor,
          existing_pkg.version.patch,
        });

        return try self.createNestedPackage(name, nested_best, depth, parent);
      }

      return existing_pkg;
    }

    var metadata = try self.fetchMetadata(name);
    const version_info = blk: {
      if (optimal_versions.get(name)) |optimal| {
        if (constraint.satisfies(optimal.version) and optimal.matchesPlatform()) break :blk optimal;
      }
      break :blk self.selectBestVersion(&metadata, constraint) orelse return error.NoMatchingVersion;
    };

    if (!version_info.matchesPlatform()) return error.PlatformMismatch;

    const cons_gop = try self.constraints.getOrPut(name);
    if (!cons_gop.found_existing) {
      cons_gop.key_ptr.* = try self.allocator.dupe(u8, name);
      cons_gop.value_ptr.* = .{};
    } try cons_gop.value_ptr.append(self.allocator, constraint);

    const pkg = try self.allocator.create(ResolvedPackage);
    errdefer self.allocator.destroy(pkg);

    pkg.* = .{
      .name = try self.string_pool.intern(name),
      .version = version_info.version,
      .integrity = version_info.integrity,
      .tarball_url = try self.allocator.dupe(u8, version_info.tarball_url),
      .dependencies = .{},
      .depth = depth,
      .direct = direct,
      .parent_path = null,
      .allocator = self.allocator,
    };

    const name_key = try self.allocator.dupe(u8, name);
    errdefer self.allocator.free(name_key);
    try self.resolved.put(name_key, pkg);

    var dep_it = version_info.dependencies.iterator();
    while (dep_it.next()) |entry| {
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(entry.key_ptr.*),
        .constraint = try self.allocator.dupe(u8, entry.value_ptr.*),
        .flags = .{},
      });
    }

    var opt_it = version_info.optional_dependencies.iterator();
    while (opt_it.next()) |entry| {
      if (self.metadata_cache.get(entry.key_ptr.*)) |opt_meta| {
        const opt_con = Constraint.parse(entry.value_ptr.*) catch continue;
        const opt_best = self.selectBestVersion(&opt_meta, opt_con) orelse continue;
        if (!opt_best.matchesPlatform()) continue;
      }
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(entry.key_ptr.*),
        .constraint = try self.allocator.dupe(u8, entry.value_ptr.*),
        .flags = .{ .optional = true },
      });
    }

    var peer_it = version_info.peer_dependencies.iterator();
    while (peer_it.next()) |entry| {
      if (version_info.peer_dependencies_meta.contains(entry.key_ptr.*)) continue;
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(entry.key_ptr.*),
        .constraint = try self.allocator.dupe(u8, entry.value_ptr.*),
        .flags = .{ .peer = true },
      });
    }

    if (self.on_package_resolved) |callback| {
      callback(pkg, self.on_package_resolved_data);
    }

    return pkg;
  }

  fn resolveSingle(self: *Resolver, name: []const u8, constraint_str: []const u8, depth: u32, direct: bool, parent_name: ?[]const u8) !*ResolvedPackage {
    const constraint = try Constraint.parse(constraint_str);

    if (self.resolved.get(name)) |existing_pkg| {
      if (constraint.satisfies(existing_pkg.version)) {
        if (direct) existing_pkg.direct = true;
        if (depth < existing_pkg.depth) existing_pkg.depth = depth;
        return existing_pkg;
      }

      const cons_gop = try self.constraints.getOrPut(name);
      if (!cons_gop.found_existing) {
        cons_gop.key_ptr.* = try self.allocator.dupe(u8, name);
        cons_gop.value_ptr.* = .{};
      }
      try cons_gop.value_ptr.append(self.allocator, constraint);

      var metadata = try self.fetchMetadata(name);
      const all_constraints = cons_gop.value_ptr.items;
      const best = self.selectBestVersionForConstraints(&metadata, all_constraints);

      if (best) |b| {
        if (!b.matchesPlatform()) {
          return error.PlatformMismatch;
        }

        if (b.version.order(existing_pkg.version) != .eq) {
          debug.log("  re-resolve {s}: {d}.{d}.{d} -> {d}.{d}.{d}", .{
            name,
            existing_pkg.version.major,
            existing_pkg.version.minor,
            existing_pkg.version.patch,
            b.version.major,
            b.version.minor,
            b.version.patch,
          });

          existing_pkg.version = b.version;
          existing_pkg.integrity = b.integrity;
          self.allocator.free(existing_pkg.tarball_url);
          existing_pkg.tarball_url = try self.allocator.dupe(u8, b.tarball_url);

          if (self.on_package_resolved) |callback| {
            callback(existing_pkg, self.on_package_resolved_data);
          }
        }

        if (direct) existing_pkg.direct = true;
        if (depth < existing_pkg.depth) existing_pkg.depth = depth;
        return existing_pkg;
      } else {
        if (parent_name) |parent| {
          const nested_best = self.selectBestVersion(&metadata, constraint) orelse {
            debug.log("  nested: no version for {s} {s}", .{ name, constraint_str });
            return existing_pkg;
          };

          if (!nested_best.matchesPlatform()) return existing_pkg;
          debug.log("  nested: {s}@{d}.{d}.{d} under {s} (hoisted: {d}.{d}.{d})", .{
            name,
            nested_best.version.major,
            nested_best.version.minor,
            nested_best.version.patch,
            parent,
            existing_pkg.version.major,
            existing_pkg.version.minor,
            existing_pkg.version.patch,
          });

          return try self.createNestedPackage(name, nested_best, depth, parent);
        } else {
          debug.log("  version conflict for {s}: no version satisfies all constraints", .{name});
          return existing_pkg;
        }
      }
    }

    const cons_gop = try self.constraints.getOrPut(name);
    if (!cons_gop.found_existing) {
      cons_gop.key_ptr.* = try self.allocator.dupe(u8, name);
      cons_gop.value_ptr.* = .{};
    }
    try cons_gop.value_ptr.append(self.allocator, constraint);

    var metadata = try self.fetchMetadata(name);
    const best = self.selectBestVersion(&metadata, constraint) orelse {
      return error.NoMatchingVersion;
    };

    if (!best.matchesPlatform()) {
      return error.PlatformMismatch;
    }

    const pkg = try self.allocator.create(ResolvedPackage);
    errdefer self.allocator.destroy(pkg);

    pkg.* = .{
      .name = try self.string_pool.intern(name),
      .version = best.version,
      .integrity = best.integrity,
      .tarball_url = try self.allocator.dupe(u8, best.tarball_url),
      .dependencies = .{},
      .depth = depth,
      .direct = direct,
      .parent_path = null,
      .allocator = self.allocator,
    };

    const name_key = try self.allocator.dupe(u8, name);
    errdefer self.allocator.free(name_key);
    try self.resolved.put(name_key, pkg);

    var dep_iter = best.dependencies.iterator();
    while (dep_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{},
      });
    }

    var opt_iter = best.optional_dependencies.iterator();
    while (opt_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      if (self.metadata_cache.get(dep_name)) |opt_metadata| {
        const opt_constraint = Constraint.parse(dep_constraint) catch continue;
        const opt_best = self.selectBestVersion(&opt_metadata, opt_constraint) orelse continue;

        if (!opt_best.matchesPlatform()) continue;
      }

      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{ .optional = true },
      });
    }

    var peer_iter = best.peer_dependencies.iterator();
    while (peer_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      if (best.peer_dependencies_meta.contains(dep_name)) continue;
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{ .peer = true },
      });
    }

    if (self.on_package_resolved) |callback| {
      callback(pkg, self.on_package_resolved_data);
    }

    return pkg;
  }

  fn createNestedPackage(self: *Resolver, name: []const u8, version_info: *const VersionInfo, depth: u32, parent_path: []const u8) !*ResolvedPackage {
    const nested_key = try std.fmt.allocPrint(self.allocator, "{s}/node_modules/{s}", .{ parent_path, name });
    errdefer self.allocator.free(nested_key);

    if (self.resolved.get(nested_key)) |existing| {
      self.allocator.free(nested_key);
      return existing;
    }

    const pkg = try self.allocator.create(ResolvedPackage);
    errdefer self.allocator.destroy(pkg);

    pkg.* = .{
      .name = try self.string_pool.intern(name),
      .version = version_info.version,
      .integrity = version_info.integrity,
      .tarball_url = try self.allocator.dupe(u8, version_info.tarball_url),
      .dependencies = .{},
      .depth = depth,
      .direct = false,
      .parent_path = try self.allocator.dupe(u8, parent_path),
      .allocator = self.allocator,
    };

    try self.resolved.put(nested_key, pkg);

    var dep_iter = version_info.dependencies.iterator();
    while (dep_iter.next()) |entry| {
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(entry.key_ptr.*),
        .constraint = try self.allocator.dupe(u8, entry.value_ptr.*),
        .flags = .{},
      });
    }

    var opt_iter = version_info.optional_dependencies.iterator();
    while (opt_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      if (self.metadata_cache.get(dep_name)) |opt_metadata| {
        const opt_constraint = Constraint.parse(dep_constraint) catch continue;
        const opt_best = self.selectBestVersion(&opt_metadata, opt_constraint) orelse continue;
        if (!opt_best.matchesPlatform()) continue;
      }

      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{ .optional = true },
      });
    }

    if (self.on_package_resolved) |callback| {
      callback(pkg, self.on_package_resolved_data);
    }

    return pkg;
  }

  pub fn resolve(self: *Resolver, name: []const u8, constraint_str: []const u8, depth: u32) !*ResolvedPackage {
    if (self.in_progress.contains(name)) {
      return error.CyclicDependency;
    }
    const in_progress_key = try self.allocator.dupe(u8, name);
    try self.in_progress.put(in_progress_key, {});
    defer {
      _ = self.in_progress.remove(name);
      self.allocator.free(in_progress_key);
    }

    const constraint = try Constraint.parse(constraint_str);
    const cons_gop = try self.constraints.getOrPut(name);
    
    if (!cons_gop.found_existing) {
      cons_gop.key_ptr.* = try self.allocator.dupe(u8, name);
      cons_gop.value_ptr.* = .{};
    }
    try cons_gop.value_ptr.append(self.allocator, constraint);

    if (self.resolved.get(name)) |existing_pkg| {
      if (constraint.satisfies(existing_pkg.version)) {
        if (depth == 0) existing_pkg.direct = true;
        if (depth < existing_pkg.depth) existing_pkg.depth = depth;
        return existing_pkg;
      }

      var metadata = try self.fetchMetadata(name);
      const all_constraints = cons_gop.value_ptr.items;
      const best = self.selectBestVersionForConstraints(&metadata, all_constraints) orelse {
        debug.log("  version conflict for {s}: no version satisfies all constraints", .{name});
        return existing_pkg;
      };

      if (best.version.order(existing_pkg.version) != .eq) {
        debug.log("  re-resolve {s}: {d}.{d}.{d} -> {d}.{d}.{d}", .{
          name,
          existing_pkg.version.major,
          existing_pkg.version.minor,
          existing_pkg.version.patch,
          best.version.major,
          best.version.minor,
          best.version.patch,
        });

        existing_pkg.version = best.version;
        existing_pkg.integrity = best.integrity;
        self.allocator.free(existing_pkg.tarball_url);
        existing_pkg.tarball_url = try self.allocator.dupe(u8, best.tarball_url);
      }

      if (depth == 0) existing_pkg.direct = true;
      if (depth < existing_pkg.depth) existing_pkg.depth = depth;
      return existing_pkg;
    }

    var metadata = try self.fetchMetadata(name);
    const all_constraints = cons_gop.value_ptr.items;
    const best = self.selectBestVersionForConstraints(&metadata, all_constraints) orelse {
      return error.NoMatchingVersion;
    };

    const pkg = try self.allocator.create(ResolvedPackage);
    errdefer self.allocator.destroy(pkg);

    pkg.* = .{
      .name = try self.string_pool.intern(name),
      .version = best.version,
      .integrity = best.integrity,
      .tarball_url = try self.allocator.dupe(u8, best.tarball_url),
      .dependencies = .{},
      .depth = depth,
      .direct = (depth == 0),
      .parent_path = null,
      .allocator = self.allocator,
    };

    const name_key = try self.allocator.dupe(u8, name);
    errdefer self.allocator.free(name_key);
    try self.resolved.put(name_key, pkg);

    var dep_iter = best.dependencies.iterator();
    while (dep_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      _ = self.resolve(dep_name, dep_constraint, depth + 1) catch |err| {
        std.log.debug("Skipping dep {s}: {}", .{ dep_name, err });
        continue;
      };

      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{},
      });
    }

    var opt_iter = best.optional_dependencies.iterator();
    while (opt_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      const resolved_opt = self.resolve(dep_name, dep_constraint, depth + 1) catch {
        continue;
      };

      const opt_metadata = self.fetchMetadata(dep_name) catch continue;
      const opt_constraint = Constraint.parse(dep_constraint) catch continue;
      const opt_best = self.selectBestVersion(&opt_metadata, opt_constraint) orelse continue;

      if (!opt_best.matchesPlatform()) {
        if (self.resolved.fetchRemove(dep_name)) |kv| {
          self.allocator.free(kv.key);
          kv.value.deinit();
          self.allocator.destroy(kv.value);
        }
        continue;
      }

      _ = resolved_opt;
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{ .optional = true },
      });
    }

    var peer_iter = best.peer_dependencies.iterator();
    while (peer_iter.next()) |entry| {
      const dep_name = entry.key_ptr.*;
      const dep_constraint = entry.value_ptr.*;

      if (best.peer_dependencies_meta.contains(dep_name)) continue;
      _ = self.resolve(dep_name, dep_constraint, depth + 1) catch continue;
      try pkg.dependencies.append(self.allocator, .{
        .name = try self.string_pool.intern(dep_name),
        .constraint = try self.allocator.dupe(u8, dep_constraint),
        .flags = .{ .peer = true },
      });
    }

    return pkg;
  }

  fn fetchMetadata(self: *Resolver, name: []const u8) !PackageMetadata {
    if (self.metadata_cache.get(name)) |cached| {
      debug.log("  metadata: {s} (memory cache)", .{name});
      return PackageMetadata{
        .allocator = cached.allocator,
        .name = cached.name,
        .versions = cached.versions,
      };
    }

    if (self.cache_db) |db| {
      if (db.lookupMetadata(name, self.allocator)) |json_data| {
        defer self.allocator.free(json_data);
        const metadata = PackageMetadata.parseFromJson(self.cache_allocator, json_data) catch {
          return self.fetchFromNetwork(name);
        };

        debug.log("  metadata: {s} (disk cache)", .{name});
        const cache_key = try self.cache_allocator.dupe(u8, name);
        try self.metadata_cache.put(cache_key, metadata);
        return self.metadata_cache.get(name).?;
      }
    }

    return self.fetchFromNetwork(name);
  }

  fn fetchFromNetwork(self: *Resolver, name: []const u8) !PackageMetadata {
    debug.log("  metadata: {s} (fetch)", .{name});
    const json_data = try self.http.fetchMetadata(name, self.allocator);
    defer self.allocator.free(json_data);

    if (self.cache_db) |db| {
      db.insertMetadata(name, json_data) catch {};
    }

    const metadata = try PackageMetadata.parseFromJson(self.cache_allocator, json_data);
    const cache_key = try self.cache_allocator.dupe(u8, name);
    
    try self.metadata_cache.put(cache_key, metadata);
    return self.metadata_cache.get(name).?;
  }

  fn selectBestVersion(_: *Resolver, metadata: *const PackageMetadata, constraint: Constraint) ?*const VersionInfo {
    var best: ?*const VersionInfo = null;
    const want_prerelease = constraint.version.prerelease != null;

    for (metadata.versions.items) |*v| {
      if (v.version.prerelease != null and !want_prerelease) continue;
      if (constraint.satisfies(v.version)) {
        if (best == null or v.version.order(best.?.version) == .gt) best = v;
      }
    }

    return best;
  }

  fn selectBestVersionForConstraints(_: *Resolver, metadata: *const PackageMetadata, all_constraints: []const Constraint) ?*const VersionInfo {
    var best: ?*const VersionInfo = null;

    var want_prerelease = false;
    for (all_constraints) |c| {
      if (c.version.prerelease != null) {
        want_prerelease = true;
        break;
      }
    }

    for (metadata.versions.items) |*v| {
      if (v.version.prerelease != null and !want_prerelease) continue;

      var satisfies_all = true;
      for (all_constraints) |c| {
        if (!c.satisfies(v.version)) {
          satisfies_all = false; break;
        }
      }

      if (satisfies_all) {
        if (best == null or v.version.order(best.?.version) == .gt) best = v;
      }
    }

    return best;
  }

  pub fn writeLockfile(self: *Resolver, path: []const u8) !void {
    var writer = lockfile.LockfileWriter.init(self.allocator);
    defer writer.deinit();

    var pkg_indices = std.StringHashMap(u32).init(self.allocator);
    defer {
      var key_iter = pkg_indices.keyIterator();
      while (key_iter.next()) |k| self.allocator.free(k.*);
      pkg_indices.deinit();
    }

    var idx: u32 = 0;
    var iter = self.resolved.valueIterator();
    while (iter.next()) |pkg_ptr| {
      const pkg = pkg_ptr.*;
      const install_path = try pkg.installPath(self.allocator);
      try pkg_indices.put(install_path, idx);
      idx += 1;
    }

    iter = self.resolved.valueIterator();
    while (iter.next()) |pkg_ptr| {
      const pkg = pkg_ptr.*;
      const name_ref = try writer.internString(pkg.name.slice());
      const url_ref = try writer.internString(pkg.tarball_url);
      const prerelease_ref = if (pkg.version.prerelease) |pre|
        try writer.internString(pre)
      else lockfile.StringRef.empty;
      const parent_ref = if (pkg.parent_path) |parent|
        try writer.internString(parent)
      else lockfile.StringRef.empty;

      const deps_start: u32 = @intCast(writer.dependencies.items.len);
      const pkg_install_path = try pkg.installPath(self.allocator);
      defer self.allocator.free(pkg_install_path);

      for (pkg.dependencies.items) |dep| {
        const dep_name = dep.name.slice();
        var found_idx = pkg_indices.get(dep_name);

        if (found_idx == null) {
          const nested_path = std.fmt.allocPrint(self.allocator, "{s}/node_modules/{s}", .{ pkg_install_path, dep_name }) catch continue;
          defer self.allocator.free(nested_path);
          found_idx = pkg_indices.get(nested_path);
        }

        if (found_idx) |fi| {
          const constraint_ref = try writer.internString(dep.constraint);
          try writer.addDependency(.{
            .package_index = fi,
            .constraint = constraint_ref,
            .flags = .{
              .peer = dep.flags.peer,
              .dev = dep.flags.dev,
              .optional = dep.flags.optional,
            },
          });
        }
      }

      _ = try writer.addPackage(.{
        .name = name_ref,
        .version_major = pkg.version.major,
        .version_minor = pkg.version.minor,
        .version_patch = pkg.version.patch,
        .prerelease = prerelease_ref,
        .integrity = pkg.integrity,
        .tarball_url = url_ref,
        .parent_path = parent_ref,
        .deps_start = deps_start,
        .deps_count = @intCast(pkg.dependencies.items.len),
        .flags = .{ .direct = pkg.direct },
      });
    }

    try writer.write(path);
  }
};
