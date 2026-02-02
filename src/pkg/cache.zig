const std = @import("std");

const c = @cImport({
  @cInclude("lmdb.h");
});

extern fn strip_npm_metadata(json_data: [*]const u8, json_len: usize, out_len: *usize) ?[*]u8;
extern fn strip_metadata_free(ptr: [*]u8) void;

pub const CacheEntry = struct {
  integrity: [64]u8,
  path: []const u8,
  unpacked_size: u64,
  file_count: u32,
  cached_at: i64,
  allocator: ?std.mem.Allocator = null,

  pub fn deinit(self: *CacheEntry) void {
    if (self.allocator) |alloc| alloc.free(self.path);
  }
};

const SerializedEntry = extern struct {
  unpacked_size: u64,
  file_count: u32,
  cached_at: i64,
  path_len: u32,
};

fn check(rc: c_int) !void {
  if (rc != 0) return error.MdbError;
}

pub const CacheDB = struct {
  env: *c.MDB_env,
  dbi_primary: c.MDB_dbi,
  dbi_secondary: c.MDB_dbi,
  dbi_metadata: c.MDB_dbi,
  cache_dir: []const u8,
  allocator: std.mem.Allocator,

  const MAP_SIZE: usize = 1024 * 1024 * 1024;
  const METADATA_TTL_SECS: i64 = 24 * 60 * 60;

  pub fn open(cache_dir: []const u8) !*CacheDB {
    const allocator = std.heap.c_allocator;

    std.fs.cwd().makePath(cache_dir) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.CacheError,
    };

    const packages_path = try std.fmt.allocPrintSentinel(allocator, "{s}/cache", .{cache_dir}, 0);
    defer allocator.free(packages_path);
    std.fs.cwd().makePath(packages_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.CacheError,
    };

    var env: ?*c.MDB_env = null;
    if (c.mdb_env_create(&env) != 0) {
      return error.DatabaseOpen;
    }
    errdefer c.mdb_env_close(env);

    try check(c.mdb_env_set_mapsize(env, MAP_SIZE));
    try check(c.mdb_env_set_maxdbs(env, 3));

    const db_path = try std.fmt.allocPrintSentinel(allocator, "{s}/index.lmdb", .{cache_dir}, 0);
    defer allocator.free(db_path);

    const flags: c_uint = c.MDB_NOSUBDIR | c.MDB_NOSYNC;
    if (c.mdb_env_open(env, db_path.ptr, flags, 0o644) != 0) {
      return error.DatabaseOpen;
    }

    const self = try allocator.create(CacheDB);
    errdefer allocator.destroy(self);

    self.* = .{
      .env = env.?,
      .dbi_primary = 0,
      .dbi_secondary = 0,
      .dbi_metadata = 0,
      .cache_dir = try allocator.dupe(u8, cache_dir),
      .allocator = allocator,
    };

    try self.openDatabases();

    return self;
  }

  fn openDatabases(self: *CacheDB) !void {
    var txn: ?*c.MDB_txn = null;
    
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.DatabaseOpen;
    } errdefer c.mdb_txn_abort(txn);

    if (c.mdb_dbi_open(txn, "primary", c.MDB_CREATE, &self.dbi_primary) != 0) return error.DatabaseOpen;
    if (c.mdb_dbi_open(txn, "secondary", c.MDB_CREATE, &self.dbi_secondary) != 0) return error.DatabaseOpen;
    if (c.mdb_dbi_open(txn, "metadata", c.MDB_CREATE, &self.dbi_metadata) != 0) return error.DatabaseOpen;
    if (c.mdb_txn_commit(txn) != 0) return error.DatabaseOpen;
  }

  pub fn close(self: *CacheDB) void {
    c.mdb_dbi_close(self.env, self.dbi_primary);
    c.mdb_dbi_close(self.env, self.dbi_secondary);
    c.mdb_dbi_close(self.env, self.dbi_metadata);
    c.mdb_env_close(self.env);
    
    self.allocator.free(self.cache_dir);
    self.allocator.destroy(self);
  }

  fn makeIntegrityKey(integrity: *const [64]u8) [66]u8 {
    var key: [66]u8 = undefined;
    key[0] = 'i'; key[1] = ':';
    @memcpy(key[2..66], integrity);
    return key;
  }

  fn makeNameKey(allocator: std.mem.Allocator, name: []const u8, version: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "n:{s}@{s}", .{ name, version });
  }

  pub fn lookup(self: *CacheDB, integrity: *const [64]u8) ?CacheEntry {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return null;
    } defer c.mdb_txn_abort(txn);

    const key_bytes = makeIntegrityKey(integrity);
    var key = c.MDB_val{
      .mv_size = key_bytes.len,
      .mv_data = @constCast(&key_bytes),
    };
    var value: c.MDB_val = undefined;

    if (c.mdb_get(txn, self.dbi_primary, &key, &value) != 0) return null;
    return deserializeEntry(integrity, value, self.allocator);
  }

  pub fn hasIntegrity(self: *CacheDB, integrity: *const [64]u8) bool {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return false;
    } defer c.mdb_txn_abort(txn);

    const key_bytes = makeIntegrityKey(integrity);
    var key = c.MDB_val{
      .mv_size = key_bytes.len,
      .mv_data = @constCast(&key_bytes),
    };
    var value: c.MDB_val = undefined;

    return c.mdb_get(txn, self.dbi_primary, &key, &value) == 0;
  }

  fn deserializeEntry(integrity: *const [64]u8, value: c.MDB_val, allocator: std.mem.Allocator) ?CacheEntry {
    if (value.mv_size < @sizeOf(SerializedEntry)) return null;

    const data: [*]const u8 = @ptrCast(value.mv_data);
    const header: *const SerializedEntry = @ptrCast(@alignCast(data));

    const path_start = @sizeOf(SerializedEntry);
    if (value.mv_size < path_start + header.path_len) return null;

    const path = allocator.dupe(u8, data[path_start..][0..header.path_len]) catch return null;

    return CacheEntry{
      .integrity = integrity.*,
      .path = path,
      .unpacked_size = header.unpacked_size,
      .file_count = header.file_count,
      .cached_at = header.cached_at,
      .allocator = allocator,
    };
  }

  pub fn lookupByName(self: *CacheDB, name: []const u8, version: []const u8) ?CacheEntry {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return null;
    } defer c.mdb_txn_abort(txn);

    const name_key = makeNameKey(self.allocator, name, version) catch return null;
    defer self.allocator.free(name_key);

    var key = c.MDB_val{
      .mv_size = name_key.len,
      .mv_data = @constCast(name_key.ptr),
    };
    var value: c.MDB_val = undefined;

    if (c.mdb_get(txn, self.dbi_secondary, &key, &value) != 0) return null;
    if (value.mv_size != 64) return null;

    const integrity: *const [64]u8 = @ptrCast(value.mv_data);
    return self.lookup(integrity);
  }

  pub const BatchHit = struct {
    index: u32,
    file_count: u32,
  };

  pub fn batchLookup(
    self: *CacheDB,
    integrities: []const [64]u8,
    allocator: std.mem.Allocator,
  ) !struct {
    items: []BatchHit,
    allocator: std.mem.Allocator,
    pub fn deinit(s: *@This()) void {
      s.allocator.free(s.items);
    }
  } {
    var hits = std.ArrayListUnmanaged(BatchHit){};
    errdefer hits.deinit(allocator);

    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return .{ .items = &.{}, .allocator = allocator };
    } defer c.mdb_txn_abort(txn);

    for (integrities, 0..) |integrity, i| {
      const key_bytes = makeIntegrityKey(&integrity);
      var key = c.MDB_val{
        .mv_size = key_bytes.len,
        .mv_data = @constCast(&key_bytes),
      };
      var value: c.MDB_val = undefined;
      if (c.mdb_get(txn, self.dbi_primary, &key, &value) == 0) {
        var file_count: u32 = 0;
        if (value.mv_size >= @sizeOf(SerializedEntry)) {
          const data: [*]const u8 = @ptrCast(value.mv_data);
          const header: *const SerializedEntry = @ptrCast(@alignCast(data));
          file_count = header.file_count;
        }
        try hits.append(allocator, .{ .index = @intCast(i), .file_count = file_count });
      }
    }

    return .{ .items = hits.toOwnedSlice(allocator) catch &.{}, .allocator = allocator };
  }

  pub fn insert(self: *CacheDB, entry: *const CacheEntry, name: ?[]const u8, version: ?[]const u8) !void {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.InsertError;
    } errdefer c.mdb_txn_abort(txn);

    try self.insertInTxn(txn.?, entry, name, version);
    if (c.mdb_txn_commit(txn) != 0) return error.InsertError;
  }

  fn insertInTxn(self: *CacheDB, txn: *c.MDB_txn, entry: *const CacheEntry, name: ?[]const u8, version: ?[]const u8) !void {
    const value_size = @sizeOf(SerializedEntry) + entry.path.len;
    const value_buf = try self.allocator.alloc(u8, value_size);
    defer self.allocator.free(value_buf);

    const header: *SerializedEntry = @ptrCast(@alignCast(value_buf.ptr));
    header.* = .{
      .unpacked_size = entry.unpacked_size,
      .file_count = entry.file_count,
      .cached_at = entry.cached_at,
      .path_len = @intCast(entry.path.len),
    };
    
    @memcpy(value_buf[@sizeOf(SerializedEntry)..], entry.path);
    const key_bytes = makeIntegrityKey(&entry.integrity);
    
    var key = c.MDB_val{
      .mv_size = key_bytes.len,
      .mv_data = @constCast(&key_bytes),
    };
    
    var value = c.MDB_val{
      .mv_size = value_size,
      .mv_data = value_buf.ptr,
    };

    if (c.mdb_put(txn, self.dbi_primary, &key, &value, 0) != 0) {
      return error.InsertError;
    }

    if (name != null and version != null) {
      const name_key = try makeNameKey(self.allocator, name.?, version.?);
      defer self.allocator.free(name_key);

      var sec_key = c.MDB_val{
        .mv_size = name_key.len,
        .mv_data = @constCast(name_key.ptr),
      };
      var sec_value = c.MDB_val{
        .mv_size = 32,
        .mv_data = @constCast(&entry.integrity),
      };

      if (c.mdb_put(txn, self.dbi_secondary, &sec_key, &sec_value, 0) != 0) {
        return error.InsertError;
      }
    }
  }

  pub fn batchInsert(self: *CacheDB, entries: []const CacheEntry) !void {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.InsertError;
    } errdefer c.mdb_txn_abort(txn);

    for (entries) |*entry| {
      try self.insertInTxn(txn.?, entry, null, null);
    } if (c.mdb_txn_commit(txn) != 0) return error.InsertError;
  }

  pub const NamedCacheEntry = struct {
    entry: CacheEntry,
    name: []const u8,
    version: []const u8,
  };

  pub fn batchInsertNamed(self: *CacheDB, entries: []const NamedCacheEntry) !void {
    if (entries.len == 0) return;

    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.InsertError;
    } errdefer c.mdb_txn_abort(txn);

    for (entries) |item| {
      self.insertInTxn(txn.?, &item.entry, item.name, item.version) catch continue;
    } if (c.mdb_txn_commit(txn) != 0) return error.InsertError;
  }

  pub fn delete(self: *CacheDB, integrity: *const [64]u8) !void {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.DeleteError;
    } errdefer c.mdb_txn_abort(txn);

    const key_bytes = makeIntegrityKey(integrity);
    var key = c.MDB_val{
      .mv_size = key_bytes.len,
      .mv_data = @constCast(&key_bytes),
    };

    _ = c.mdb_del(txn, self.dbi_primary, &key, null);
    if (c.mdb_txn_commit(txn) != 0) return error.DeleteError;
  }

  pub fn getPackagePath(self: *CacheDB, integrity: *const [64]u8, allocator: std.mem.Allocator) ![]u8 {
    const hex = std.fmt.bytesToHex(integrity.*, .lower);
    return std.fmt.allocPrint(allocator, "{s}/cache/{s}", .{ self.cache_dir, hex });
  }

  pub fn sync(self: *CacheDB) void {
    _ = c.mdb_env_sync(self.env, 1);
  }

  pub fn stats(self: *CacheDB) !struct { entries: usize, map_size: usize, used_size: usize } {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return error.DatabaseError;
    } defer c.mdb_txn_abort(txn);

    var db_stat: c.MDB_stat = undefined;
    _ = c.mdb_stat(txn, self.dbi_primary, &db_stat);

    var env_info: c.MDB_envinfo = undefined;
    _ = c.mdb_env_info(self.env, &env_info);

    return .{
      .entries = db_stat.ms_entries,
      .map_size = env_info.me_mapsize,
      .used_size = env_info.me_last_pgno * @as(usize, @intCast(db_stat.ms_psize)),
    };
  }

  fn makeMetadataKey(allocator: std.mem.Allocator, name: []const u8) ![]u8 {
    return std.fmt.allocPrint(allocator, "m:{s}", .{name});
  }

  pub fn lookupMetadata(self: *CacheDB, name: []const u8, allocator: std.mem.Allocator) ?[]u8 {
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, c.MDB_RDONLY, &txn) != 0) {
      return null;
    } defer c.mdb_txn_abort(txn);

    const meta_key = makeMetadataKey(self.allocator, name) catch return null;
    defer self.allocator.free(meta_key);

    var key = c.MDB_val{
      .mv_size = meta_key.len,
      .mv_data = @constCast(meta_key.ptr),
    };
    var value: c.MDB_val = undefined;

    if (c.mdb_get(txn, self.dbi_metadata, &key, &value) != 0) return null;
    if (value.mv_size < @sizeOf(i64)) return null;

    const data: [*]const u8 = @ptrCast(value.mv_data);
    const cached_at: i64 = @bitCast(data[0..@sizeOf(i64)].*);

    const now = std.time.timestamp();
    if (now - cached_at > METADATA_TTL_SECS) return null;

    const json_data = data[@sizeOf(i64)..value.mv_size];
    return allocator.dupe(u8, json_data) catch null;
  }

  pub fn insertMetadata(self: *CacheDB, name: []const u8, json_data: []const u8) !void {
    var stripped_len: usize = 0;
    const stripped_ptr = strip_npm_metadata(json_data.ptr, json_data.len, &stripped_len);
    defer if (stripped_ptr) |p| strip_metadata_free(p);

    const data_to_store = if (stripped_ptr) |p| p[0..stripped_len] else json_data;

    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.InsertError;
    }
    errdefer c.mdb_txn_abort(txn);

    const meta_key = try makeMetadataKey(self.allocator, name);
    defer self.allocator.free(meta_key);

    const value_size = @sizeOf(i64) + data_to_store.len;
    const value_buf = try self.allocator.alloc(u8, value_size);
    defer self.allocator.free(value_buf);

    const now: i64 = std.time.timestamp();
    @memcpy(value_buf[0..@sizeOf(i64)], std.mem.asBytes(&now));
    @memcpy(value_buf[@sizeOf(i64)..], data_to_store);

    var key = c.MDB_val{
      .mv_size = meta_key.len,
      .mv_data = @constCast(meta_key.ptr),
    };
    
    var value = c.MDB_val{
      .mv_size = value_size,
      .mv_data = value_buf.ptr,
    };

    if (c.mdb_put(txn, self.dbi_metadata, &key, &value, 0) != 0) return error.InsertError;
    if (c.mdb_txn_commit(txn) != 0) return error.InsertError;
  }

  pub fn prune(self: *CacheDB, max_age_days: u32) !u32 {
    const now = std.time.timestamp();
    const max_age_secs: i64 = @as(i64, max_age_days) * 24 * 60 * 60;
    const cutoff = now - max_age_secs;
    
    var txn: ?*c.MDB_txn = null;
    if (c.mdb_txn_begin(self.env, null, 0, &txn) != 0) {
      return error.DatabaseError;
    }
    errdefer c.mdb_txn_abort(txn);
    
    var cursor: ?*c.MDB_cursor = null;
    if (c.mdb_cursor_open(txn, self.dbi_primary, &cursor) != 0) {
      return error.DatabaseError;
    }
    defer c.mdb_cursor_close(cursor);
    
    var key: c.MDB_val = undefined;
    var value: c.MDB_val = undefined;
    var pruned: u32 = 0;
    var to_delete = std.ArrayListUnmanaged([66]u8){};
    defer to_delete.deinit(self.allocator);
    var paths_to_delete = std.ArrayListUnmanaged([]const u8){};
    defer {
      for (paths_to_delete.items) |p| self.allocator.free(p);
      paths_to_delete.deinit(self.allocator);
    }
    
    var rc = c.mdb_cursor_get(cursor, &key, &value, c.MDB_FIRST);
    while (rc == 0) {
      if (value.mv_size >= @sizeOf(SerializedEntry)) {
        const data: [*]const u8 = @ptrCast(value.mv_data);
        const header: *const SerializedEntry = @ptrCast(@alignCast(data));
        
        if (header.cached_at < cutoff) {
          const key_data: [*]const u8 = @ptrCast(key.mv_data);
          var key_copy: [66]u8 = undefined;
          if (key.mv_size == 66) {
            @memcpy(&key_copy, key_data[0..66]);
            to_delete.append(self.allocator, key_copy) catch {};
            
            const path_start = @sizeOf(SerializedEntry);
            if (value.mv_size >= path_start + header.path_len) {
              const path = self.allocator.dupe(u8, data[path_start..][0..header.path_len]) catch continue;
              paths_to_delete.append(self.allocator, path) catch self.allocator.free(path);
            }
          }
        }
      }
      rc = c.mdb_cursor_get(cursor, &key, &value, c.MDB_NEXT);
    }
    
    c.mdb_cursor_close(cursor);
    cursor = null;
    
    for (to_delete.items) |*key_bytes| {
      var del_key = c.MDB_val{ .mv_size = 66, .mv_data = key_bytes };
      if (c.mdb_del(txn, self.dbi_primary, &del_key, null) == 0) pruned += 1;
    }
    
    if (c.mdb_txn_commit(txn) != 0) return error.DatabaseError;
    for (paths_to_delete.items) |path| std.fs.cwd().deleteTree(path) catch {};
    
    return pruned;
  }
};
