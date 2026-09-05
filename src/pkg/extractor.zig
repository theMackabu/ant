const std = @import("std");
const builtin = @import("builtin");
const linker = @import("linker.zig");
const io = std.Io.Threaded.global_single_threaded.io();

const c = @cImport({
  @cInclude("zlib.h");
});

pub const ExtractError = error{
  DecompressionFailed,
  InvalidTarHeader,
  IoError,
  OutOfMemory,
  PathTooLong,
  UnsupportedFormat,
  InvalidPath,
};

inline fn validateBasic(path: []const u8) ExtractError!void {
  if (path.len == 0 or path.len > 4096) return error.InvalidPath;
  if (path[0] == '/') return error.InvalidPath;
}

inline fn validateBadCharsAndTraversal(path: []const u8) ExtractError!void {
  const len = path.len;
  var i: usize = 0; var segment_start: usize = 0;

  while (i < len) : (i += 1) {
    const ch = path[i];
    if (ch == 0 or ch == '\\' or ch < 0x20) return error.InvalidPath;
    if (ch == '/') {
      const seg_len = i - segment_start; if (seg_len == 2) {
        const seg = path[segment_start..i];
        if (seg[0] == '.' and seg[1] == '.') return error.InvalidPath;
      } segment_start = i + 1;
    }
  }

  const final_len = len - segment_start; if (final_len == 2) {
    const seg = path[segment_start..];
    if (seg[0] == '.' and seg[1] == '.') return error.InvalidPath;
  }
}

inline fn isWindowsReserved(name: []const u8) bool {
  const reserved = [_][]const u8{
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
  };

  for (reserved) |r| {
    if (name.len < r.len) continue;
    const prefix = name[0..r.len];
    if (!std.ascii.eqlIgnoreCase(prefix, r)) continue;
    return name.len == r.len or name[r.len] == '.';
  }
  
  return false;
}

inline fn validateWindowsReserved(path: []const u8) ExtractError!void {
  if (comptime builtin.os.tag != .windows) return;

  const slash_idx = std.mem.lastIndexOfScalar(u8, path, '/');
  const basename = if (slash_idx) |i| path[i + 1 ..] else path;
  if (basename.len == 0) return error.InvalidPath;

  const first = std.ascii.toUpper(basename[0]);
  const should_check = first == 'C' or first == 'P' or first == 'A' or first == 'N' or first == 'L';
  if (should_check and isWindowsReserved(basename)) return error.InvalidPath;
}

fn validatePath(path: []const u8) ExtractError!void {
  try validateBasic(path);
  try validateBadCharsAndTraversal(path);
  try validateWindowsReserved(path);
}

pub const TarHeader = extern struct {
  name: [100]u8,
  mode: [8]u8,
  uid: [8]u8,
  gid: [8]u8,
  size: [12]u8,
  mtime: [12]u8,
  checksum: [8]u8,
  typeflag: u8,
  linkname: [100]u8,
  magic: [6]u8,
  version: [2]u8,
  uname: [32]u8,
  gname: [32]u8,
  devmajor: [8]u8,
  devminor: [8]u8,
  prefix: [155]u8,
  _padding: [12]u8,

  comptime {
    std.debug.assert(@sizeOf(TarHeader) == 512);
  }

  pub fn isZero(self: *const TarHeader) bool {
    const bytes: *const [512]u8 = @ptrCast(self);
    for (bytes) |b| if (b != 0) return false;
    return true;
  }

  pub fn getName(self: *const TarHeader, buf: []u8) ![]const u8 {
    const prefix_len = std.mem.indexOfScalar(u8, &self.prefix, 0) orelse self.prefix.len;
    const name_len = std.mem.indexOfScalar(u8, &self.name, 0) orelse self.name.len;
    
    if (prefix_len > 0) {
      const total_len = prefix_len + 1 + name_len;
      if (total_len > buf.len) return error.InvalidPath;
      @memcpy(buf[0..prefix_len], self.prefix[0..prefix_len]);
      buf[prefix_len] = '/';
      @memcpy(buf[prefix_len + 1 ..][0..name_len], self.name[0..name_len]);
      return buf[0 .. prefix_len + 1 + name_len];
    }
    
    return self.name[0..name_len];
  }

  pub fn getSize(self: *const TarHeader) !u64 {
    const size_str = std.mem.trimEnd(u8, &self.size, &[_]u8{ 0, ' ' });
    return std.fmt.parseInt(u64, size_str, 8) catch return error.InvalidTarHeader;
  }

  pub fn getMode(self: *const TarHeader) !u32 {
    const mode_str = std.mem.trimEnd(u8, &self.mode, &[_]u8{ 0, ' ' });
    return std.fmt.parseInt(u32, mode_str, 8) catch return error.InvalidTarHeader;
  }

  pub fn isFile(self: *const TarHeader) bool {
    return self.typeflag == '0' or self.typeflag == 0;
  }

  pub fn isDirectory(self: *const TarHeader) bool {
    return self.typeflag == '5';
  }

  pub fn isSymlink(self: *const TarHeader) bool {
    return self.typeflag == '2';
  }
};

pub const GzipDecompressor = struct {
  stream: c.z_stream,
  initialized: bool,
  allocator: std.mem.Allocator,

  pub fn init(allocator: std.mem.Allocator) !*GzipDecompressor {
    const self = try allocator.create(GzipDecompressor);
    errdefer allocator.destroy(self);

    self.allocator = allocator;
    self.stream = std.mem.zeroes(c.z_stream);
    self.initialized = false;

    const ret = c.inflateInit2(&self.stream, 15 + 32);
    if (ret != c.Z_OK) {
      allocator.destroy(self);
      return error.DecompressionFailed;
    }

    self.initialized = true;
    return self;
  }

  pub fn deinit(self: *GzipDecompressor) void {
    if (self.initialized) _ = c.inflateEnd(&self.stream);
    self.allocator.destroy(self);
  }

  pub fn decompress(
    self: *GzipDecompressor,
    input: []const u8,
    output_fn: *const fn (data: []const u8, user_data: ?*anyopaque) anyerror!void,
    user_data: ?*anyopaque,
  ) !bool {
    var output_buf: [256 * 1024]u8 = undefined;

    self.stream.next_in = @constCast(input.ptr);
    self.stream.avail_in = @intCast(input.len);

    while (self.stream.avail_in > 0) {
      self.stream.next_out = &output_buf;
      self.stream.avail_out = output_buf.len;

      const ret = c.inflate(&self.stream, c.Z_NO_FLUSH);

      if (ret == c.Z_STREAM_END) {
        const produced = output_buf.len - self.stream.avail_out;
        if (produced > 0) {
          try output_fn(output_buf[0..produced], user_data);
        } return true;
      }

      if (ret != c.Z_OK) return error.DecompressionFailed;
      const produced = output_buf.len - self.stream.avail_out;
      if (produced > 0) try output_fn(output_buf[0..produced], user_data);
    }

    return false;
  }
};

pub const TarParser = struct {
  state: State,
  header: TarHeader,
  header_bytes_read: usize,
  current_file_remaining: u64,
  skip_bytes: usize,
  strip_prefix: [128]u8,
  strip_prefix_len: usize,
  prefix_detected: bool,
  path_buf: [4096]u8,
  pax_data: [64 * 1024]u8 = undefined,
  pax_len: usize = 0,
  pax_path: [4096]u8 = undefined,
  pax_path_len: usize = 0,
  pax_link: [4096]u8 = undefined,
  pax_link_len: usize = 0,
  pax_size: ?u64 = null,
  entry_size: u64 = 0,

  const State = enum {
    read_header,
    read_file_data,
    read_pax_data,
    skip_padding,
  };

  pub fn init(default_prefix: []const u8) TarParser {
    var prefix_buf: [128]u8 = undefined;
    const len = @min(default_prefix.len, 128);
    @memcpy(prefix_buf[0..len], default_prefix[0..len]);
    return .{
      .state = .read_header,
      .header = undefined,
      .header_bytes_read = 0,
      .current_file_remaining = 0,
      .skip_bytes = 0,
      .strip_prefix = prefix_buf,
      .strip_prefix_len = len,
      .prefix_detected = false,
      .path_buf = undefined,
    };
  }

  pub const Entry = struct {
    path: []const u8,
    mode: u32,
    size: u64,
    entry_type: Type,
    link_target: []const u8,

    pub const Type = enum {
      file,
      directory,
      symlink,
    };
  };

  pub const ParseResult = struct {
    kind: Kind,
    consumed: usize,

    pub const Kind = union(enum) {
      need_more_data,
      entry: Entry,
      file_data: []const u8,
      end_of_archive,
      err: ExtractError,
    };
  };

  fn parsePax(self: *TarParser) ExtractError!void {
    var records = self.pax_data[0..self.pax_len];
    while (records.len > 0) {
      const space = std.mem.indexOfScalar(u8, records, ' ') orelse return error.InvalidTarHeader;
      const len = std.fmt.parseInt(usize, records[0..space], 10) catch return error.InvalidTarHeader;
      if (len <= space + 1 or len > records.len or records[len - 1] != '\n') return error.InvalidTarHeader;
      const record = records[space + 1 .. len - 1];
      const equals = std.mem.indexOfScalar(u8, record, '=') orelse return error.InvalidTarHeader;
      const key = record[0..equals];
      const value = record[equals + 1 ..];
      if (std.mem.eql(u8, key, "path")) {
        try validatePath(value);
        @memcpy(self.pax_path[0..value.len], value);
        self.pax_path_len = value.len;
      } else if (std.mem.eql(u8, key, "linkpath")) {
        try validatePath(value);
        @memcpy(self.pax_link[0..value.len], value);
        self.pax_link_len = value.len;
      } else if (std.mem.eql(u8, key, "size")) {
        self.pax_size = std.fmt.parseInt(u64, value, 10) catch return error.InvalidTarHeader;
      }
      records = records[len..];
    }
  }

  fn finishData(self: *TarParser) void {
    const padding = (512 - (self.entry_size % 512)) % 512;
    self.skip_bytes = @intCast(padding);
    self.state = if (padding > 0) .skip_padding else .read_header;
  }

  pub fn feed(self: *TarParser, data: []const u8) ParseResult {
    switch (self.state) {
      .read_header => {
        const needed = @sizeOf(TarHeader) - self.header_bytes_read;
        const to_copy = @min(needed, data.len);

        const header_bytes: *[512]u8 = @ptrCast(&self.header);
        @memcpy(header_bytes[self.header_bytes_read..][0..to_copy], data[0..to_copy]);
        self.header_bytes_read += to_copy;

        if (self.header_bytes_read < @sizeOf(TarHeader)) {
          return .{ .kind = .need_more_data, .consumed = to_copy };
        } self.header_bytes_read = 0;

        if (self.header.isZero()) {
          return .{ .kind = .end_of_archive, .consumed = to_copy };
        }
        const header_size = self.header.getSize() catch return .{ .kind = .{ .err = error.InvalidTarHeader }, .consumed = to_copy };
        if (self.header.typeflag == 'x') {
          if (header_size > self.pax_data.len) return .{ .kind = .{ .err = error.UnsupportedFormat }, .consumed = to_copy };
          self.pax_len = 0;
          self.entry_size = header_size;
          self.current_file_remaining = header_size;
          self.state = if (header_size > 0) .read_pax_data else .read_header;
          return .{ .kind = .need_more_data, .consumed = to_copy };
        }
        // Do not silently extract unsupported metadata using a truncated name.
        if (!self.header.isFile() and !self.header.isDirectory() and !self.header.isSymlink()) {
          return .{ .kind = .{ .err = error.UnsupportedFormat }, .consumed = to_copy };
        }
        var path = if (self.pax_path_len > 0) self.pax_path[0..self.pax_path_len] else self.header.getName(&self.path_buf) catch {
          return .{ .kind = .{ .err = ExtractError.InvalidPath }, .consumed = to_copy };
        };

        if (!self.prefix_detected and self.header.isDirectory()) {
          var prefix_len = @min(path.len, 127);
          @memcpy(self.strip_prefix[0..prefix_len], path[0..prefix_len]);
          if (prefix_len > 0 and self.strip_prefix[prefix_len - 1] != '/') {
            self.strip_prefix[prefix_len] = '/';
            prefix_len += 1;
          }
          self.strip_prefix_len = prefix_len;
          self.prefix_detected = true;
        }

        const prefix = self.strip_prefix[0..self.strip_prefix_len];
        if (std.mem.startsWith(u8, path, prefix)) {
          path = path[self.strip_prefix_len..];
        }

        if (path.len > 0) validatePath(path) catch {
          return .{ .kind = .{ .err = ExtractError.InvalidPath }, .consumed = to_copy };
        };

        const size = self.pax_size orelse header_size;
        self.pax_path_len = 0;
        self.pax_size = null;
        self.entry_size = size;
        const mode = self.header.getMode() catch return .{ .kind = .{ .err = ExtractError.InvalidTarHeader }, .consumed = to_copy };

        const entry_type: Entry.Type = if (self.header.isDirectory()) .directory
        else if (self.header.isSymlink()) .symlink
        else .file;

        self.current_file_remaining = size;
        if (size > 0) {
          self.state = .read_file_data;
        } else self.state = .read_header;

        const entry: Entry = .{
          .path = path,
          .mode = mode,
          .size = size,
          .entry_type = entry_type,
          .link_target = if (self.pax_link_len > 0) self.pax_link[0..self.pax_link_len]
            else std.mem.sliceTo(&self.header.linkname, 0),
        };
        
        self.pax_link_len = 0;
        return .{ .consumed = to_copy, .kind = .{ .entry = entry } };
      },

      .read_file_data => {
        const to_read: usize = @min(self.current_file_remaining, data.len);
        self.current_file_remaining -= to_read;

        if (self.current_file_remaining == 0) self.finishData();

        return .{ .kind = .{ .file_data = data[0..to_read] }, .consumed = to_read };
      },

      .read_pax_data => {
        const to_read: usize = @intCast(@min(self.current_file_remaining, data.len));
        @memcpy(self.pax_data[self.pax_len..][0..to_read], data[0..to_read]);
        self.pax_len += to_read;
        self.current_file_remaining -= to_read;
        if (self.current_file_remaining == 0) {
          self.parsePax() catch |err| return .{ .kind = .{ .err = err }, .consumed = to_read };
          self.finishData();
        }
        return .{ .kind = .need_more_data, .consumed = to_read };
      },

      .skip_padding => {
        const to_skip = @min(self.skip_bytes, data.len);
        self.skip_bytes -= to_skip;

        if (self.skip_bytes == 0) {
          self.state = .read_header;
        }

        if (data.len > to_skip) {
          const next = self.feed(data[to_skip..]);
          return .{ .kind = next.kind, .consumed = to_skip + next.consumed };
        }
        return .{ .kind = .need_more_data, .consumed = to_skip };
      },
    }
  }

  pub fn reset(self: *TarParser) void { self.* = TarParser.init(self.strip_prefix[0..self.strip_prefix_len]); }
};

pub const Extractor = struct {
  allocator: std.mem.Allocator,
  output_dir: std.Io.Dir,
  parser: TarParser,
  decompressor: *GzipDecompressor,
  current_file: ?std.Io.File,
  current_file_path: [4096]u8,
  current_file_path_len: usize,
  current_file_mode: u32,
  files_extracted: u32,
  bytes_extracted: u64,
  gzip_finished: bool,
  archive_finished: bool,

  pub fn init(allocator: std.mem.Allocator, output_path: []const u8) !*Extractor {
    const extractor = try allocator.create(Extractor);
    errdefer allocator.destroy(extractor);

    std.Io.Dir.cwd().createDirPath(io, output_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };

    const decompressor = try GzipDecompressor.init(allocator);
    errdefer decompressor.deinit();

    extractor.* = .{
      .allocator = allocator,
      .output_dir = try std.Io.Dir.cwd().openDir(io, output_path, .{}),
      .parser = TarParser.init("package/"),
      .decompressor = decompressor,
      .current_file = null,
      .current_file_path = undefined,
      .current_file_path_len = 0,
      .current_file_mode = 0o644,
      .files_extracted = 0,
      .bytes_extracted = 0,
      .gzip_finished = false,
      .archive_finished = false,
    };

    return extractor;
  }

  pub fn deinit(self: *Extractor) void {
    if (self.current_file) |f| {
      f.close(io);
      self.applyFileMode();
    }
    self.output_dir.close(io);
    self.decompressor.deinit();
    self.allocator.destroy(self);
  }

  fn applyFileMode(self: *Extractor) void {
    if (self.current_file_path_len == 0) return;

    if (comptime builtin.os.tag != .windows) {
      if (self.current_file_mode & 0o111 != 0) {
        const path = self.current_file_path[0..self.current_file_path_len];
        var path_buf: [4097]u8 = undefined;
        @memcpy(path_buf[0..path.len], path);
        path_buf[path.len] = 0;
        const path_z: [*:0]const u8 = path_buf[0..path.len :0];
        _ = std.c.fchmodat(self.output_dir.handle, path_z, @intCast(self.current_file_mode & 0o777), 0);
      }
    }
    self.current_file_path_len = 0;
  }

  pub fn feedCompressed(self: *Extractor, data: []const u8) !void {
    if (try self.decompressor.decompress(data, handleDecompressed, self)) {
      self.gzip_finished = true;
    }
  }

  fn handleDecompressed(data: []const u8, user_data: ?*anyopaque) !void {
    const self: *Extractor = @ptrCast(@alignCast(user_data));
    try self.feedTar(data);
  }

  pub fn feedTar(self: *Extractor, data: []const u8) !void {
    var remaining = data;
    while (remaining.len > 0) {
      const result = self.parser.feed(remaining);
      remaining = remaining[result.consumed..];
      switch (result.kind) {
        .need_more_data => if (result.consumed == 0) return,
        .entry => |entry| try self.handleEntry(entry),
        .file_data => |d| try self.writeFileData(d),
        .end_of_archive => {
          self.archive_finished = true;
          return self.closeCurrentFile();
        },
        .err => |e| return e,
      }
    }
  }
  
  inline fn handleEntry(self: *Extractor, entry: TarParser.Entry) !void {
    if (entry.path.len == 0) return;
    switch (entry.entry_type) {
      .directory => self.output_dir.createDirPath(io, entry.path) catch {},
      .file => try self.createFile(entry),
      .symlink => self.createSymlink(entry) catch {},
    }
  }
  
  inline fn createFile(self: *Extractor, entry: TarParser.Entry) !void {
    self.closeCurrentFile();
    if (std.fs.path.dirname(entry.path)) |dir| {
      try self.output_dir.createDirPath(io, dir);
    }
    const already_exists = blk: {
      self.output_dir.access(io, entry.path, .{}) catch break :blk false;
      break :blk true;
    };
    self.current_file = try self.output_dir.createFile(io, entry.path, .{});
    const len = entry.path.len;
    @memcpy(self.current_file_path[0..len], entry.path[0..len]);
    self.current_file_path_len = len;
    self.current_file_mode = entry.mode;
    if (!already_exists) self.files_extracted += 1;
  }
  
  inline fn createSymlink(self: *Extractor, entry: TarParser.Entry) !void {
    const target = entry.link_target;
    
    if (entry.path.len == 0 or target.len == 0) return;
    try validatePath(target);
    
    if (std.fs.path.dirname(entry.path)) |dir| {
      try self.output_dir.createDirPath(io, dir);
    }
    
    self.output_dir.deleteFile(io, entry.path) catch {};
    try linker.createSymlinkOrCopy(self.output_dir, target, entry.path);
  }
  
  inline fn writeFileData(self: *Extractor, data: []const u8) !void {
    if (self.current_file) |f| {
      try f.writeStreamingAll(io, data);
      self.bytes_extracted += data.len;
    }
  }
  
  inline fn closeCurrentFile(self: *Extractor) void {
    if (self.current_file) |f| {
      f.close(io);
      self.applyFileMode();
      self.current_file = null;
    }
  }

  pub fn stats(self: *const Extractor) struct { files: u32, bytes: u64 } {
    return .{
      .files = self.files_extracted,
      .bytes = self.bytes_extracted,
    };
  }

  pub fn isComplete(self: *const Extractor) bool {
    return self.gzip_finished and self.archive_finished and self.files_extracted > 0;
  }
};

const TestTar = struct {
  bytes: [16384]u8 = @splat(0),
  len: usize = 0,

  fn add(self: *TestTar, name: []const u8, kind: u8, body: []const u8) !void {
    var header = std.mem.zeroes(TarHeader);
    @memcpy(header.name[0..@min(name.len, 100)], name[0..@min(name.len, 100)]);
    _ = try std.fmt.bufPrint(&header.size, "{o:0>11}", .{body.len});
    _ = try std.fmt.bufPrint(&header.mode, "{o:0>7}", .{@as(u32, 0o644)});
    header.typeflag = kind;
    @memcpy(self.bytes[self.len..][0..512], std.mem.asBytes(&header));
    self.len += 512;
    @memcpy(self.bytes[self.len..][0..body.len], body);
    self.len += (body.len + 511) / 512 * 512;
  }

  fn paxRecord(buf: []u8, key: []const u8, value: []const u8) ![]const u8 {
    var len = key.len + value.len + 4;
    while (true) {
      const record = try std.fmt.bufPrint(buf, "{d} {s}={s}\n", .{ len, key, value });
      if (record.len == len) return record;
      len = record.len;
    }
  }
};

test "PAX source map paths survive arbitrary input boundaries and apply once" {
  const name = "getchatcompletionfieldoptionscountsv1observabilitychatcompletionfieldsfieldnameoptionscountspost.js";
  var tar = TestTar{};
  try tar.add(name, '0', "module.exports = 1;");
  var record_buf: [512]u8 = undefined;
  try tar.add("PaxHeader/map", 'x', try TestTar.paxRecord(&record_buf, "path", "package/" ++ name ++ ".map"));
  // npm's fallback header truncates .map away, colliding with the JS file.
  try tar.add(name, '0', "{\"version\":3}");
  try tar.add("package/after.js", '0', "after");
  const paths = [_][]const u8{ name, name ++ ".map", "after.js" };
  const bodies = [_][]const u8{ "module.exports = 1;", "{\"version\":3}", "after" };
  for ([_]usize{ 1, 7, 511, 512, 513, 16384 }) |chunk_size| {
    var parser = TarParser.init("package/");
    var offset: usize = 0;
    var count: usize = 0;
    var body_offset: usize = 0;
    while (offset < tar.len) {
      const result = parser.feed(tar.bytes[offset..@min(offset + chunk_size, tar.len)]);
      try std.testing.expect(result.consumed > 0);
      offset += result.consumed;
      switch (result.kind) {
        .entry => |entry| {
          if (count > 0) try std.testing.expectEqual(bodies[count - 1].len, body_offset);
          try std.testing.expect(count < paths.len);
          try std.testing.expectEqualStrings(paths[count], entry.path);
          try std.testing.expectEqual(bodies[count].len, entry.size);
          body_offset = 0;
          count += 1;
        },
        .file_data => |data| {
          try std.testing.expect(count > 0);
          try std.testing.expectEqualStrings(bodies[count - 1][body_offset..][0..data.len], data);
          body_offset += data.len;
        },
        .need_more_data => {},
        else => return error.UnexpectedParseResult,
      }
    }
    try std.testing.expectEqual(paths.len, count);
    try std.testing.expectEqual(bodies[count - 1].len, body_offset);
  }
}

test "PAX rejects malformed records and unsafe paths" {
  for ([_][]const u8{ "999 path=a\n", "0 path=a\n", "12 path=abc!", "12 missing=\n" }) |bad| {
    // The last record is a valid unknown key and must be ignored.
    var parser = TarParser.init("package/");
    @memcpy(parser.pax_data[0..bad.len], bad);
    parser.pax_len = bad.len;
    if (std.mem.eql(u8, bad, "12 missing=\n")) {
      try parser.parsePax();
    } else try std.testing.expectError(error.InvalidTarHeader, parser.parsePax());
  }
  for ([_][]const u8{ "../escape", "package/../../escape", "/absolute", "package/a\\b", "package/a\x00b" }) |path| {
    var parser = TarParser.init("package/");
    const record = try TestTar.paxRecord(&parser.pax_data, "path", path);
    parser.pax_len = record.len;
    try std.testing.expectError(error.InvalidPath, parser.parsePax());
  }
}

test "PAX size and linkpath override one entry" {
  var tar = TestTar{};
  var buf: [512]u8 = undefined;
  const first = try TestTar.paxRecord(&buf, "linkpath", "long/target");
  const first_len = first.len;
  const second = try TestTar.paxRecord(buf[first_len..], "size", "3");
  try tar.add("PaxHeader/link", 'x', buf[0 .. first_len + second.len]);
  const entry_offset = tar.len;
  try tar.add("package/link", '2', "abc");
  // The PAX size controls both data consumption and padding.
  @memcpy(tar.bytes[entry_offset + 124 ..][0..11], "00000000000");
  try tar.add("package/next", '0', "");
  var parser = TarParser.init("package/");
  var offset: usize = 0;
  var count: usize = 0;
  while (offset < tar.len) {
    const result = parser.feed(tar.bytes[offset..tar.len]);
    try std.testing.expect(result.consumed > 0);
    offset += result.consumed;
    switch (result.kind) {
      .entry => |entry| {
        try std.testing.expectEqualStrings(if (count == 0) "long/target" else "", entry.link_target);
        try std.testing.expectEqual(@as(u64, if (count == 0) 3 else 0), entry.size);
        count += 1;
      },
      .file_data => |data| try std.testing.expectEqualStrings("abc", data),
      .need_more_data => {},
      else => return error.UnexpectedParseResult,
    }
  }
  try std.testing.expectEqual(@as(usize, 2), count);
}

test "PAX accepts paths beyond the legacy buffer and rejects oversized metadata" {
  var parser = TarParser.init("package/");
  var path: [300]u8 = @splat('a');
  path[100] = '/';
  path[200] = '/';
  const record = try TestTar.paxRecord(&parser.pax_data, "path", &path);
  parser.pax_len = record.len;
  try parser.parsePax();
  try std.testing.expectEqualStrings(&path, parser.pax_path[0..parser.pax_path_len]);

  var tar = TestTar{};
  try tar.add("PaxHeader/large", 'x', "");
  @memcpy(tar.bytes[124..][0..11], "00000200001"); // 64 KiB + 1
  parser = TarParser.init("package/");
  const result = parser.feed(tar.bytes[0..512]);
  try std.testing.expectEqual(error.UnsupportedFormat, result.kind.err);
  for ([_]u8{ 'g', 'L', 'K' }) |kind| {
    tar.bytes[156] = kind;
    parser = TarParser.init("package/");
    const unsupported = parser.feed(tar.bytes[0..512]);
    try std.testing.expectEqual(error.UnsupportedFormat, unsupported.kind.err);
  }
}
