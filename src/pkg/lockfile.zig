const std = @import("std");
const io = std.Io.Threaded.global_single_threaded.io();
const builtin = @import("builtin");

pub const MAGIC: u32 = 0x504B474C;
pub const VERSION: u32 = 3;

fn platformHash(value: []const u8) u32 {
  return @truncate(std.hash.Wyhash.hash(0, value));
}

fn alignFileOffset(offset: u32, alignment: u32) u32 {
  const rem = offset % alignment;
  return if (rem == 0) offset else offset + (alignment - rem);
}

fn alignSectionOffset(offset: usize, alignment: usize) usize {
  const rem = offset % alignment;
  return if (rem == 0) offset else offset + (alignment - rem);
}

fn sectionEnd(data_len: usize, offset: u32, count: u32, elem_size: usize) ?usize {
  const start: usize = @intCast(offset);
  const n: usize = @intCast(count);
  const bytes = std.math.mul(usize, n, elem_size) catch return null;
  if (start > data_len or bytes > data_len - start) return null;
  return start + bytes;
}

pub const StringRef = extern struct {
  offset: u32,
  len: u32,

  pub fn slice(self: StringRef, string_table: []const u8) []const u8 {
    const offset: usize = self.offset;
    const len: usize = self.len;
    if (offset >= string_table.len) return "";
    const end = @min(offset + len, string_table.len);
    return string_table[offset..end];
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
  platform_os_hash: u32 = 0,
  platform_cpu_hash: u32 = 0,
  platform_abi_hash: u32 = 0,
  bin_entry_offset: u32 = 0,
  bin_entry_count: u32 = 0,
  disabled_dependency_count: u32 = 0,
  platform_entry_offset: u32 = 0,
  platform_entry_count: u32 = 0,
  graph_hash: u64 = 0,
  resolution_hash: u64 = 0,

  pub fn validate(self: *const Header) bool {
    return self.magic == MAGIC and self.version == VERSION;
  }

  pub fn matchesCurrentPlatform(self: *const Header) bool {
    const os_hash = platformHash(@tagName(builtin.os.tag));
    const cpu_hash = platformHash(@tagName(builtin.cpu.arch));
    const abi_hash = platformHash(@tagName(builtin.abi));
    return (self.platform_os_hash == 0 or self.platform_os_hash == os_hash) and
      (self.platform_cpu_hash == 0 or self.platform_cpu_hash == cpu_hash) and
      (self.platform_abi_hash == 0 or self.platform_abi_hash == abi_hash);
  }

  pub fn hasInstallMetadata(self: *const Header) bool {
    return self.platform_os_hash != 0 and self.platform_cpu_hash != 0 and self.platform_abi_hash != 0;
  }

  comptime {
    std.debug.assert(@sizeOf(Header) == 88);
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
  file_count_bytes: [3]u8 = .{ 0, 0, 0 },

  comptime {
    std.debug.assert(@sizeOf(Package) == 136);
  }

  pub fn fileCountBytes(count: u32) [3]u8 {
    const capped = @min(count, 0x00ff_ffff);
    return .{
      @intCast(capped & 0xff),
      @intCast((capped >> 8) & 0xff),
      @intCast((capped >> 16) & 0xff),
    };
  }

  pub fn fileCount(self: *const Package) u32 {
    return @as(u32, self.file_count_bytes[0]) |
      (@as(u32, self.file_count_bytes[1]) << 8) |
      (@as(u32, self.file_count_bytes[2]) << 16);
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

pub const BinEntry = extern struct {
  package_index: u32,
  name: StringRef,
  path: StringRef,
};

pub const DisabledDependency = extern struct {
  parent_package_index: u32,
  name: StringRef,
  constraint: StringRef,

  pub const root_parent = std.math.maxInt(u32);
};

pub const PlatformEntry = extern struct {
  package_index: u32,
  os: StringRef,
  cpu: StringRef,
  libc: StringRef,
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
  bin_entries: []const BinEntry,
  disabled_dependencies: []const DisabledDependency,
  platform_entries: []const PlatformEntry,

  pub fn open(path: []const u8) !Lockfile {
    const file = try std.Io.Dir.cwd().openFile(io, path, .{});
    defer file.close(io);
    
    const stat = try file.stat(io);
    if (stat.size < @sizeOf(Header)) {
      return error.InvalidLockfile;
    }
    
    if (comptime builtin.os.tag == .windows) {
      const data = try std.heap.c_allocator.alignedAlloc(u8, std.mem.Alignment.fromByteUnits(@alignOf(Header)), stat.size);
      errdefer std.heap.c_allocator.free(data);
      
      const bytes_read = try file.readPositionalAll(io, data, 0);
      if (bytes_read != stat.size) {
        std.heap.c_allocator.free(data);
        return error.InvalidLockfile;
      }
      
      return initFromDataWindows(data);
    } else {
      const data = try std.posix.mmap(
        null, stat.size,
        .{ .READ = true },
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

    if (sectionEnd(data.len, header.string_table_offset, header.string_table_size, 1) == null or
      sectionEnd(data.len, header.package_array_offset, header.package_count, @sizeOf(Package)) == null or
      sectionEnd(data.len, header.dependency_array_offset, header.dependency_count, @sizeOf(Dependency)) == null)
    { return error.InvalidLockfile; }

    const hash_end = sectionEnd(data.len, header.hash_table_offset, header.hash_table_size, @sizeOf(HashBucket)) orelse return error.InvalidLockfile;
    if (header.bin_entry_count > 0) {
      const bin_offset: usize = @intCast(header.bin_entry_offset);
      if (bin_offset < hash_end) return error.InvalidLockfile;
      _ = sectionEnd(data.len, header.bin_entry_offset, header.bin_entry_count, @sizeOf(BinEntry)) orelse return error.InvalidLockfile;
    } else if (header.bin_entry_offset != 0) {
      const bin_offset: usize = @intCast(header.bin_entry_offset);
      if (bin_offset < hash_end or bin_offset > data.len) return error.InvalidLockfile;
    }

    const bin_entries = if (header.bin_entry_count > 0)
      @as([*]const BinEntry, @ptrCast(@alignCast(data.ptr + header.bin_entry_offset)))[0..header.bin_entry_count]
    else
      &[_]BinEntry{};
    if (header.platform_entry_count > 0 and header.platform_entry_offset != 0) {
      _ = sectionEnd(data.len, header.platform_entry_offset, header.platform_entry_count, @sizeOf(PlatformEntry)) orelse return error.InvalidLockfile;
    }
    const disabled_dependencies = try disabledDependenciesFromDataWindows(data, header);
    const platform_entries = try platformEntriesFromDataWindows(data, header);
    if (!validateSideTables(header.package_count, bin_entries, disabled_dependencies, platform_entries)) {
      return error.InvalidLockfile;
    }

    return .{
      .data = data,
      .header = header,
      .string_table = data[header.string_table_offset..][0..header.string_table_size],
      .packages = @as([*]const Package, @ptrCast(@alignCast(data.ptr + header.package_array_offset)))[0..header.package_count],
      .dependencies = @as([*]const Dependency, @ptrCast(@alignCast(data.ptr + header.dependency_array_offset)))[0..header.dependency_count],
      .hash_table = @as([*]const HashBucket, @ptrCast(@alignCast(data.ptr + header.hash_table_offset)))[0..header.hash_table_size],
      .bin_entries = bin_entries,
      .disabled_dependencies = disabled_dependencies,
      .platform_entries = platform_entries,
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

    if (sectionEnd(data.len, header.string_table_offset, header.string_table_size, 1) == null or
      sectionEnd(data.len, header.package_array_offset, header.package_count, @sizeOf(Package)) == null or
      sectionEnd(data.len, header.dependency_array_offset, header.dependency_count, @sizeOf(Dependency)) == null)
    { return error.InvalidLockfile; }

    const hash_end = sectionEnd(data.len, header.hash_table_offset, header.hash_table_size, @sizeOf(HashBucket)) orelse return error.InvalidLockfile;
    if (header.bin_entry_count > 0) {
      const bin_offset: usize = @intCast(header.bin_entry_offset);
      if (bin_offset < hash_end) return error.InvalidLockfile;
      _ = sectionEnd(data.len, header.bin_entry_offset, header.bin_entry_count, @sizeOf(BinEntry)) orelse return error.InvalidLockfile;
    } else if (header.bin_entry_offset != 0) {
      const bin_offset: usize = @intCast(header.bin_entry_offset);
      if (bin_offset < hash_end or bin_offset > data.len) return error.InvalidLockfile;
    }

    const string_table = data[header.string_table_offset..][0..header.string_table_size];

    const packages_ptr: [*]const Package = @ptrCast(@alignCast(data.ptr + header.package_array_offset));
    const packages = packages_ptr[0..header.package_count];

    const deps_ptr: [*]const Dependency = @ptrCast(@alignCast(data.ptr + header.dependency_array_offset));
    const dependencies = deps_ptr[0..header.dependency_count];

    const hash_ptr: [*]const HashBucket = @ptrCast(@alignCast(data.ptr + header.hash_table_offset));
    const hash_table = hash_ptr[0..header.hash_table_size];

    const bin_entries = if (header.bin_entry_count > 0) blk: {
      const bin_ptr: [*]const BinEntry = @ptrCast(@alignCast(data.ptr + header.bin_entry_offset));
      break :blk bin_ptr[0..header.bin_entry_count];
    } else &[_]BinEntry{};
    if (header.platform_entry_count > 0 and header.platform_entry_offset != 0) {
      _ = sectionEnd(data.len, header.platform_entry_offset, header.platform_entry_count, @sizeOf(PlatformEntry)) orelse return error.InvalidLockfile;
    }
    const disabled_dependencies = try disabledDependenciesFromData(data, header);
    const platform_entries = try platformEntriesFromData(data, header);
    if (!validateSideTables(header.package_count, bin_entries, disabled_dependencies, platform_entries)) {
      return error.InvalidLockfile;
    }

    return .{
      .data = data,
      .header = header,
      .string_table = string_table,
      .packages = packages,
      .dependencies = dependencies,
      .hash_table = hash_table,
      .bin_entries = bin_entries,
      .disabled_dependencies = disabled_dependencies,
      .platform_entries = platform_entries,
    };
  }

  fn disabledSectionOffset(data_len: usize, header: *const Header) !usize {
    const bin_base = if (header.bin_entry_offset != 0)
      sectionEnd(data_len, header.bin_entry_offset, header.bin_entry_count, @sizeOf(BinEntry)) orelse return error.InvalidLockfile
    else
      sectionEnd(data_len, header.hash_table_offset, header.hash_table_size, @sizeOf(HashBucket)) orelse return error.InvalidLockfile;
    return alignSectionOffset(bin_base, @alignOf(DisabledDependency));
  }

  fn disabledDependenciesFromDataWindows(data: []align(@alignOf(Header)) u8, header: *const Header) ![]const DisabledDependency {
    if (header.disabled_dependency_count == 0) return &[_]DisabledDependency{};
    const offset = try disabledSectionOffset(data.len, header);
    const count: usize = header.disabled_dependency_count;
    const bytes = std.math.mul(usize, count, @sizeOf(DisabledDependency)) catch return error.InvalidLockfile;
    if (offset > data.len or bytes > data.len - offset) return error.InvalidLockfile;
    return @as([*]const DisabledDependency, @ptrCast(@alignCast(data.ptr + offset)))[0..count];
  }

  fn disabledDependenciesFromData(data: []align(std.heap.page_size_min) const u8, header: *const Header) ![]const DisabledDependency {
    if (header.disabled_dependency_count == 0) return &[_]DisabledDependency{};
    const offset = try disabledSectionOffset(data.len, header);
    const count: usize = header.disabled_dependency_count;
    const bytes = std.math.mul(usize, count, @sizeOf(DisabledDependency)) catch return error.InvalidLockfile;
    if (offset > data.len or bytes > data.len - offset) return error.InvalidLockfile;
    return @as([*]const DisabledDependency, @ptrCast(@alignCast(data.ptr + offset)))[0..count];
  }

  fn platformSectionOffset(data_len: usize, header: *const Header) !usize {
    const disabled_base = try disabledSectionOffset(data_len, header);
    const disabled_bytes = std.math.mul(usize, @as(usize, @intCast(header.disabled_dependency_count)), @sizeOf(DisabledDependency)) catch return error.InvalidLockfile;
    return alignSectionOffset(disabled_base + disabled_bytes, @alignOf(PlatformEntry));
  }

  fn platformEntriesFromDataWindows(data: []align(@alignOf(Header)) u8, header: *const Header) ![]const PlatformEntry {
    if (header.platform_entry_count == 0) return &[_]PlatformEntry{};
    const min_offset = try platformSectionOffset(data.len, header);
    const offset = if (header.platform_entry_offset != 0)
      @as(usize, @intCast(header.platform_entry_offset))
    else
      min_offset;
    if (offset < min_offset) return error.InvalidLockfile;
    const count: usize = header.platform_entry_count;
    const bytes = std.math.mul(usize, count, @sizeOf(PlatformEntry)) catch return error.InvalidLockfile;
    if (offset > data.len or bytes > data.len - offset) return error.InvalidLockfile;
    return @as([*]const PlatformEntry, @ptrCast(@alignCast(data.ptr + offset)))[0..count];
  }

  fn platformEntriesFromData(data: []align(std.heap.page_size_min) const u8, header: *const Header) ![]const PlatformEntry {
    if (header.platform_entry_count == 0) return &[_]PlatformEntry{};
    const min_offset = try platformSectionOffset(data.len, header);
    const offset = if (header.platform_entry_offset != 0)
      @as(usize, @intCast(header.platform_entry_offset))
    else
      min_offset;
    if (offset < min_offset) return error.InvalidLockfile;
    const count: usize = header.platform_entry_count;
    const bytes = std.math.mul(usize, count, @sizeOf(PlatformEntry)) catch return error.InvalidLockfile;
    if (offset > data.len or bytes > data.len - offset) return error.InvalidLockfile;
    return @as([*]const PlatformEntry, @ptrCast(@alignCast(data.ptr + offset)))[0..count];
  }

  fn validateSideTables(
    package_count: u32,
    bin_entries: []const BinEntry,
    disabled_dependencies: []const DisabledDependency,
    platform_entries: []const PlatformEntry,
  ) bool {
    var last_bin_package: ?u32 = null;
    for (bin_entries) |entry| {
      if (entry.package_index >= package_count) return false;
      if (last_bin_package) |last| if (entry.package_index < last) return false;
      last_bin_package = entry.package_index;
    }

    var last_disabled_parent: ?u32 = null;
    for (disabled_dependencies) |entry| {
      if (entry.parent_package_index != DisabledDependency.root_parent and
          entry.parent_package_index >= package_count) return false;
      if (last_disabled_parent) |last| if (entry.parent_package_index < last) return false;
      last_disabled_parent = entry.parent_package_index;
    }

    var last_platform_package: ?u32 = null;
    for (platform_entries) |entry| {
      if (entry.package_index >= package_count) return false;
      if (last_platform_package) |last| if (entry.package_index < last) return false;
      last_platform_package = entry.package_index;
    }

    return true;
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
    if (pkg.deps_start >= self.dependencies.len or
        pkg.deps_start + pkg.deps_count > self.dependencies.len) return &[_]Dependency{};
    return self.dependencies[pkg.deps_start..][0..pkg.deps_count];
  }

  pub fn getPackageBins(self: *const Lockfile, package_index: u32) []const BinEntry {
    var start: ?usize = null;
    var end: usize = 0;
    for (self.bin_entries, 0..) |entry, i| {
      if (entry.package_index == package_index) {
        if (start == null) start = i;
        end = i + 1;
      } else if (start != null) break;
    }
    const s = start orelse return &[_]BinEntry{};
    return self.bin_entries[s..end];
  }

  pub fn getPackageDisabledDependencies(self: *const Lockfile, package_index: u32) []const DisabledDependency {
    var start: ?usize = null;
    var end: usize = 0;
    for (self.disabled_dependencies, 0..) |entry, i| {
      if (entry.parent_package_index == package_index) {
        if (start == null) start = i;
        end = i + 1;
      } else if (start != null) break;
    }
    const s = start orelse return &[_]DisabledDependency{};
    return self.disabled_dependencies[s..end];
  }

  pub fn getRootDisabledDependencies(self: *const Lockfile) []const DisabledDependency {
    return self.getPackageDisabledDependencies(DisabledDependency.root_parent);
  }
};

pub const LockfileWriter = struct {
  allocator: std.mem.Allocator,
  packages: std.ArrayListUnmanaged(Package),
  dependencies: std.ArrayListUnmanaged(Dependency),
  bin_entries: std.ArrayListUnmanaged(BinEntry),
  disabled_dependencies: std.ArrayListUnmanaged(DisabledDependency),
  platform_entries: std.ArrayListUnmanaged(PlatformEntry),
  string_builder: std.ArrayListUnmanaged(u8),
  string_offsets: std.StringHashMap(StringRef),
  resolution_hash: u64,

  pub fn init(allocator: std.mem.Allocator) LockfileWriter {
    return .{
      .allocator = allocator,
      .packages = .empty,
      .dependencies = .empty,
      .bin_entries = .empty,
      .disabled_dependencies = .empty,
      .platform_entries = .empty,
      .string_builder = .empty,
      .string_offsets = std.StringHashMap(StringRef).init(allocator),
      .resolution_hash = 0,
    };
  }

  pub fn setResolutionHash(self: *LockfileWriter, hash: u64) void {
    self.resolution_hash = hash;
  }

  pub fn deinit(self: *LockfileWriter) void {
    self.packages.deinit(self.allocator);
    self.dependencies.deinit(self.allocator);
    self.bin_entries.deinit(self.allocator);
    self.disabled_dependencies.deinit(self.allocator);
    self.platform_entries.deinit(self.allocator);
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

    if (self.string_builder.items.len > std.math.maxInt(u32)) return error.StringTableTooLarge;
    if (str.len > std.math.maxInt(u32)) return error.StringTooLarge;

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

  pub fn addBinEntry(self: *LockfileWriter, package_index: u32, name: []const u8, path: []const u8) !void {
    try self.bin_entries.append(self.allocator, .{
      .package_index = package_index,
      .name = try self.internString(name),
      .path = try self.internString(path),
    });
  }

  pub fn addDisabledDependency(self: *LockfileWriter, parent_package_index: u32, name: []const u8, constraint: []const u8) !void {
    try self.disabled_dependencies.append(self.allocator, .{
      .parent_package_index = parent_package_index,
      .name = try self.internString(name),
      .constraint = try self.internString(constraint),
    });
  }

  pub fn addPlatformEntry(self: *LockfileWriter, package_index: u32, os: ?[]const u8, cpu: ?[]const u8, libc: ?[]const u8) !void {
    if (os == null and cpu == null and libc == null) return;
    try self.platform_entries.append(self.allocator, .{
      .package_index = package_index,
      .os = if (os) |value| try self.internString(value) else StringRef.empty,
      .cpu = if (cpu) |value| try self.internString(value) else StringRef.empty,
      .libc = if (libc) |value| try self.internString(value) else StringRef.empty,
    });
  }

  fn stringRefOrder(self: *const LockfileWriter, a: StringRef, b: StringRef) std.math.Order {
    return std.mem.order(u8, a.slice(self.string_builder.items), b.slice(self.string_builder.items));
  }

  fn binEntryLessThan(self: *const LockfileWriter, a: BinEntry, b: BinEntry) bool {
    if (a.package_index != b.package_index) return a.package_index < b.package_index;
    const name_order = self.stringRefOrder(a.name, b.name);
    if (name_order != .eq) return name_order == .lt;
    return self.stringRefOrder(a.path, b.path) == .lt;
  }

  fn disabledDependencyLessThan(self: *const LockfileWriter, a: DisabledDependency, b: DisabledDependency) bool {
    if (a.parent_package_index != b.parent_package_index) return a.parent_package_index < b.parent_package_index;
    const name_order = self.stringRefOrder(a.name, b.name);
    if (name_order != .eq) return name_order == .lt;
    return self.stringRefOrder(a.constraint, b.constraint) == .lt;
  }

  fn platformEntryLessThan(self: *const LockfileWriter, a: PlatformEntry, b: PlatformEntry) bool {
    if (a.package_index != b.package_index) return a.package_index < b.package_index;
    const os_order = self.stringRefOrder(a.os, b.os);
    if (os_order != .eq) return os_order == .lt;
    const cpu_order = self.stringRefOrder(a.cpu, b.cpu);
    if (cpu_order != .eq) return cpu_order == .lt;
    return self.stringRefOrder(a.libc, b.libc) == .lt;
  }

  fn sortSideTables(self: *LockfileWriter) void {
    std.mem.sort(BinEntry, self.bin_entries.items, self, binEntryLessThan);
    std.mem.sort(DisabledDependency, self.disabled_dependencies.items, self, disabledDependencyLessThan);
    std.mem.sort(PlatformEntry, self.platform_entries.items, self, platformEntryLessThan);
  }

  fn alignOffset(offset: u32, alignment: u32) u32 {
    return alignFileOffset(offset, alignment);
  }

  fn hashBytes(hasher: *std.hash.Wyhash, bytes: []const u8) void {
    const len: u64 = @intCast(bytes.len);
    hasher.update(std.mem.asBytes(&len));
    hasher.update(bytes);
  }

  fn hashStringRef(self: *const LockfileWriter, hasher: *std.hash.Wyhash, ref: StringRef) void {
    hashBytes(hasher, ref.slice(self.string_builder.items));
  }

  fn computeGraphHash(self: *const LockfileWriter) u64 {
    var hasher = std.hash.Wyhash.init(0);
    hasher.update("ant-lock-graph-v1");

    const package_count: u64 = @intCast(self.packages.items.len);
    hasher.update(std.mem.asBytes(&package_count));
    for (self.packages.items, 0..) |pkg, i| {
      const index: u64 = @intCast(i);
      hasher.update(std.mem.asBytes(&index));
      self.hashStringRef(&hasher, pkg.name);
      hasher.update(std.mem.asBytes(&pkg.version_major));
      hasher.update(std.mem.asBytes(&pkg.version_minor));
      hasher.update(std.mem.asBytes(&pkg.version_patch));
      self.hashStringRef(&hasher, pkg.prerelease);
      hasher.update(&pkg.integrity);
      self.hashStringRef(&hasher, pkg.tarball_url);
      self.hashStringRef(&hasher, pkg.parent_path);
      hasher.update(std.mem.asBytes(&pkg.deps_start));
      hasher.update(std.mem.asBytes(&pkg.deps_count));
      hasher.update(std.mem.asBytes(&pkg.flags));
    }

    const dep_count: u64 = @intCast(self.dependencies.items.len);
    hasher.update(std.mem.asBytes(&dep_count));
    for (self.dependencies.items) |dep| {
      hasher.update(std.mem.asBytes(&dep.package_index));
      self.hashStringRef(&hasher, dep.constraint);
      hasher.update(std.mem.asBytes(&dep.flags));
    }

    const bin_count: u64 = @intCast(self.bin_entries.items.len);
    hasher.update(std.mem.asBytes(&bin_count));
    for (self.bin_entries.items) |entry| {
      hasher.update(std.mem.asBytes(&entry.package_index));
      self.hashStringRef(&hasher, entry.name);
      self.hashStringRef(&hasher, entry.path);
    }

    const disabled_count: u64 = @intCast(self.disabled_dependencies.items.len);
    hasher.update(std.mem.asBytes(&disabled_count));
    for (self.disabled_dependencies.items) |entry| {
      hasher.update(std.mem.asBytes(&entry.parent_package_index));
      self.hashStringRef(&hasher, entry.name);
      self.hashStringRef(&hasher, entry.constraint);
    }

    const platform_count: u64 = @intCast(self.platform_entries.items.len);
    hasher.update(std.mem.asBytes(&platform_count));
    for (self.platform_entries.items) |entry| {
      hasher.update(std.mem.asBytes(&entry.package_index));
      self.hashStringRef(&hasher, entry.os);
      self.hashStringRef(&hasher, entry.cpu);
      self.hashStringRef(&hasher, entry.libc);
    }

    return hasher.final();
  }

  pub fn write(self: *LockfileWriter, path: []const u8) !u64 {
    self.sortSideTables();

    const file = try std.Io.Dir.cwd().createFile(io, path, .{});
    defer file.close(io);

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
      var probes: u32 = 0;

      while (hash_table[index].package_index != std.math.maxInt(u32) and probes < hash_table_size) : (probes += 1) {
        index = (index + 1) % hash_table_size;
      }
      if (probes >= hash_table_size) return error.HashTableFull;

      hash_table[index] = .{
        .name_hash = hash,
        .package_index = @intCast(i),
      };
    }

    const hash_table_offset = alignOffset(dependency_array_offset + dependency_array_size, @alignOf(HashBucket));
    const hash_pad_size = hash_table_offset - (dependency_array_offset + dependency_array_size);
    const hash_table_bytes: u32 = @intCast(hash_table.len * @sizeOf(HashBucket));

    const bin_entry_offset = alignOffset(hash_table_offset + hash_table_bytes, @alignOf(BinEntry));
    const bin_pad_size = bin_entry_offset - (hash_table_offset + hash_table_bytes);
    const bin_entry_bytes: u32 = @intCast(self.bin_entries.items.len * @sizeOf(BinEntry));

    const disabled_offset = alignOffset(bin_entry_offset + bin_entry_bytes, @alignOf(DisabledDependency));
    const disabled_pad_size = disabled_offset - (bin_entry_offset + bin_entry_bytes);
    const disabled_bytes: u32 = @intCast(self.disabled_dependencies.items.len * @sizeOf(DisabledDependency));

    const platform_entry_offset = alignOffset(disabled_offset + disabled_bytes, @alignOf(PlatformEntry));
    const platform_pad_size = platform_entry_offset - (disabled_offset + disabled_bytes);
    const graph_hash = self.computeGraphHash();

    const header = Header{
      .package_count = @intCast(self.packages.items.len),
      .dependency_count = @intCast(self.dependencies.items.len),
      .string_table_offset = string_table_offset,
      .string_table_size = string_table_size,
      .package_array_offset = package_array_offset,
      .dependency_array_offset = dependency_array_offset,
      .hash_table_offset = hash_table_offset,
      .hash_table_size = hash_table_size,
      .platform_os_hash = platformHash(@tagName(builtin.os.tag)),
      .platform_cpu_hash = platformHash(@tagName(builtin.cpu.arch)),
      .platform_abi_hash = platformHash(@tagName(builtin.abi)),
      .bin_entry_offset = bin_entry_offset,
      .bin_entry_count = @intCast(self.bin_entries.items.len),
      .disabled_dependency_count = @intCast(self.disabled_dependencies.items.len),
      .platform_entry_offset = platform_entry_offset,
      .platform_entry_count = @intCast(self.platform_entries.items.len),
      .graph_hash = graph_hash,
      .resolution_hash = self.resolution_hash,
    };

    try file.writeStreamingAll(io, std.mem.asBytes(&header));
    try file.writeStreamingAll(io, self.string_builder.items);
    
    if (package_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeStreamingAll(io, padding[0..package_pad_size]);
    } try file.writeStreamingAll(io, std.mem.sliceAsBytes(self.packages.items));
    
    if (dep_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeStreamingAll(io, padding[0..dep_pad_size]);
    } try file.writeStreamingAll(io, std.mem.sliceAsBytes(self.dependencies.items));
    
    if (hash_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeStreamingAll(io, padding[0..hash_pad_size]);
    } try file.writeStreamingAll(io, std.mem.sliceAsBytes(hash_table));

    if (bin_pad_size > 0) {
      const padding = [_]u8{0} ** 8;
      try file.writeStreamingAll(io, padding[0..bin_pad_size]);
    } try file.writeStreamingAll(io, std.mem.sliceAsBytes(self.bin_entries.items));

    if (self.disabled_dependencies.items.len > 0 or self.platform_entries.items.len > 0) {
      if (disabled_pad_size > 0) {
        const padding = [_]u8{0} ** 8;
        try file.writeStreamingAll(io, padding[0..disabled_pad_size]);
      }
    }

    if (self.disabled_dependencies.items.len > 0) {
      try file.writeStreamingAll(io, std.mem.sliceAsBytes(self.disabled_dependencies.items));
    }

    if (self.platform_entries.items.len > 0) {
      if (platform_pad_size > 0) {
        const padding = [_]u8{0} ** 8;
        try file.writeStreamingAll(io, padding[0..platform_pad_size]);
      } try file.writeStreamingAll(io, std.mem.sliceAsBytes(self.platform_entries.items));
    }

    return graph_hash;
  }
};
