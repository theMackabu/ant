const std = @import("std");
const builtin = @import("builtin");

pub const MAGIC: u32 = 0x504B474C;
pub const VERSION: u32 = 1;

pub const StringRef = extern struct {
  offset: u32,
  len: u32,

  pub fn slice(self: StringRef, string_table: []const u8) []const u8 {
    if (self.offset >= string_table.len) return "";
    const end = @min(self.offset + self.len, @as(u32, @intCast(string_table.len)));
    return string_table[self.offset..end];
  }

  pub const empty: StringRef = .{ .offset = 0, .len = 0 };
};

pub const Header = extern struct {
  magic: u32 = MAGIC,
  version: u32 = VERSION,
  package_count: u32 = 0,
  dependency_count: u32 = 0,
  string_table_offset: u32 = 0,
  string_table_size: u32 = 0,
  package_array_offset: u32 = 0,
  dependency_array_offset: u32 = 0,
  hash_table_offset: u32 = 0,
  hash_table_size: u32 = 0,
  _reserved: [24]u8 = [_]u8{0} ** 24,

  pub fn validate(self: *const Header) bool {
    return self.magic == MAGIC and self.version == VERSION;
  }
};

pub const PackageFlags = packed struct(u8) {
  dev: bool = false,
  optional: bool = false,
  peer: bool = false,
  bundled: bool = false,
  has_bin: bool = false,
  has_scripts: bool = false,
  direct: bool = false,
  _reserved: u1 = 0,
};

pub const Package = extern struct {
  name: StringRef,
  version_major: u64,
  version_minor: u64,
  version_patch: u64,
  prerelease: StringRef,
  integrity: [64]u8,
  tarball_url: StringRef,
  parent_path: StringRef,
  deps_start: u32,
  deps_count: u32,
  flags: PackageFlags,
  _padding: [3]u8 = .{ 0, 0, 0 },

  comptime {
    std.debug.assert(@sizeOf(Package) == 136);
  }

  pub fn versionString(self: *const Package, allocator: std.mem.Allocator, string_table: []const u8) ![]u8 {
    const prerelease_str = self.prerelease.slice(string_table);
    if (prerelease_str.len > 0) {
      return std.fmt.allocPrint(allocator, "{d}.{d}.{d}-{s}", .{
        self.version_major,
        self.version_minor,
        self.version_patch,
        prerelease_str,
      });
    }
    return std.fmt.allocPrint(allocator, "{d}.{d}.{d}", .{
      self.version_major,
      self.version_minor,
      self.version_patch,
    });
  }

  pub fn integrityHex(self: *const Package) [128]u8 {
    return std.fmt.bytesToHex(self.integrity, .lower);
  }

  pub fn installPath(self: *const Package, allocator: std.mem.Allocator, string_table: []const u8) ![]u8 {
    const parent = self.parent_path.slice(string_table);
    const name = self.name.slice(string_table);
    if (parent.len > 0) return std.fmt.allocPrint(allocator, "{s}/node_modules/{s}", .{ parent, name });
    return allocator.dupe(u8, name);
  }

  pub fn isNested(self: *const Package) bool {
    return self.parent_path.len > 0;
  }
};

pub const DependencyFlags = packed struct(u8) {
  peer: bool = false,
  dev: bool = false,
  optional: bool = false,
  _reserved: u5 = 0,
};

pub const Dependency = extern struct {
  package_index: u32,
  constraint: StringRef,
  flags: DependencyFlags = .{},
  _padding: [3]u8 = .{ 0, 0, 0 },
};

pub const HashBucket = extern struct {
  name_hash: u32,
  package_index: u32,
  pub const empty: HashBucket = .{ 
    .name_hash = 0,
    .package_index = std.math.maxInt(u32)
  };
};

pub fn djb2Hash(str: []const u8) u32 {
  var hash: u32 = 5381;
  for (str) |c| hash = ((hash << 5) +% hash) +% c;
  return hash;
}

pub const Lockfile = struct {
  data: 
    if (builtin.os.tag == .windows) []align(@alignOf(Header)) u8 
    else []align(std.heap.page_size_min) const u8,
  header: *const Header,
  string_table: []const u8,
  packages: []const Package,
  dependencies: []const Dependency,
  hash_table: []const HashBucket,

  pub fn open(path: []const u8) !Lockfile {
    const file = try std.fs.cwd().openFile(path, .{});
    defer file.close();
    
    const stat = try file.stat();
    if (stat.size < @sizeOf(Header)) {
      return error.InvalidLockfile;
    }
    
    if (comptime builtin.os.tag == .windows) {
      const data = try std.heap.c_allocator.alignedAlloc(u8, std.mem.Alignment.fromByteUnits(@alignOf(Header)), stat.size);
      errdefer std.heap.c_allocator.free(data);
      
      const bytes_read = try file.readAll(data);
      if (bytes_read != stat.size) {
        std.heap.c_allocator.free(data);
        return error.InvalidLockfile;
      }
      
      return initFromDataWindows(data);
    } else {
      const data = try std.posix.mmap(
        null, stat.size,
        std.posix.PROT.READ,
        .{ .TYPE = .PRIVATE },
        file.handle, 0,
      );
      
      return initFromData(data);
    }
  }

  fn initFromDataWindows(data: []align(@alignOf(Header)) u8) !Lockfile {
    if (data.len < @sizeOf(Header)) return error.InvalidLockfile;

    const header: *const Header = @ptrCast(@alignCast(data.ptr));
    if (!header.validate()) return error.InvalidLockfile;

    if (header.string_table_offset + header.string_table_size > data.len or
      header.package_array_offset + header.package_count * @sizeOf(Package) > data.len or
      header.dependency_array_offset + header.dependency_count * @sizeOf(Dependency) > data.len or
      header.hash_table_offset + header.hash_table_size * @sizeOf(HashBucket) > data.len)
    { return error.InvalidLockfile; }

    return .{
      .data = data,
      .header = header,
      .string_table = data[header.string_table_offset..][0..header.string_table_size],
      .packages = @as([*]const Package, @ptrCast(@alignCast(data.ptr + header.package_array_offset)))[0..header.package_count],
      .dependencies = @as([*]const Dependency, @ptrCast(@alignCast(data.ptr + header.dependency_array_offset)))[0..header.dependency_count],
      .hash_table = @as([*]const HashBucket, @ptrCast(@alignCast(data.ptr + header.hash_table_offset)))[0..header.hash_table_size],
    };
  }

  pub fn initFromData(data: []align(std.heap.page_size_min) const u8) !Lockfile {
    if (data.len < @sizeOf(Header)) {
      return error.InvalidLockfile;
    }

    const header: *const Header = @ptrCast(@alignCast(data.ptr));
    if (!header.validate()) {
      return error.InvalidLockfile;
    }

    if (header.string_table_offset + header.string_table_size > data.len or
      header.package_array_offset + header.package_count * @sizeOf(Package) > data.len or
      header.dependency_array_offset + header.dependency_count * @sizeOf(Dependency) > data.len or
      header.hash_table_offset + header.hash_table_size * @sizeOf(HashBucket) > data.len)
    { return error.InvalidLockfile; }

    const string_table = data[header.string_table_offset..][0..header.string_table_size];

    const packages_ptr: [*]const Package = @ptrCast(@alignCast(data.ptr + header.package_array_offset));
    const packages = packages_ptr[0..header.package_count];

    const deps_ptr: [*]const Dependency = @ptrCast(@alignCast(data.ptr + header.dependency_array_offset));
    const dependencies = deps_ptr[0..header.dependency_count];

    const hash_ptr: [*]const HashBucket = @ptrCast(@alignCast(data.ptr + header.hash_table_offset));
    const hash_table = hash_ptr[0..header.hash_table_size];

    return .{
      .data = data,
      .header = header,
      .string_table = string_table,
      .packages = packages,
      .dependencies = dependencies,
      .hash_table = hash_table,
    };
  }

  pub fn close(self: *Lockfile) void {
    if (comptime builtin.os.tag == .windows) {
      std.heap.c_allocator.free(self.data);
    } else std.posix.munmap(self.data);
    self.* = undefined;
  }

  pub fn lookupPackage(self: *const Lockfile, name: []const u8) ?*const Package {
    if (self.hash_table.len == 0) return null;

    const hash = djb2Hash(name);
    var index = hash % @as(u32, @intCast(self.hash_table.len));
    var probes: u32 = 0;

    while (probes < self.hash_table.len) : (probes += 1) {
      const bucket = &self.hash_table[index];
      if (bucket.package_index == std.math.maxInt(u32)) return null;
      if (bucket.name_hash == hash) {
        const pkg = &self.packages[bucket.package_index];
        const pkg_name = pkg.name.slice(self.string_table);
        if (std.mem.eql(u8, pkg_name, name)) return pkg;
      }
      index = (index + 1) % @as(u32, @intCast(self.hash_table.len));
    }
    return null;
  }

  pub fn getPackageDeps(self: *const Lockfile, pkg: *const Package) []const Dependency {
    if (pkg.deps_count == 0) return &[_]Dependency{};
    return self.dependencies[pkg.deps_start..][0..pkg.deps_count];
  }
};

pub const LockfileWriter = struct {
  allocator: std.mem.Allocator,
  packages: std.ArrayListUnmanaged(Package),
  dependencies: std.ArrayListUnmanaged(Dependency),
  string_builder: std.ArrayListUnmanaged(u8),
  string_offsets: std.StringHashMap(StringRef),

  pub fn init(allocator: std.mem.Allocator) LockfileWriter {
    return .{
      .allocator = allocator,
      .packages = .{},
      .dependencies = .{},
      .string_builder = .{},
      .string_offsets = std.StringHashMap(StringRef).init(allocator),
    };
  }

  pub fn deinit(self: *LockfileWriter) void {
    self.packages.deinit(self.allocator);
    self.dependencies.deinit(self.allocator);
    self.string_builder.deinit(self.allocator);
    var key_iter = self.string_offsets.keyIterator();
    while (key_iter.next()) |key| {
      self.allocator.free(key.*);
    }
    self.string_offsets.deinit();
  }

  pub fn internString(self: *LockfileWriter, str: []const u8) !StringRef {
    if (str.len == 0) return StringRef.empty;
    if (self.string_offsets.get(str)) |ref| return ref;

    const offset: u32 = @intCast(self.string_builder.items.len);
    try self.string_builder.appendSlice(self.allocator, str);

    const ref = StringRef{ .offset = offset, .len = @intCast(str.len) };
    const stored_str = try self.allocator.dupe(u8, str);
    
    errdefer self.allocator.free(stored_str);
    try self.string_offsets.put(stored_str, ref);

    return ref;
  }

  pub fn addPackage(self: *LockfileWriter, pkg: Package) !u32 {
    const index: u32 = @intCast(self.packages.items.len);
    try self.packages.append(self.allocator, pkg);
    return index;
  }

  pub fn addDependency(self: *LockfileWriter, dep: Dependency) !void {
    try self.dependencies.append(self.allocator, dep);
  }

  fn alignOffset(offset: u32, alignment: u32) u32 {
    const rem = offset % alignment;
    return if (rem == 0) offset else offset + (alignment - rem);
  }

  pub fn write(self: *LockfileWriter, path: []const u8) !void {
    const file = try std.fs.cwd().createFile(path, .{});
    defer file.close();

    const header_size: u32 = @sizeOf(Header);
    const string_table_offset = header_size;
    const string_table_size: u32 = @intCast(self.string_builder.items.len);

    const package_array_offset = alignOffset(string_table_offset + string_table_size, @alignOf(Package));
    const package_pad_size = package_array_offset - (string_table_offset + string_table_size);
    const package_array_size: u32 = @intCast(self.packages.items.len * @sizeOf(Package));

    const dependency_array_offset = alignOffset(package_array_offset + package_array_size, @alignOf(Dependency));
    const dep_pad_size = dependency_array_offset - (package_array_offset + package_array_size);
    const dependency_array_size: u32 = @intCast(self.dependencies.items.len * @sizeOf(Dependency));

    const hash_table_size: u32 = @intCast(@max(1, self.packages.items.len * 10 / 7));
    var hash_table = try self.allocator.alloc(HashBucket, hash_table_size);
    defer self.allocator.free(hash_table);

    for (hash_table) |*bucket| {
      bucket.* = HashBucket.empty;
    }

    for (self.packages.items, 0..) |pkg, i| {
      const name = pkg.name.slice(self.string_builder.items);
      const hash = djb2Hash(name);
      var index = hash % hash_table_size;

      while (hash_table[index].package_index != std.math.maxInt(u32)) {
        index = (index + 1) % hash_table_size;
      }

      hash_table[index] = .{
        .name_hash = hash,
        .package_index = @intCast(i),
      };
    }

    const hash_table_offset = alignOffset(dependency_array_offset + dependency_array_size, @alignOf(HashBucket));
    const hash_pad_size = hash_table_offset - (dependency_array_offset + dependency_array_size);

    const header = Header{
      .package_count = @intCast(self.packages.items.len),
      .dependency_count = @intCast(self.dependencies.items.len),
      .string_table_offset = string_table_offset,
      .string_table_size = string_table_size,
      .package_array_offset = package_array_offset,
      .dependency_array_offset = dependency_array_offset,
      .hash_table_offset = hash_table_offset,
      .hash_table_size = hash_table_size,
    };

    try file.writeAll(std.mem.asBytes(&header));
    try file.writeAll(self.string_builder.items);
    
    if (package_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeAll(padding[0..package_pad_size]);
    } try file.writeAll(std.mem.sliceAsBytes(self.packages.items));
    
    if (dep_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeAll(padding[0..dep_pad_size]);
    } try file.writeAll(std.mem.sliceAsBytes(self.dependencies.items));
    
    if (hash_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeAll(padding[0..hash_pad_size]);
    } try file.writeAll(std.mem.sliceAsBytes(hash_table));
  }
};
