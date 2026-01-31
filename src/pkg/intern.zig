const std = @import("std");

pub const InternedString = struct {
  ptr: [*]const u8,
  len: u32,

  pub fn slice(self: InternedString) []const u8 {
    return self.ptr[0..self.len];
  }

  pub fn eql(a: InternedString, b: InternedString) bool {
    return a.ptr == b.ptr and a.len == b.len;
  }

  pub fn hash(self: InternedString) u64 {
    return @intFromPtr(self.ptr);
  }

  pub const empty: InternedString = .{ .ptr = "", .len = 0 };
};

pub const StringPool = struct {
  allocator: std.mem.Allocator,
  strings: std.StringHashMap(InternedString),
  storage: std.ArrayListUnmanaged([]const u8),

  pub fn init(allocator: std.mem.Allocator) StringPool {
    return .{
      .allocator = allocator,
      .strings = std.StringHashMap(InternedString).init(allocator),
      .storage = std.ArrayListUnmanaged([]const u8){},
    };
  }

  pub fn deinit(self: *StringPool) void {
    for (self.storage.items) |s| {
      self.allocator.free(s);
    }
    self.storage.deinit(self.allocator);
    self.strings.deinit();
  }

  pub fn intern(self: *StringPool, str: []const u8) !InternedString {
    if (str.len == 0) return InternedString.empty;
    if (self.strings.get(str)) |interned| return interned;

    const owned = try self.allocator.dupe(u8, str);
    errdefer self.allocator.free(owned);

    try self.storage.append(self.allocator, owned);

    const interned = InternedString{
      .ptr = owned.ptr,
      .len = @intCast(owned.len),
    };

    try self.strings.put(owned, interned);
    return interned;
  }

  pub fn internOwned(self: *StringPool, owned: []const u8) !InternedString {
    if (owned.len == 0) return InternedString.empty;

    if (self.strings.get(owned)) |interned| {
      self.allocator.free(@constCast(owned));
      return interned;
    }

    try self.storage.append(self.allocator, owned);

    const interned = InternedString{
      .ptr = owned.ptr,
      .len = @intCast(owned.len),
    };

    try self.strings.put(owned, interned);
    return interned;
  }

  pub fn stats(self: *const StringPool) Stats {
    var total_bytes: usize = 0;
    for (self.storage.items) |s| {
      total_bytes += s.len;
    }
    return .{
      .string_count = self.storage.items.len,
      .total_bytes = total_bytes,
    };
  }

  pub const Stats = struct {
    string_count: usize,
    total_bytes: usize,
  };
};

pub const CommonStrings = struct {
  pool: *StringPool,

  lodash: InternedString = InternedString.empty,
  react: InternedString = InternedString.empty,
  typescript: InternedString = InternedString.empty,
  webpack: InternedString = InternedString.empty,
  babel: InternedString = InternedString.empty,
  eslint: InternedString = InternedString.empty,
  jest: InternedString = InternedString.empty,
  express: InternedString = InternedString.empty,

  caret: InternedString = InternedString.empty, // ^
  tilde: InternedString = InternedString.empty, // ~

  pub fn init(pool: *StringPool) !CommonStrings {
    return .{
      .pool = pool,
      .lodash = try pool.intern("lodash"),
      .react = try pool.intern("react"),
      .typescript = try pool.intern("typescript"),
      .webpack = try pool.intern("webpack"),
      .babel = try pool.intern("@babel/core"),
      .eslint = try pool.intern("eslint"),
      .jest = try pool.intern("jest"),
      .express = try pool.intern("express"),
      .caret = try pool.intern("^"),
      .tilde = try pool.intern("~"),
    };
  }
};