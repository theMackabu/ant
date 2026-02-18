const std = @import("std");
const builtin = @import("builtin");
const linker = @import("linker.zig");

const c = @cImport({
  @cInclude("zlib-ng.h");
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
    const size_str = std.mem.trimRight(u8, &self.size, &[_]u8{ 0, ' ' });
    return std.fmt.parseInt(u64, size_str, 8) catch return error.InvalidTarHeader;
  }

  pub fn getMode(self: *const TarHeader) !u32 {
    const mode_str = std.mem.trimRight(u8, &self.mode, &[_]u8{ 0, ' ' });
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
  stream: c.zng_stream,
  initialized: bool,
  allocator: std.mem.Allocator,

  pub fn init(allocator: std.mem.Allocator) !*GzipDecompressor {
    const self = try allocator.create(GzipDecompressor);
    errdefer allocator.destroy(self);

    self.allocator = allocator;
    self.stream = std.mem.zeroes(c.zng_stream);
    self.initialized = false;

    const ret = c.zng_inflateInit2(&self.stream, 15 + 32);
    if (ret != c.Z_OK) {
      allocator.destroy(self);
      return error.DecompressionFailed;
    }

    self.initialized = true;
    return self;
  }

  pub fn deinit(self: *GzipDecompressor) void {
    if (self.initialized) _ = c.zng_inflateEnd(&self.stream);
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

      const ret = c.zng_inflate(&self.stream, c.Z_NO_FLUSH);

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
  path_buf: [256]u8,

  const State = enum {
    read_header,
    read_file_data,
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
        } var path = self.header.getName(&self.path_buf) catch {
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

        const size = self.header.getSize() catch return .{ .kind = .{ .err = ExtractError.InvalidTarHeader }, .consumed = to_copy };
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
        };
        
        return .{ .consumed = to_copy, .kind = .{ .entry = entry } };
      },

      .read_file_data => {
        const to_read: usize = @min(self.current_file_remaining, data.len);
        self.current_file_remaining -= to_read;

        if (self.current_file_remaining == 0) {
          const size = self.header.getSize() catch return .{ .kind = .{ .err = ExtractError.InvalidTarHeader }, .consumed = to_read };
          const padding = (512 - (size % 512)) % 512;
          if (padding > 0) {
            self.skip_bytes = @intCast(padding);
            self.state = .skip_padding;
          } else self.state = .read_header;
        }

        return .{ .kind = .{ .file_data = data[0..to_read] }, .consumed = to_read };
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
  output_dir: std.fs.Dir,
  parser: TarParser,
  decompressor: *GzipDecompressor,
  current_file: ?std.fs.File,
  current_file_path: [256]u8,
  current_file_path_len: usize,
  current_file_mode: u32,
  files_extracted: u32,
  bytes_extracted: u64,

  pub fn init(allocator: std.mem.Allocator, output_path: []const u8) !*Extractor {
    const extractor = try allocator.create(Extractor);
    errdefer allocator.destroy(extractor);

    std.fs.cwd().makePath(output_path) catch |err| switch (err) {
      error.PathAlreadyExists => {},
      else => return error.IoError,
    };

    const decompressor = try GzipDecompressor.init(allocator);
    errdefer decompressor.deinit();

    extractor.* = .{
      .allocator = allocator,
      .output_dir = try std.fs.cwd().openDir(output_path, .{}),
      .parser = TarParser.init("package/"),
      .decompressor = decompressor,
      .current_file = null,
      .current_file_path = undefined,
      .current_file_path_len = 0,
      .current_file_mode = 0o644,
      .files_extracted = 0,
      .bytes_extracted = 0,
    };

    return extractor;
  }

  pub fn deinit(self: *Extractor) void {
    if (self.current_file) |f| {
      f.close();
      self.applyFileMode();
    }
    self.output_dir.close();
    self.decompressor.deinit();
    self.allocator.destroy(self);
  }

  fn applyFileMode(self: *Extractor) void {
    if (self.current_file_path_len == 0) return;

    if (comptime builtin.os.tag != .windows) {
      if (self.current_file_mode & 0o111 != 0) {
        const path = self.current_file_path[0..self.current_file_path_len];
        var path_buf: [257]u8 = undefined;
        @memcpy(path_buf[0..path.len], path);
        path_buf[path.len] = 0;
        const path_z: [*:0]const u8 = path_buf[0..path.len :0];
        _ = std.c.fchmodat(self.output_dir.fd, path_z, @intCast(self.current_file_mode & 0o777), 0);
      }
    }
    self.current_file_path_len = 0;
  }

  pub fn feedCompressed(self: *Extractor, data: []const u8) !void {
    _ = try self.decompressor.decompress(data, handleDecompressed, self);
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
        .need_more_data => return,
        .entry => |entry| try self.handleEntry(entry),
        .file_data => |d| try self.writeFileData(d),
        .end_of_archive => return self.closeCurrentFile(),
        .err => |e| return e,
      }
    }
  }
  
  inline fn handleEntry(self: *Extractor, entry: TarParser.Entry) !void {
    if (entry.path.len == 0) return;
    switch (entry.entry_type) {
      .directory => self.output_dir.makePath(entry.path) catch {},
      .file => try self.createFile(entry),
      .symlink => self.createSymlink(entry) catch {},
    }
  }
  
  inline fn createFile(self: *Extractor, entry: TarParser.Entry) !void {
    self.closeCurrentFile();
    if (std.fs.path.dirname(entry.path)) |dir| {
      try self.output_dir.makePath(dir);
    }
    self.current_file = try self.output_dir.createFile(entry.path, .{});
    const len = @min(entry.path.len, 256);
    @memcpy(self.current_file_path[0..len], entry.path[0..len]);
    self.current_file_path_len = len;
    self.current_file_mode = entry.mode;
    self.files_extracted += 1;
  }
  
  inline fn createSymlink(self: *Extractor, entry: TarParser.Entry) !void {
    const linkname_len = std.mem.indexOfScalar(u8, &self.parser.header.linkname, 0) orelse self.parser.header.linkname.len;
    const target = self.parser.header.linkname[0..linkname_len];
    
    if (entry.path.len == 0 or target.len == 0) return;
    try validatePath(target);
    
    if (std.fs.path.dirname(entry.path)) |dir| {
      try self.output_dir.makePath(dir);
    }
    
    self.output_dir.deleteFile(entry.path) catch {};
    try linker.createSymlinkOrCopy(self.output_dir, target, entry.path);
  }
  
  inline fn writeFileData(self: *Extractor, data: []const u8) !void {
    if (self.current_file) |f| {
      try f.writeAll(data);
      self.bytes_extracted += data.len;
    }
  }
  
  inline fn closeCurrentFile(self: *Extractor) void {
    if (self.current_file) |f| {
      f.close();
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
};
