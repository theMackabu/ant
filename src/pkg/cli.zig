const std = @import("std");
const root = @import("root.zig");

const ListCallback = 
  *const fn ([*:0]const u8, [*:0]const u8, ?*anyopaque) callconv(.c) void;

const PkgDeps = struct {
  deps: ?std.json.ObjectMap,
  dev_deps: ?std.json.ObjectMap,
  nm_path: []const u8,
  arena: std.mem.Allocator,
  parsed: std.json.Parsed(std.json.Value),
  
  pub fn deinit(self: *PkgDeps) void { 
    self.parsed.deinit(); 
  }
  
  pub fn count(self: *const PkgDeps) u32 {
    var c: u32 = 0;
    if (self.deps) |d| c += @intCast(d.count());
    if (self.dev_deps) |d| c += @intCast(d.count());
    return c;
  }
};

fn emitPkg(pd: *PkgDeps, name: []const u8, cb: ListCallback, ud: ?*anyopaque) void {
  const pkg_json = std.fmt.allocPrint(pd.arena, "{s}/{s}/package.json", .{pd.nm_path, name}) catch return;
  const content = std.fs.cwd().readFileAlloc(pd.arena, pkg_json, 256 * 1024) catch return;
  const parsed = std.json.parseFromSlice(std.json.Value, pd.arena, content, .{}) catch return;
  defer parsed.deinit();
  
  const ver = if (parsed.value.object.get("version")) |v| if (v == .string) v.string else "?" else "?";
  cb(pd.arena.dupeZ(u8, name) catch return, pd.arena.dupeZ(u8, ver) catch return, ud);
}

pub fn get_dependencies(ctx: ?*root.PkgContext, base_path: ?[]const u8, include_dev: bool) ?PkgDeps {
  const c = ctx orelse return null;
  _ = c.arena_state.reset(.retain_capacity);
  const arena = c.arena_state.allocator();
  
  const pkg_json_path = if (base_path) |bp| std.fmt.allocPrint(arena, "{s}/package.json", .{bp}) catch return null
  else arena.dupe(u8, "package.json") catch return null;
    
  const nm_path = if (base_path) |bp| std.fmt.allocPrint(arena, "{s}/node_modules", .{bp}) catch return null
  else arena.dupe(u8, "node_modules") catch return null;
  
  const content = std.fs.cwd().readFileAlloc(arena, pkg_json_path, 1024 * 1024) catch return null;
  const parsed = std.json.parseFromSlice(std.json.Value, arena, content, .{}) catch return null;
  
  const deps_val = parsed.value.object.get("dependencies");
  const deps = if (deps_val) |v| if (v == .object) v.object else null else null;
  
  const dev_deps = if (include_dev) blk: {
    const dev_val = parsed.value.object.get("devDependencies");
    break :blk if (dev_val) |v| if (v == .object) v.object else null else null;
  } else null;
  
  if (deps == null and dev_deps == null) {
    var p = parsed;
    p.deinit();
    return null;
  }
  
  return .{ .deps = deps, .dev_deps = dev_deps, .nm_path = nm_path, .arena = arena, .parsed = parsed };
}

pub fn list_dependencies(pd: *PkgDeps, cb: ListCallback, ud: ?*anyopaque) void {
  if (pd.deps) |d| for (d.keys()) |n| emitPkg(pd, n, cb, ud);
  if (pd.dev_deps) |d| for (d.keys()) |n| emitPkg(pd, n, cb, ud);
}
