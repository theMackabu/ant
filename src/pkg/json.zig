const std = @import("std");

pub const yyjson = @cImport({
  @cInclude("yyjson.h");
});

pub const JsonError = error{
  ParseError,
  OutOfMemory,
  InvalidType,
  KeyNotFound,
  IoError,
};

pub const JsonDoc = struct {
  doc: *yyjson.yyjson_doc,

  pub fn parse(data: []const u8) !JsonDoc {
    const doc = yyjson.yyjson_read(data.ptr, data.len, 0);
    if (doc == null) return error.ParseError;
    return JsonDoc{ .doc = doc.? };
  }

  pub fn parseFile(path: [:0]const u8) !JsonDoc {
    const doc = yyjson.yyjson_read_file(path.ptr, 0, null, null);
    if (doc == null) return error.ParseError;
    return JsonDoc{ .doc = doc.? };
  }

  pub fn deinit(self: *JsonDoc) void {
    yyjson.yyjson_doc_free(self.doc);
  }

  pub fn root(self: *JsonDoc) JsonValue {
    return JsonValue{ .val = yyjson.yyjson_doc_get_root(self.doc).? };
  }
};

pub const JsonValue = struct {
  val: *yyjson.yyjson_val,

  pub fn getString(self: JsonValue, key: [:0]const u8) ?[]const u8 {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_str(obj)) return null;
    const ptr = yyjson.yyjson_get_str(obj) orelse return null;
    const len = yyjson.yyjson_get_len(obj);
    return ptr[0..len];
  }

  pub fn getInt(self: JsonValue, key: [:0]const u8) ?i64 {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_int(obj)) return null;
    return yyjson.yyjson_get_sint(obj);
  }

  pub fn getUint(self: JsonValue, key: [:0]const u8) ?u64 {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_uint(obj)) return null;
    return yyjson.yyjson_get_uint(obj);
  }

  pub fn getDouble(self: JsonValue, key: [:0]const u8) ?f64 {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_real(obj)) return null;
    return yyjson.yyjson_get_real(obj);
  }

  pub fn getBool(self: JsonValue, key: [:0]const u8) ?bool {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_bool(obj)) return null;
    return yyjson.yyjson_get_bool(obj);
  }

  pub fn getObject(self: JsonValue, key: [:0]const u8) ?JsonValue {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_obj(obj)) return null;
    return JsonValue{ .val = obj };
  }

  pub fn getArray(self: JsonValue, key: [:0]const u8) ?JsonValue {
    const obj = yyjson.yyjson_obj_get(self.val, key.ptr) orelse return null;
    if (!yyjson.yyjson_is_arr(obj)) return null;
    return JsonValue{ .val = obj };
  }

  pub fn isNull(self: JsonValue) bool {
    return yyjson.yyjson_is_null(self.val);
  }

  pub fn isArray(self: JsonValue) bool {
    return yyjson.yyjson_is_arr(self.val);
  }

  pub fn isObject(self: JsonValue) bool {
    return yyjson.yyjson_is_obj(self.val);
  }

  pub fn arrayLen(self: JsonValue) usize {
    return yyjson.yyjson_arr_size(self.val);
  }

  pub fn arrayGet(self: JsonValue, index: usize) ?JsonValue {
    const elem = yyjson.yyjson_arr_get(self.val, index) orelse return null;
    return JsonValue{ .val = elem };
  }

  pub fn asString(self: JsonValue) ?[]const u8 {
    if (!yyjson.yyjson_is_str(self.val)) return null;
    const ptr = yyjson.yyjson_get_str(self.val) orelse return null;
    const len = yyjson.yyjson_get_len(self.val);
    return ptr[0..len];
  }

  pub const ObjectIterator = struct {
    iter: yyjson.yyjson_obj_iter,

    pub fn next(self: *ObjectIterator) ?struct { key: []const u8, value: JsonValue } {
      const key_val = yyjson.yyjson_obj_iter_next(&self.iter) orelse return null;
      const val = yyjson.yyjson_obj_iter_get_val(key_val) orelse return null;

      const key_ptr = yyjson.yyjson_get_str(key_val) orelse return null;
      const key_len = yyjson.yyjson_get_len(key_val);

      return .{
        .key = key_ptr[0..key_len],
        .value = JsonValue{ .val = val },
      };
    }

    pub fn deinit(_: *ObjectIterator) void {}
  };

  pub fn objectIterator(self: JsonValue) ?ObjectIterator {
    if (!yyjson.yyjson_is_obj(self.val)) return null;
    var iter: yyjson.yyjson_obj_iter = undefined;
    if (!yyjson.yyjson_obj_iter_init(self.val, &iter)) return null;
    return ObjectIterator{ .iter = iter };
  }
};

pub const JsonWriter = struct {
  doc: *yyjson.yyjson_mut_doc,

  pub fn init() !JsonWriter {
    const doc = yyjson.yyjson_mut_doc_new(null);
    if (doc == null) return error.OutOfMemory;
    return JsonWriter{ .doc = doc.? };
  }

  pub fn deinit(self: *JsonWriter) void {
    yyjson.yyjson_mut_doc_free(self.doc);
  }

  pub fn createObject(self: *JsonWriter) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_obj(self.doc).?;
  }

  pub fn createArray(self: *JsonWriter) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_arr(self.doc).?;
  }

  pub fn createString(self: *JsonWriter, str: []const u8) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_strncpy(self.doc, str.ptr, str.len).?;
  }

  pub fn createInt(self: *JsonWriter, val: i64) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_sint(self.doc, val).?;
  }

  pub fn createUint(self: *JsonWriter, val: u64) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_uint(self.doc, val).?;
  }

  pub fn createBool(self: *JsonWriter, val: bool) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_bool(self.doc, val).?;
  }

  pub fn createReal(self: *JsonWriter, val: f64) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_real(self.doc, val).?;
  }

  pub fn createNull(self: *JsonWriter) *yyjson.yyjson_mut_val {
    return yyjson.yyjson_mut_null(self.doc).?;
  }

  pub fn objectAdd(self: *JsonWriter, obj: *yyjson.yyjson_mut_val, key: []const u8, val: *yyjson.yyjson_mut_val) void {
    const key_val = yyjson.yyjson_mut_strncpy(self.doc, key.ptr, key.len);
    _ = yyjson.yyjson_mut_obj_add(obj, key_val, val);
  }

  pub fn arrayAppend(_: *JsonWriter, arr: *yyjson.yyjson_mut_val, val: *yyjson.yyjson_mut_val) void {
    _ = yyjson.yyjson_mut_arr_append(arr, val);
  }

  pub fn setRoot(self: *JsonWriter, val: *yyjson.yyjson_mut_val) void {
    yyjson.yyjson_mut_doc_set_root(self.doc, val);
  }

  pub fn write(self: *JsonWriter, allocator: std.mem.Allocator) ![]u8 {
    var len: usize = 0;
    const ptr = yyjson.yyjson_mut_write(self.doc, yyjson.YYJSON_WRITE_PRETTY, &len);
    if (ptr == null) return error.OutOfMemory;
    defer std.c.free(ptr);

    const result = try allocator.alloc(u8, len);
    @memcpy(result, ptr[0..len]);
    return result;
  }

  pub fn writeToFile(self: *JsonWriter, path: [:0]const u8) !void {
    const success = yyjson.yyjson_mut_write_file(path.ptr, self.doc, yyjson.YYJSON_WRITE_PRETTY, null, null);
    if (!success) return error.IoError;
  }
};

pub const PackageJson = struct {
  name: []const u8,
  version: []const u8,
  dependencies: std.StringHashMap([]const u8),
  dev_dependencies: std.StringHashMap([]const u8),
  peer_dependencies: std.StringHashMap([]const u8),
  optional_dependencies: std.StringHashMap([]const u8),
  trusted_dependencies: std.StringHashMap(void),

  pub fn parse(allocator: std.mem.Allocator, path: [:0]const u8) !PackageJson {
    var doc = try JsonDoc.parseFile(path);
    defer doc.deinit();

    const root_val = doc.root();

    var pkg = PackageJson{
      .name = "",
      .version = "",
      .dependencies = std.StringHashMap([]const u8).init(allocator),
      .dev_dependencies = std.StringHashMap([]const u8).init(allocator),
      .peer_dependencies = std.StringHashMap([]const u8).init(allocator),
      .optional_dependencies = std.StringHashMap([]const u8).init(allocator),
      .trusted_dependencies = std.StringHashMap(void).init(allocator),
    };

    if (root_val.getString("name")) |s| {
      pkg.name = try allocator.dupe(u8, s);
    }

    if (root_val.getString("version")) |s| {
      pkg.version = try allocator.dupe(u8, s);
    }

    try parseDeps(allocator, root_val, "dependencies", &pkg.dependencies);
    try parseDeps(allocator, root_val, "devDependencies", &pkg.dev_dependencies);
    try parseDeps(allocator, root_val, "peerDependencies", &pkg.peer_dependencies);
    try parseDeps(allocator, root_val, "optionalDependencies", &pkg.optional_dependencies);

    if (root_val.getArray("trustedDependencies")) |arr| {
      for (0..arr.arrayLen()) |i| {
        const name = (arr.arrayGet(i) orelse continue).asString() orelse continue;
        try pkg.trusted_dependencies.put(try allocator.dupe(u8, name), {});
      }
    }

    return pkg;
  }

  fn parseDeps(
    allocator: std.mem.Allocator,
    root_val: JsonValue,
    key: [:0]const u8,
    map: *std.StringHashMap([]const u8),
  ) !void {
    if (root_val.getObject(key)) |deps| {
      var iter = deps.objectIterator() orelse return;
      defer iter.deinit();
      while (iter.next()) |entry| {
        const version = entry.value.asString() orelse continue;
        try map.put(try allocator.dupe(u8, entry.key), try allocator.dupe(u8, version));
      }
    }
  }

  pub fn deinit(self: *PackageJson, allocator: std.mem.Allocator) void {
    if (self.name.len > 0) allocator.free(self.name);
    if (self.version.len > 0) allocator.free(self.version);

    var iter = self.dependencies.iterator();
    while (iter.next()) |entry| {
      allocator.free(entry.key_ptr.*);
      allocator.free(entry.value_ptr.*);
    }
    self.dependencies.deinit();

    iter = self.dev_dependencies.iterator();
    while (iter.next()) |entry| {
      allocator.free(entry.key_ptr.*);
      allocator.free(entry.value_ptr.*);
    }
    self.dev_dependencies.deinit();

    iter = self.peer_dependencies.iterator();
    while (iter.next()) |entry| {
      allocator.free(entry.key_ptr.*);
      allocator.free(entry.value_ptr.*);
    }
    self.peer_dependencies.deinit();

    iter = self.optional_dependencies.iterator();
    while (iter.next()) |entry| {
      allocator.free(entry.key_ptr.*);
      allocator.free(entry.value_ptr.*);
    }
    self.optional_dependencies.deinit();

    var trusted_iter = self.trusted_dependencies.keyIterator();
    while (trusted_iter.next()) |key| {
      allocator.free(key.*);
    }
    self.trusted_dependencies.deinit();
  }
};