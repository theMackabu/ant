const std = @import("std");
const c_allocator = std.heap.c_allocator;

const debug = @import("debug.zig");
const extractor = @import("extractor.zig");
const uv = @import("uv.zig");
const tlsuv = @import("tlsuv.zig");
const nghttp2 = @import("nghttp2.zig");

const config = @import("config");
const user_agent: [:0]const u8 = "ant/" ++ config.version;

pub const FetchError = error{
  ConnectionFailed,
  TlsError,
  Http2Error,
  Timeout,
  InvalidUrl,
  ResponseError,
  OutOfMemory,
};

pub const ParsedUrl = struct {
  scheme: []const u8,
  host: []const u8,
  port: u16,
  path: []const u8,

  pub fn parse(url: []const u8) !ParsedUrl {
    var remaining = url;
    const scheme_end = std.mem.indexOf(u8, remaining, "://") orelse return error.InvalidUrl;
    const scheme = remaining[0..scheme_end];
    remaining = remaining[scheme_end + 3 ..];

    const path_start = std.mem.indexOf(u8, remaining, "/") orelse remaining.len;
    const host_port = remaining[0..path_start];
    remaining = if (path_start < remaining.len) remaining[path_start..] else "/";

    var host: []const u8 = host_port;
    var port: u16 = if (std.mem.eql(u8, scheme, "https")) 443 else 80;

    if (std.mem.indexOf(u8, host_port, ":")) |colon| {
      host = host_port[0..colon];
      port = std.fmt.parseInt(u16, host_port[colon + 1 ..], 10) catch return error.InvalidUrl;
    }

    return .{ .scheme = scheme, .host = host, .port = port, .path = remaining };
  }
};

pub const StreamHandler = struct {
  on_data: *const fn ([]const u8, ?*anyopaque) void,
  on_complete: *const fn (u16, ?*anyopaque) void,
  on_error: *const fn (FetchError, ?*anyopaque) void,
  user_data: ?*anyopaque,
  
  pub fn init(
    on_data: *const fn ([]const u8, ?*anyopaque) void,
    on_complete: *const fn (u16, ?*anyopaque) void,
    on_error: *const fn (FetchError, ?*anyopaque) void,
    user_data: ?*anyopaque,
  ) StreamHandler {
    return .{ .on_data = on_data, .on_complete = on_complete, .on_error = on_error, .user_data = user_data };
  }
};

const PendingRequest = struct {
  url: []const u8,
  handler: ?StreamHandler,
};

const MAX_PENDING_REQUESTS = 20;
const NUM_CONNECTIONS = 6;
const NUM_META_CONNECTIONS = 3;
const META_SLOW_LOG_MS: u64 = 250;

const Http2Client = struct {
  allocator: std.mem.Allocator,
  loop: *uv.loop_t,
  tls: tlsuv.stream_t,
  h2_session: ?*nghttp2.session,
  host: [:0]const u8,
  use_tls: bool,
  connected: i32,
  connect_pending: bool,
  write_buf: std.ArrayListUnmanaged(u8),
  requests: [MAX_PENDING_REQUESTS]RequestState,
  request_count: usize,
  requests_done: usize,

  const RequestState = struct {
    stream_id: i32,
    path: ?[:0]const u8,
    on_data: ?*const fn ([]const u8, ?*anyopaque) void,
    on_complete: ?*const fn (u16, ?*anyopaque) void,
    on_error: ?*const fn (FetchError, ?*anyopaque) void,
    userdata: ?*anyopaque,
    response_body: std.ArrayListUnmanaged(u8),
    status_code: u16,
    done: bool,
    has_error: bool,
    start_ns: u64,
    end_ns: u64,
    bytes: usize,
    content_encoding: ContentEncoding,
  };

  const ContentEncoding = enum {
    identity,
    gzip,
  };

  const alpn_protocols = [_][*:0]const u8{ "h2", "http/1.1" };

  pub fn init(allocator: std.mem.Allocator, host: []const u8, use_tls: bool) !*Http2Client {
    const client = try allocator.create(Http2Client);
    errdefer allocator.destroy(client);

    const host_z = try allocator.dupeZ(u8, host);
    errdefer allocator.free(host_z);

    client.* = .{
      .allocator = allocator,
      .loop = uv.uv_default_loop(),
      .tls = .{},
      .h2_session = null,
      .host = host_z,
      .use_tls = use_tls,
      .connected = 0,
      .connect_pending = false,
      .write_buf = .{},
      .requests = undefined,
      .request_count = 0,
      .requests_done = 0,
    };

    for (&client.requests) |*req| {
      req.* = .{
        .stream_id = 0,
        .path = null,
        .on_data = null,
        .on_complete = null,
        .on_error = null,
        .userdata = null,
        .response_body = .{},
        .status_code = 0,
        .done = false,
        .has_error = false,
        .start_ns = 0,
        .end_ns = 0,
        .bytes = 0,
        .content_encoding = .identity,
      };
    }

    if (tlsuv.tlsuv_stream_init(client.loop, &client.tls, null) != 0) {
      allocator.free(host_z);
      allocator.destroy(client);
      return error.ConnectionFailed;
    }

    _ = tlsuv.tlsuv_stream_set_hostname(&client.tls, host_z.ptr);
    _ = tlsuv.tlsuv_stream_set_protocols(&client.tls, 2, &alpn_protocols);

    return client;
  }

  pub fn deinit(self: *Http2Client) void {
    for (&self.requests) |*req| {
      req.on_data = null;
      req.on_complete = null;
      req.on_error = null;
      req.userdata = null;
    }

    if (self.connected > 0) {
      self.tls.data = self;
      _ = tlsuv.tlsuv_stream_close(&self.tls, onStreamClose);
      while (self.connected > 0) _ = uv.uv_run(self.loop, uv.RUN_ONCE);
    }

    if (self.h2_session) |session| nghttp2.nghttp2_session_del(session);

    for (&self.requests) |*req| {
      if (req.stream_id != -1) {
        if (req.path) |p| self.allocator.free(p);
        req.response_body.deinit(self.allocator);
      }
    }
    
    self.write_buf.deinit(self.allocator);
    self.allocator.free(self.host);
    self.allocator.destroy(self);
  }

  pub fn resetRequests(self: *Http2Client) void {
    for (self.requests[0..self.request_count]) |*req| {
      if (req.stream_id != -1) {
        if (req.path) |p| self.allocator.free(p);
        req.response_body.deinit(self.allocator);
      }
      req.* = .{
        .stream_id = 0,
        .path = null,
        .on_data = null,
        .on_complete = null,
        .on_error = null,
        .userdata = null,
        .response_body = .{},
        .status_code = 0,
        .done = false,
        .has_error = false,
        .start_ns = 0,
        .end_ns = 0,
        .bytes = 0,
        .content_encoding = .identity,
      };
    }
    self.request_count = 0;
    self.requests_done = 0;
  }

  pub fn hasCapacity(self: *const Http2Client) bool {
    for (self.requests[0..self.request_count]) |req| {
      if (req.stream_id == -1) return true;
    }
    return self.request_count < MAX_PENDING_REQUESTS - 1;
  }

  pub fn recycleCompletedRequests(self: *Http2Client) void {
    if (self.requests_done == 0) return;

    for (self.requests[0..self.request_count]) |*req| {
      if (req.done and req.stream_id != -1) {
        if (req.path) |p| self.allocator.free(p);
        req.response_body.deinit(self.allocator);
        req.path = null;
        req.response_body = .{};
        req.stream_id = -1;
      }
    }
  }

  fn findOrAllocSlot(self: *Http2Client) ?*RequestState {
    for (self.requests[0..self.request_count]) |*req| {
      if (req.stream_id == -1) return req;
    }
    if (self.request_count < MAX_PENDING_REQUESTS) {
      const req = &self.requests[self.request_count];
      self.request_count += 1;
      return req;
    }
    return null;
  }

  fn onStreamClose(handle: *uv.handle_t) callconv(.c) void {
    const tls: *tlsuv.stream_t = @ptrCast(@alignCast(handle));
    const client: *Http2Client = @ptrCast(@alignCast(tls.data));
    client.connected = -2;
  }

  fn findRequest(self: *Http2Client, stream_id: i32) ?*RequestState {
    for (self.requests[0..self.request_count]) |*req| if (req.stream_id == stream_id) return req;
    return null;
  }

  fn h2Send(_: ?*nghttp2.session, data: [*c]const u8, len: usize, _: c_int, ud: ?*anyopaque) callconv(.c) isize {
    const client: *Http2Client = @ptrCast(@alignCast(ud));
    client.write_buf.appendSlice(client.allocator, data[0..len]) catch return nghttp2.ERR_NOMEM;
    return @intCast(len);
  }

  fn h2FrameRecv(_: ?*nghttp2.session, frame: *const nghttp2.frame, ud: ?*anyopaque) callconv(.c) c_int {
    const client: *Http2Client = @ptrCast(@alignCast(ud));
    if (frame.hd.flags & nghttp2.FLAG_END_STREAM != 0) {
      if (client.findRequest(frame.hd.stream_id)) |req| {
        if (!req.done) {
          req.done = true;
          req.end_ns = @intCast(std.time.nanoTimestamp());
          client.requests_done += 1;
          if (req.on_complete) |cb| cb(req.status_code, req.userdata);
        }
      }
    }
    return 0;
  }

  fn h2DataChunk(session: ?*nghttp2.session, _: u8, stream_id: i32, data: [*c]const u8, len: usize, ud: ?*anyopaque) callconv(.c) c_int {
    const client: *Http2Client = @ptrCast(@alignCast(ud));
    const req = client.findRequest(stream_id) orelse return 0;
    if (req.on_data) |cb| cb(data[0..len], req.userdata) else req.response_body.appendSlice(client.allocator, data[0..len]) catch {
      req.has_error = true;
    }; req.bytes += len;
    if (session) |s| _ = nghttp2.nghttp2_session_consume(s, stream_id, len);

    return 0;
  }

  fn h2Header(_: ?*nghttp2.session, frame: *const nghttp2.frame, name: [*c]const u8, namelen: usize, value: [*c]const u8, valuelen: usize, _: u8, ud: ?*anyopaque) callconv(.c) c_int {
    const client: *Http2Client = @ptrCast(@alignCast(ud));
    if (frame.hd.type != nghttp2.HEADERS) return 0;
    const req = client.findRequest(frame.hd.stream_id) orelse return 0;
    if (namelen == 7 and std.mem.eql(u8, name[0..7], ":status"))
      req.status_code = std.fmt.parseInt(u16, value[0..valuelen], 10) catch 0;
    if (std.mem.eql(u8, name[0..namelen], "content-encoding")) {
      if (std.mem.startsWith(u8, value[0..valuelen], "gzip")) {
        req.content_encoding = .gzip;
      }
    }
    return 0;
  }

  fn h2StreamClose(_: ?*nghttp2.session, stream_id: i32, error_code: u32, ud: ?*anyopaque) callconv(.c) c_int {
    const client: *Http2Client = @ptrCast(@alignCast(ud));
    const req = client.findRequest(stream_id) orelse return 0;
    if (!req.done) {
      req.done = true;
      req.end_ns = @intCast(std.time.nanoTimestamp());
      client.requests_done += 1;
      if (error_code != 0) {
        req.has_error = true;
        if (req.on_error) |cb| cb(FetchError.Http2Error, req.userdata);
      } else if (req.on_complete) |cb| cb(req.status_code, req.userdata);
    }
    return 0;
  }

  fn initH2(self: *Http2Client) !void {
    var callbacks: *nghttp2.session_callbacks = undefined;
    if (nghttp2.nghttp2_session_callbacks_new(&callbacks) != 0) return error.Http2Error;
    defer nghttp2.nghttp2_session_callbacks_del(callbacks);

   nghttp2.nghttp2_session_callbacks_set_send_callback2(callbacks, h2Send);
   nghttp2.nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2FrameRecv);
   nghttp2.nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, h2DataChunk);
   nghttp2.nghttp2_session_callbacks_set_on_header_callback(callbacks, h2Header);
   nghttp2.nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2StreamClose);

    var session: *nghttp2.session = undefined;
    if (nghttp2.nghttp2_session_client_new(&session, callbacks, self) != 0) return error.Http2Error;
    self.h2_session = session;

    var settings = [_]nghttp2.settings_entry{
      .{ .settings_id = nghttp2.SETTINGS_MAX_CONCURRENT_STREAMS, .value = MAX_PENDING_REQUESTS },
      .{ .settings_id = nghttp2.SETTINGS_INITIAL_WINDOW_SIZE, .value = 16 * 1024 * 1024 },
    };
    if (nghttp2.nghttp2_submit_settings(self.h2_session.?, nghttp2.FLAG_NONE, &settings, settings.len) != 0) return error.Http2Error;

    const conn_window_increase: i32 = (16 * 1024 * 1024) - 65535;
    _ = nghttp2.nghttp2_submit_window_update(self.h2_session.?, nghttp2.FLAG_NONE, 0, conn_window_increase);
  }

  fn flush(self: *Http2Client) !void {
    if (self.h2_session) |session| while (nghttp2.nghttp2_session_want_write(session) != 0) if (nghttp2.nghttp2_session_send(session) != 0) break;
    if (self.write_buf.items.len > 0) {
      const data = try self.allocator.dupe(u8, self.write_buf.items);
      self.write_buf.clearRetainingCapacity();
      const wr = try self.allocator.create(uv.write_t);
      wr.data = data.ptr;
      var buf = uv.buf_t{ .base = data.ptr, .len = data.len };
      if (tlsuv.tlsuv_stream_write(wr, &self.tls, &buf, onWrite) != 0) {
        self.allocator.free(data);
        self.allocator.destroy(wr);
        return error.ConnectionFailed;
      }
    }
  }

  fn onWrite(wr: *uv.write_t, _: c_int) callconv(.c) void {
    const data_ptr: [*]u8 = @ptrCast(wr.data);
    std.c.free(data_ptr);
    std.c.free(@ptrCast(wr));
  }

  fn allocBuf(_: *uv.handle_t, size: usize, buf: *uv.buf_t) callconv(.c) void {
    buf.base = @ptrCast(std.c.malloc(size) orelse return);
    buf.len = size;
  }

  fn onRead(stream: *uv.stream_t, nread: isize, buf: *const uv.buf_t) callconv(.c) void {
    const tls: *tlsuv.stream_t = @ptrCast(@alignCast(stream));
    const client: *Http2Client = @ptrCast(@alignCast(tls.data));
    defer if (buf.base) |b| std.c.free(b);
    if (nread < 0) {
      for (client.requests[0..client.request_count]) |*req| if (!req.done) {
        req.done = true;
        req.has_error = true;
        client.requests_done += 1;
        if (req.on_error) |cb| cb(FetchError.ConnectionFailed, req.userdata);
      };
      return;
    }
    if (nread > 0 and client.h2_session != null) {
      _ = nghttp2.nghttp2_session_mem_recv(client.h2_session.?, @ptrCast(buf.base), @intCast(nread));
      client.flush() catch {};
    }
  }

  fn onConnect(req: *uv.connect_t, status: c_int) callconv(.c) void {
    const ctx: *ConnectCtx = @ptrCast(@alignCast(req.data));
    defer ctx.client.allocator.destroy(ctx);
    if (status < 0) {
      ctx.client.connected = -1;
      return;
    }
    ctx.client.connected = 1;
    ctx.client.tls.data = ctx.client;
    ctx.client.initH2() catch {
      ctx.client.connected = -1;
      return;
    };
    _ = tlsuv.tlsuv_stream_read_start(&ctx.client.tls, allocBuf, onRead);
    ctx.client.flush() catch {};
  }

  const ConnectCtx = struct { client: *Http2Client, req: uv.connect_t };

  fn ensureConnected(self: *Http2Client) !void {
    if (self.connected > 0) return;
    if (self.connected < 0) return error.ConnectionFailed;

    var conn_start: u64 = @intCast(std.time.nanoTimestamp());

    if (!self.connect_pending) {
      const ctx = try self.allocator.create(ConnectCtx);
      ctx.* = .{ .client = self, .req = .{} };
      ctx.req.data = ctx;
      if (tlsuv.tlsuv_stream_connect(&ctx.req, &self.tls, self.host.ptr, if (self.use_tls) 443 else 80, onConnect) != 0) {
        self.allocator.destroy(ctx);
        return error.ConnectionFailed;
      }
      self.connect_pending = true;
    }

    var loop_count: u32 = 0;
    while (self.connected == 0) {
      _ = uv.uv_run(self.loop, uv.RUN_ONCE);
      loop_count += 1;
    }
    conn_start = debug.timer("    h2: tls connect", conn_start);
    debug.log("    h2: connect loop iterations={d}", .{loop_count});
    if (self.connected < 0) return error.ConnectionFailed;
  }

  pub fn initiateConnectAsync(self: *Http2Client) !void {
    if (self.connected > 0) return;
    if (self.connected < 0) return error.ConnectionFailed;
    if (self.connect_pending) return;

    const ctx = try self.allocator.create(ConnectCtx);
    ctx.* = .{ .client = self, .req = .{} };
    ctx.req.data = ctx;
    if (tlsuv.tlsuv_stream_connect(&ctx.req, &self.tls, self.host.ptr, if (self.use_tls) 443 else 80, onConnect) != 0) {
      self.allocator.destroy(ctx);
      return error.ConnectionFailed;
    }
    self.connect_pending = true;
  }

  fn makeNv(name: [:0]const u8, value: [:0]const u8) nghttp2.nv {
    return .{ 
      .name = @constCast(name.ptr),
      .value = @constCast(value.ptr),
      .namelen = name.len, .valuelen = value.len, .flags = nghttp2.NV_FLAG_NONE 
    };
  }

  pub fn get(self: *Http2Client, path: []const u8, allocator: std.mem.Allocator) ![]u8 {
    return self.getWithAccept(path, "application/json", allocator);
  }

  pub fn getWithAccept(self: *Http2Client, path: []const u8, accept: [:0]const u8, allocator: std.mem.Allocator) ![]u8 {
    try self.ensureConnected();
    if (self.request_count >= MAX_PENDING_REQUESTS) self.resetRequests();
    const req = &self.requests[self.request_count]; self.request_count += 1;
    
    req.* = .{
      .stream_id = 0,
      .path = try self.allocator.dupeZ(u8, path),
      .on_data = null,
      .on_complete = null,
      .on_error = null,
      .userdata = null,
      .response_body = .{},
      .status_code = 0,
      .done = false,
      .has_error = false,
      .start_ns = @intCast(std.time.nanoTimestamp()),
      .end_ns = 0,
      .bytes = 0,
      .content_encoding = .identity,
    };
    
    const session = self.h2_session orelse return error.Http2Error;
    
    var hdrs = [_]nghttp2.nv{ 
      makeNv(":method", "GET"),
      makeNv(":path", req.path.?),
      makeNv(":scheme", "https"),
      makeNv(":authority", self.host),
      makeNv("accept", accept),
      makeNv("user-agent", user_agent) 
    };
    
    const sid = nghttp2.nghttp2_submit_request(session, null, &hdrs, hdrs.len, null, req);
    if (sid < 0) {
      self.request_count -= 1;
      if (req.path) |p| self.allocator.free(p);
      return error.Http2Error;
    }
    
    req.stream_id = sid;
    try self.flush();
    while (!req.done) {
      _ = uv.uv_run(self.loop, uv.RUN_ONCE);
      try self.flush();
    }
    
    if (req.has_error or req.status_code != 200) return error.ResponseError;
    return try allocator.dupe(u8, req.response_body.items);
  }

  pub fn getStream(self: *Http2Client, path: []const u8, on_data: *const fn ([]const u8, ?*anyopaque) void, on_complete: *const fn (u16, ?*anyopaque) void, on_error: *const fn (FetchError, ?*anyopaque) void, userdata: ?*anyopaque) !void {
    try self.ensureConnected();
    const req = self.findOrAllocSlot() orelse return error.OutOfMemory;
    
    req.* = .{ 
      .stream_id = 0,
      .path = try self.allocator.dupeZ(u8, path),
      .on_data = on_data,
      .on_complete = on_complete,
      .on_error = on_error,
      .userdata = userdata,
      .response_body = .{},
      .status_code = 0,
      .done = false,
      .has_error = false,
      .start_ns = @intCast(std.time.nanoTimestamp()),
      .end_ns = 0,
      .bytes = 0,
      .content_encoding = .identity 
    };

    const session = self.h2_session orelse return error.Http2Error;
    
    var hdrs = [_]nghttp2.nv{ 
      makeNv(":method", "GET"),
      makeNv(":path", req.path.?),
      makeNv(":scheme", "https"),
      makeNv(":authority", self.host),
      makeNv("accept", "*/*"),
      makeNv("user-agent", user_agent)
    };
    
    const sid = nghttp2.nghttp2_submit_request(session, null, &hdrs, hdrs.len, null, req);
    if (sid < 0) {
      if (req.path) |p| self.allocator.free(p);
      req.stream_id = -1;
      return error.Http2Error;
    }
    
    req.stream_id = sid;
    try self.flush();
  }

  pub fn run(self: *Http2Client) !void {
    const run_start: u64 = @intCast(std.time.nanoTimestamp());
    var loop_count: u32 = 0;
    var last_done: usize = 0;
    var last_report: u64 = run_start;

    while (self.requests_done < self.request_count) {
      if (uv.uv_run(self.loop, uv.RUN_ONCE) == 0) break;
      try self.flush();
      loop_count += 1;

      const now: u64 = @intCast(std.time.nanoTimestamp());
      if (now - last_report > 1_000_000_000) {
        const done_delta = self.requests_done - last_done;
        debug.log("    h2: progress {d}/{d} (+{d} in last 1s) loops={d}", .{
          self.requests_done, self.request_count,
          done_delta, loop_count,
        });
        last_done = self.requests_done;
        last_report = now;
      }
    }

    const elapsed_ns: u64 = @intCast(@as(i128, @intCast(std.time.nanoTimestamp())) - @as(i128, run_start));
    const elapsed_ms = elapsed_ns / 1_000_000;
    debug.log("    h2: run complete in {d}ms, {d} loops, {d}/{d} done", .{
      elapsed_ms, loop_count,
      self.requests_done, self.request_count,
    });

    var error_count: usize = 0;
    for (self.requests[0..self.request_count]) |req| {
      if (req.has_error) error_count += 1;
    }
    if (error_count > 0) {
      debug.log("    h2: {d} requests had errors", .{error_count});
      return error.ResponseError;
    }
  }
};

pub const TarballCtx = struct {
  handler: StreamHandler,
  done: bool,
  has_error: bool,
  url: []const u8,
  start_ns: u64,
  bytes: usize,
};

const TarballStats = struct {
  url: []const u8,
  bytes: usize,
  elapsed_ms: u64,
};

const TarballCallbacks = struct {
  fn onData(data: []const u8, ud: ?*anyopaque) void {
    const ctx: *TarballCtx = @ptrCast(@alignCast(ud));
    ctx.bytes += data.len;
    ctx.handler.on_data(data, ctx.handler.user_data);
  }
  
  fn onComplete(status: u16, ud: ?*anyopaque) void {
    const ctx: *TarballCtx = @ptrCast(@alignCast(ud));
    ctx.handler.on_complete(status, ctx.handler.user_data);
    ctx.done = true;
  }
  
  fn onError(err: FetchError, ud: ?*anyopaque) void {
    const ctx: *TarballCtx = @ptrCast(@alignCast(ud));
    ctx.handler.on_error(err, ctx.handler.user_data);
    ctx.done = true;
    ctx.has_error = true;
  }
};

pub const Fetcher = struct {
  allocator: std.mem.Allocator,
  registry_host: []const u8,
  meta_clients: [NUM_META_CONNECTIONS]?*Http2Client,
  meta_clients_initialized: bool,
  pending: std.ArrayListUnmanaged(PendingRequest),
  tarball_clients: [NUM_CONNECTIONS]?*Http2Client,
  tarball_clients_initialized: bool,
  tarball_contexts: std.ArrayListUnmanaged(*TarballCtx),
  tarball_round_robin: usize,
  tarball_stats: std.ArrayListUnmanaged(TarballStats),

  pub fn init(allocator: std.mem.Allocator, registry_host: []const u8) !*Fetcher {
    const f = try allocator.create(Fetcher);
    f.* = .{
      .allocator = allocator,
      .registry_host = try allocator.dupe(u8, registry_host),
      .meta_clients = [_]?*Http2Client{null} ** NUM_META_CONNECTIONS,
      .meta_clients_initialized = false,
      .pending = .{},
      .tarball_clients = [_]?*Http2Client{null} ** NUM_CONNECTIONS,
      .tarball_clients_initialized = false,
      .tarball_contexts = .{},
      .tarball_round_robin = 0,
      .tarball_stats = .{},
    };
    return f;
  }

  pub fn deinit(self: *Fetcher) void {
    for (&self.meta_clients) |*maybe_client| {
      if (maybe_client.*) |c| { c.deinit();  maybe_client.* = null; }
    }
    for (self.pending.items) |req| self.allocator.free(req.url);
    self.pending.deinit(self.allocator);
    for (&self.tarball_clients) |*maybe_client| {
      if (maybe_client.*) |c| { c.deinit(); maybe_client.* = null; }
    }
    for (self.tarball_contexts.items) |ctx| {
      self.allocator.free(ctx.url);
      self.allocator.destroy(ctx);
    }
    self.tarball_contexts.deinit(self.allocator);
    for (self.tarball_stats.items) |stat| self.allocator.free(stat.url);
    self.tarball_stats.deinit(self.allocator);
    self.allocator.free(self.registry_host);
    self.allocator.destroy(self);
  }

  fn ensureMetaClients(self: *Fetcher) !void {
    if (self.meta_clients_initialized) return;

    for (&self.meta_clients, 0..) |*slot, i| {
      const client = Http2Client.init(self.allocator, self.registry_host, true) catch |err| {
        debug.log("fetcher: failed to init meta connection {d}: {}", .{ i, err });
        continue;
      };
      client.ensureConnected() catch |err| {
        debug.log("fetcher: failed to connect meta {d}: {}", .{ i, err });
        client.deinit();
        continue;
      };
      slot.* = client;
    }

    var any_connected = false;
    for (self.meta_clients) |slot| {
      if (slot != null) { any_connected = true; break; }
    }
    
    if (!any_connected) return error.ConnectionFailed;
    self.meta_clients_initialized = true;
  }

  pub fn resetMetaClients(self: *Fetcher) void {
    for (&self.meta_clients) |*slot| {
      if (slot.*) |client| { client.deinit(); slot.* = null; }
    }
    self.meta_clients_initialized = false;
  }

  fn ensureTarballClients(self: *Fetcher) !void {
    if (self.tarball_clients_initialized) return;

    debug.log("fetcher: initializing {d} persistent connections", .{NUM_CONNECTIONS});
    const init_start: u64 = @intCast(std.time.nanoTimestamp());

    for (&self.tarball_clients, 0..) |*slot, i| {
      const client = Http2Client.init(self.allocator, self.registry_host, true) catch |err| {
        debug.log("fetcher: failed to init connection {d}: {}", .{ i, err });
        continue;
      };
      client.ensureConnected() catch |err| {
        debug.log("fetcher: failed to connect {d}: {}", .{ i, err });
        client.deinit();
        continue;
      };
      slot.* = client;
    }

    self.tarball_clients_initialized = true;
    _ = debug.timer("fetcher: connection pool init", init_start);
  }
  
  fn findAvailableClient(self: *Fetcher) ?struct { client: *Http2Client, idx: usize } {
    var attempts: usize = 0;
    while (attempts < NUM_CONNECTIONS) : (attempts += 1) {
      const idx = (self.tarball_round_robin + attempts) % NUM_CONNECTIONS;
      if (self.tarball_clients[idx]) |client| { if (client.hasCapacity()) return .{ .client = client, .idx = idx }; }
    }
    return null;
  }

  pub fn initiateTarballConnectionsAsync(self: *Fetcher) void {
    if (self.tarball_clients_initialized) return;
    debug.log("fetcher: initiating {d} tarball connections (async)", .{NUM_CONNECTIONS});

    for (&self.tarball_clients, 0..) |*slot, i| {
      const client = Http2Client.init(self.allocator, self.registry_host, true) catch {
        continue;
      };
      client.initiateConnectAsync() catch {
        client.deinit(); continue;
      };
      slot.* = client; _ = i;
    }
    self.tarball_clients_initialized = true;
  }

  pub fn queueTarballAsync(self: *Fetcher, url: []const u8, handler: StreamHandler) !void {
    try self.ensureTarballClients();
    const parsed = try ParsedUrl.parse(url);
    
    const available = self.findAvailableClient() orelse {
      try self.pending.append(self.allocator, .{
        .url = try self.allocator.dupe(u8, url),
        .handler = handler,
      }); return;
    };
    
    const ctx = try self.allocator.create(TarballCtx);
    ctx.* = .{
      .handler = handler,
      .done = false,
      .has_error = false,
      .url = try self.allocator.dupe(u8, url),
      .start_ns = @intCast(std.time.nanoTimestamp()),
      .bytes = 0,
    };
    
    try self.tarball_contexts.append(
      self.allocator,
      ctx
    );

    try available.client.getStream(
      parsed.path,
      TarballCallbacks.onData,
      TarballCallbacks.onComplete,
      TarballCallbacks.onError,
      ctx,
    );
    
    self.tarball_round_robin = (available.idx + 1) % NUM_CONNECTIONS;
  }

  pub fn tick(self: *Fetcher) usize {
    self.ensureTarballClients() catch return 0;
    const loop = uv.uv_default_loop();
    
    for (&self.tarball_clients) |maybe_client| {
      if (maybe_client) |c| c.flush() catch {};
    }
    _ = uv.uv_run(loop, uv.RUN_NOWAIT);
    
    for (&self.tarball_clients) |maybe_client| {
      if (maybe_client) |c| c.recycleCompletedRequests();
    }
    
    const completed = self.cleanupCompletedContexts();
    self.dispatchPending();
    
    return completed;
  }
  
  fn cleanupCompletedContexts(self: *Fetcher) usize {
    var completed: usize = 0; var i: usize = 0;
    while (i < self.tarball_contexts.items.len) {
      const ctx = self.tarball_contexts.items[i];
      if (ctx.done) {
        completed += 1;
        self.allocator.free(ctx.url);
        self.allocator.destroy(ctx);
        _ = self.tarball_contexts.swapRemove(i);
      } else i += 1;
    }
    return completed;
  }
  
  fn dispatchPending(self: *Fetcher) void {
    while (self.pending.items.len > 0) {
      const available = self.findAvailableClient() orelse break;
      const req = self.pending.pop() orelse break;
      
      const handler = req.handler orelse {
        self.allocator.free(req.url); continue;
      };
      
      self.dispatchRequest(available.client, req.url, handler) catch |err| {
        handler.on_error(errToFetchError(err), handler.user_data);
        self.allocator.free(req.url); continue;
      };
    }
  }
  
  fn dispatchRequest(self: *Fetcher, client: *Http2Client, url: []const u8, handler: StreamHandler) !void {
    const parsed = try ParsedUrl.parse(url);
    const ctx = try self.allocator.create(TarballCtx);
    
    ctx.* = .{
      .handler = handler,
      .done = false,
      .has_error = false,
      .url = url,
      .start_ns = @intCast(std.time.nanoTimestamp()),
      .bytes = 0,
    };
    
    errdefer self.allocator.destroy(ctx);
    try self.tarball_contexts.append(self.allocator, ctx);
    
    try client.getStream(
      parsed.path,
      TarballCallbacks.onData,
      TarballCallbacks.onComplete,
      TarballCallbacks.onError,
      ctx,
    );
  }
  
  fn errToFetchError(err: anyerror) FetchError {
    return switch (err) {
      error.InvalidUrl => FetchError.InvalidUrl,
      error.OutOfMemory => FetchError.OutOfMemory,
      else => FetchError.Http2Error,
    };
  }
  
  pub fn pendingTarballCount(self: *Fetcher) usize {
    return self.tarball_contexts.items.len;
  }

  pub fn finishTarballs(self: *Fetcher) void {
    const loop = uv.uv_default_loop();
    var last_report: u64 = @intCast(std.time.nanoTimestamp());
    var loops: usize = 0;
    var completed: usize = 0;
    const start = last_report;

    while (self.tarball_contexts.items.len > 0 or self.pending.items.len > 0) {
      for (&self.tarball_clients) |maybe_client| {
        if (maybe_client) |c| c.flush() catch {};
      }

      if (uv.uv_run(loop, uv.RUN_ONCE) == 0 and self.pending.items.len == 0 and self.tarball_contexts.items.len == 0) break;
      loops += 1;

      for (&self.tarball_clients) |maybe_client| {
        if (maybe_client) |c| c.recycleCompletedRequests();
      }

      var i: usize = 0;
      while (i < self.tarball_contexts.items.len) {
        const ctx = self.tarball_contexts.items[i];
        if (ctx.done) {
          if (!ctx.has_error) {
            const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.time.nanoTimestamp())) - ctx.start_ns) / 1_000_000);
            const url_copy = self.allocator.dupe(u8, ctx.url) catch null;
            if (url_copy) |url| {
              self.tarball_stats.append(self.allocator, .{ .url = url, .bytes = ctx.bytes, .elapsed_ms = elapsed_ms }) catch {};
            }
          }
          self.allocator.free(ctx.url);
          self.allocator.destroy(ctx);
          _ = self.tarball_contexts.swapRemove(i);
          completed += 1;
        } else {
          i += 1;
        }
      }

      while (self.pending.items.len > 0) {
        var queued = false;
        for (&self.tarball_clients, 0..) |maybe_client, conn_idx| {
          if (maybe_client) |client| {
            if (client.hasCapacity()) {
              const maybe_req = self.pending.pop();
              const req = maybe_req orelse break;
              if (req.handler) |handler| {
                const parsed = ParsedUrl.parse(req.url) catch {
                  handler.on_error(FetchError.InvalidUrl, handler.user_data);
                  self.allocator.free(req.url);
                  continue;
                };

                const ctx = self.allocator.create(TarballCtx) catch {
                  handler.on_error(FetchError.OutOfMemory, handler.user_data);
                  self.allocator.free(req.url);
                  continue;
                };
                ctx.* = .{
                  .handler = handler,
                  .done = false,
                  .has_error = false,
                  .url = req.url,
                  .start_ns = @intCast(std.time.nanoTimestamp()),
                  .bytes = 0,
                };
                self.tarball_contexts.append(self.allocator, ctx) catch {
                  self.allocator.destroy(ctx);
                  self.allocator.free(req.url);
                  continue;
                };

                client.getStream(
                  parsed.path,
                  struct {
                    fn onData(data: []const u8, ud: ?*anyopaque) void {
                      const c: *TarballCtx = @ptrCast(@alignCast(ud));
                      c.bytes += data.len;
                      c.handler.on_data(data, c.handler.user_data);
                    }
                  }.onData,
                  struct {
                    fn onComplete(status: u16, ud: ?*anyopaque) void {
                      const c: *TarballCtx = @ptrCast(@alignCast(ud));
                      c.handler.on_complete(status, c.handler.user_data);
                      if (debug.enabled) {
                        const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.time.nanoTimestamp())) - c.start_ns) / 1_000_000);
                        debug.log("    tarball: done {s} {d}ms {d} bytes status={d}", .{ c.url, elapsed_ms, c.bytes, status });
                      }
                      c.done = true;
                    }
                  }.onComplete,
                  struct {
                    fn onError(err: FetchError, ud: ?*anyopaque) void {
                      const c: *TarballCtx = @ptrCast(@alignCast(ud));
                      c.handler.on_error(err, c.handler.user_data);
                      if (debug.enabled) {
                        const elapsed_ms: u64 = @intCast((@as(u64, @intCast(std.time.nanoTimestamp())) - c.start_ns) / 1_000_000);
                        debug.log("    tarball: error {s} {d}ms {d} bytes", .{ c.url, elapsed_ms, c.bytes });
                      }
                      c.done = true;
                      c.has_error = true;
                    }
                  }.onError,
                  ctx,
                ) catch {
                  handler.on_error(FetchError.Http2Error, handler.user_data);
                  ctx.done = true;
                };
                queued = true;
                _ = conn_idx;
              } else {
                self.allocator.free(req.url);
              }
              break;
            }
          }
        }
        if (!queued) break;
      }

      const now: u64 = @intCast(std.time.nanoTimestamp());
      if (now - last_report > 1_000_000_000) {
        var total_bytes: usize = 0;
        for (self.tarball_contexts.items) |ctx| {
          total_bytes += ctx.bytes;
        }
        debug.log("    h2: {d} in-flight, {d} pending, {d} completed, {d} loops", .{
          self.tarball_contexts.items.len,
          self.pending.items.len,
          completed,
          loops,
        });
        debug.log("    h2: tarball progress in-flight bytes={d}", .{ total_bytes });
        last_report = now;
      }
    }

    const elapsed_ns: u64 = @intCast(@as(i128, @intCast(std.time.nanoTimestamp())) - @as(i128, start));
    debug.log("fetcher: finishTarballs completed in {d}ms, {d} loops, {d} completed", .{
      elapsed_ns / 1_000_000,
      loops,
      completed,
    });
    if (debug.enabled and self.tarball_stats.items.len > 0) {
      var top_time: [5]?TarballStats = .{null} ** 5;
      var top_size: [5]?TarballStats = .{null} ** 5;

      for (self.tarball_stats.items) |stat| {
        var idx_time: usize = top_time.len;
        for (top_time, 0..) |slot, i| {
          if (slot == null or stat.elapsed_ms > slot.?.elapsed_ms) {
            idx_time = i;
            break;
          }
        }
        if (idx_time < top_time.len) {
          var carry = stat;
          var j = idx_time;
          while (j < top_time.len) : (j += 1) {
            const next = top_time[j];
            top_time[j] = carry;
            if (next) |n| {
              carry = n;
            } else {
              break;
            }
          }
        }

        var idx_size: usize = top_size.len;
        for (top_size, 0..) |slot, i| {
          if (slot == null or stat.bytes > slot.?.bytes) {
            idx_size = i;
            break;
          }
        }
        if (idx_size < top_size.len) {
          var carry_size = stat;
          var k = idx_size;
          while (k < top_size.len) : (k += 1) {
            const next_size = top_size[k];
            top_size[k] = carry_size;
            if (next_size) |n| {
              carry_size = n;
            } else {
              break;
            }
          }
        }
      }

      debug.log("fetcher: top tarballs by time", .{});
      for (top_time, 0..) |maybe_stat, i| {
        if (maybe_stat) |stat| {
          debug.log("  {d}. {s} {d}ms {d} bytes", .{ i + 1, stat.url, stat.elapsed_ms, stat.bytes });
        }
      }
      debug.log("fetcher: top tarballs by size", .{});
      for (top_size, 0..) |maybe_stat, i| {
        if (maybe_stat) |stat| {
          debug.log("  {d}. {s} {d} bytes {d}ms", .{ i + 1, stat.url, stat.bytes, stat.elapsed_ms });
        }
      }
    }
  }

  pub fn fetchMetadata(self: *Fetcher, package_name: []const u8, allocator: std.mem.Allocator) ![]u8 {
    return self.fetchMetadataFull(package_name, false, allocator);
  }

  pub fn fetchMetadataFull(self: *Fetcher, package_name: []const u8, full: bool, allocator: std.mem.Allocator) ![]u8 {
    try self.ensureMetaClients();
    for (self.meta_clients) |maybe_client| {
      if (maybe_client) |client| {
        var path_buf: [512]u8 = undefined;
        const path_slice = std.fmt.bufPrint(&path_buf, "/{s}", .{package_name}) catch return error.OutOfMemory;
        const accept: [:0]const u8 = if (full) "application/json" else "application/vnd.npm.install-v1+json";
        return client.getWithAccept(path_slice, accept, allocator);
      }
    }
    return error.ConnectionFailed;
  }

  pub const MetadataResult = struct {
    name: []const u8,
    data: ?[]u8,
    compressed: bool,
    has_error: bool,
  };

  pub fn fetchMetadataBatch(self: *Fetcher, names: []const []const u8, allocator: std.mem.Allocator) ![]MetadataResult {
    if (names.len == 0) return &[_]MetadataResult{};

    var total_start: u64 = @intCast(std.time.nanoTimestamp());
    try self.ensureMetaClients();
    total_start = debug.timer("  meta: get clients", total_start);

    var active_connections: usize = 0;
    for (self.meta_clients) |maybe_client| {
      if (maybe_client != null) active_connections += 1;
    } if (active_connections == 0) return error.ConnectionFailed;

    debug.log("  meta: batch {d} packages across {d} connections", .{ names.len, active_connections });

    var results = try allocator.alloc(MetadataResult, names.len);
    for (results, 0..) |*r, i| {
      r.* = .{ .name = names[i], .data = null, .compressed = false, .has_error = false };
    }

    const total_capacity = active_connections * (MAX_PENDING_REQUESTS - 1);
    var offset: usize = 0;
    var batch_num: usize = 0;

    var decompress_buf = std.ArrayListUnmanaged(u8){};
    defer decompress_buf.deinit(allocator);

    while (offset < names.len) {
      const end = @min(offset + total_capacity, names.len);
      var batch_start: u64 = @intCast(std.time.nanoTimestamp());
      debug.log("  meta: batch {d} ({d}-{d})", .{ batch_num, offset, end });

      var queued: usize = 0;
      var conn_idx: usize = 0;
      for (offset..end) |i| {
        const result = &results[i];
        const name = names[i];

        var client: ?*Http2Client = null;
        var attempts: usize = 0;
        while (attempts < NUM_META_CONNECTIONS) : (attempts += 1) {
          if (self.meta_clients[conn_idx]) |c| {
            if (c.h2_session != null and c.connected == 1 and c.request_count < MAX_PENDING_REQUESTS - 1) client = c; break;
          }
          conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
        }

        if (client == null) {
          result.has_error = true; continue;
        }

        const c = client.?;
        const session = c.h2_session orelse {
          result.has_error = true; continue;
        };
        
        var path_buf: [512]u8 = undefined;
        const path = std.fmt.bufPrint(&path_buf, "/{s}", .{name}) catch {
          result.has_error = true;
          continue;
        };

        var hdrs = [_]nghttp2.nv{
          Http2Client.makeNv(":method", "GET"),
          Http2Client.makeNv(":path", c.allocator.dupeZ(u8, path) catch {
            result.has_error = true; continue;
          }),
          Http2Client.makeNv(":scheme", "https"),
          Http2Client.makeNv(":authority", c.host),
          Http2Client.makeNv("accept", "application/vnd.npm.install-v1+json"),
          Http2Client.makeNv("accept-encoding", "gzip"),
          Http2Client.makeNv("user-agent", user_agent),
        };

        const req = &c.requests[c.request_count];
        c.request_count += 1;
        req.* = .{
          .stream_id = 0,
          .path = hdrs[1].value[0..hdrs[1].valuelen :0],
          .on_data = null,
          .on_complete = null,
          .on_error = null,
          .userdata = result,
          .response_body = .{},
          .status_code = 0,
          .done = false,
          .has_error = false,
          .start_ns = @intCast(std.time.nanoTimestamp()),
          .end_ns = 0,
          .bytes = 0,
          .content_encoding = .identity,
        };

        const sid = nghttp2.nghttp2_submit_request(session, null, &hdrs, hdrs.len, null, req);
        if (sid < 0) {
          c.request_count -= 1;
          if (req.path) |p| c.allocator.free(p);
          result.has_error = true;
          continue;
        }
        req.stream_id = sid;
        queued += 1;
        conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
      }
      batch_start = debug.timer("  meta: queue requests", batch_start);

      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| c.flush() catch {};
      }

      const loop = uv.uv_default_loop();
      var all_done = false;
      var loops: usize = 0;
      const run_start: u64 = @intCast(std.time.nanoTimestamp());

      while (!all_done) {
        _ = uv.uv_run(loop, uv.RUN_ONCE);
        loops += 1;

        all_done = true;
        for (self.meta_clients) |maybe_client| {
          if (maybe_client) |c| {
            for (c.requests[0..c.request_count]) |*req| {
              if (!req.done and !req.has_error) { all_done = false; break; }
            } if (!all_done) break;
          }
        }
      }

      const elapsed_ns: u64 = @intCast(@as(i128, @intCast(std.time.nanoTimestamp())) - @as(i128, run_start));
      debug.log("    h2: run complete in {d}ms, {d} loops", .{ elapsed_ns / 1_000_000, loops });
      batch_start = debug.timer("  meta: run h2 loop", batch_start);

      var slow_count: usize = 0;
      var max_req_ms: u64 = 0;
      var max_req_name: []const u8 = "";
      var total_bytes: usize = 0;
      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| {
          for (c.requests[0..c.request_count]) |*req| {
            const result: *MetadataResult = @ptrCast(@alignCast(req.userdata));
            const end_ns = if (req.end_ns != 0) req.end_ns else @as(u64, @intCast(std.time.nanoTimestamp()));
            const duration_ms: u64 = @intCast((end_ns - req.start_ns) / 1_000_000);
            total_bytes += req.response_body.items.len;
            if (duration_ms > max_req_ms) {
              max_req_ms = duration_ms;
              max_req_name = result.name;
            }
            if (duration_ms >= META_SLOW_LOG_MS) {
              slow_count += 1;
              debug.log("    meta: slow {s} {d}ms {d} bytes status={d}", .{
                result.name,
                duration_ms,
                req.response_body.items.len,
                req.status_code,
              });
            }
          }
        }
      }
      debug.log("    meta: summary slow={d} max={s} {d}ms total_bytes={d}", .{ slow_count, max_req_name, max_req_ms, total_bytes });

      var success: usize = 0;
      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| {
          for (c.requests[0..c.request_count]) |*req| {
            const result: *MetadataResult = @ptrCast(@alignCast(req.userdata));
            if (req.has_error or req.status_code != 200) {
              result.has_error = true;
            } else {
              if (req.content_encoding == .gzip) {
                decompress_buf.clearRetainingCapacity();
                const decomp = extractor.GzipDecompressor.init(allocator) catch {
                  result.has_error = true;
                  continue;
                };
                defer decomp.deinit();
                _ = decomp.decompress(req.response_body.items, struct {
                  fn onChunk(data: []const u8, ctx: ?*anyopaque) anyerror!void {
                    const buf: *std.ArrayListUnmanaged(u8) = @ptrCast(@alignCast(ctx));
                    try buf.appendSlice(c_allocator, data);
                  }
                }.onChunk, &decompress_buf) catch {
                  result.has_error = true;
                  continue;
                };
                result.data = allocator.dupe(u8, decompress_buf.items) catch null;
                result.compressed = true;
              } else {
                result.data = allocator.dupe(u8, req.response_body.items) catch null;
              }
              if (result.data == null) result.has_error = true else success += 1;
            }
          }
          c.resetRequests();
        }
      }
      _ = debug.timer("  meta: copy results", batch_start);
      debug.log("  meta: queued={d} success={d}", .{ queued, success });

      offset = end;
      batch_num += 1;
    }

    return results;
  }

  pub const MetadataCallback = *const fn (
    name: []const u8,
    data: ?[]const u8,
    has_error: bool,
    userdata: ?*anyopaque
  ) void;

  pub fn fetchMetadataStreaming(
    self: *Fetcher,
    names: []const []const u8,
    allocator: std.mem.Allocator,
    callback: MetadataCallback,
    userdata: ?*anyopaque,
  ) !void {
    if (names.len == 0) return;

    var total_start: u64 = @intCast(std.time.nanoTimestamp());
    try self.ensureMetaClients();
    total_start = debug.timer("  meta: get clients", total_start);

    var active_connections: usize = 0;
    for (self.meta_clients) |maybe_client| {
      if (maybe_client != null) active_connections += 1;
    }
    
    if (active_connections == 0) return error.ConnectionFailed;
    debug.log("  meta: streaming {d} packages across {d} connections", .{ names.len, active_connections });

    var processed = try allocator.alloc(bool, names.len);
    defer allocator.free(processed);
    @memset(processed, false);

    const ResultTracker = struct {
      name: []const u8,
      index: usize,
    };
    
    var trackers = try allocator.alloc(ResultTracker, names.len);
    defer allocator.free(trackers);
    for (names, 0..) |name, i| {
      trackers[i] = .{ .name = name, .index = i };
    }

    const total_capacity = active_connections * (MAX_PENDING_REQUESTS - 1);
    var offset: usize = 0;
    var batch_num: usize = 0;

    var decompress_buf = std.ArrayListUnmanaged(u8){};
    defer decompress_buf.deinit(allocator);

    while (offset < names.len) {
      const end = @min(offset + total_capacity, names.len);
      var batch_start: u64 = @intCast(std.time.nanoTimestamp());

      debug.log("  meta: batch {d} ({d}-{d})", .{ batch_num, offset, end });

      var queued: usize = 0;
      var conn_idx: usize = 0;
      for (offset..end) |i| {
        const tracker = &trackers[i];
        const name = names[i];

        var client: ?*Http2Client = null;
        var attempts: usize = 0;
        while (attempts < NUM_META_CONNECTIONS) : (attempts += 1) {
          if (self.meta_clients[conn_idx]) |c| {
            if (c.h2_session != null and c.connected == 1 and c.request_count < MAX_PENDING_REQUESTS - 1) client = c; break;
          } conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
        } if (client == null) continue;

        const c = client.?;
        const session = c.h2_session orelse continue;
        
        var path_buf: [512]u8 = undefined;
        const path = std.fmt.bufPrint(&path_buf, "/{s}", .{name}) catch continue;

        var hdrs = [_]nghttp2.nv{
          Http2Client.makeNv(":method", "GET"),
          Http2Client.makeNv(":path", c.allocator.dupeZ(u8, path) catch continue),
          Http2Client.makeNv(":scheme", "https"),
          Http2Client.makeNv(":authority", c.host),
          Http2Client.makeNv("accept", "application/vnd.npm.install-v1+json"),
          Http2Client.makeNv("accept-encoding", "gzip"),
          Http2Client.makeNv("user-agent", user_agent),
        };

        const req = &c.requests[c.request_count];
        c.request_count += 1;
        req.* = .{
          .stream_id = 0,
          .path = hdrs[1].value[0..hdrs[1].valuelen :0],
          .on_data = null,
          .on_complete = null,
          .on_error = null,
          .userdata = tracker,
          .response_body = .{},
          .status_code = 0,
          .done = false,
          .has_error = false,
          .start_ns = 0,
          .end_ns = 0,
          .bytes = 0,
          .content_encoding = .identity,
        };
        req.start_ns = @intCast(std.time.nanoTimestamp());

        const sid = nghttp2.nghttp2_submit_request(session, null, &hdrs, hdrs.len, null, req);
        if (sid < 0) {
          c.request_count -= 1;
          if (req.path) |p| c.allocator.free(p);
          continue;
        }
        req.stream_id = sid;
        queued += 1;
        conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
      }
      batch_start = debug.timer("  meta: queue requests", batch_start);

      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| c.flush() catch {};
      }

      const loop = uv.uv_default_loop();
      var all_done = false;
      var loops: usize = 0;
      const run_start: u64 = @intCast(std.time.nanoTimestamp());

      while (!all_done) {
        _ = uv.uv_run(loop, uv.RUN_ONCE);
        loops += 1;

        for (self.meta_clients) |maybe_client| {
          if (maybe_client) |c| {
            for (c.requests[0..c.request_count]) |*req| {
              if (req.done or req.has_error) {
                const tracker: *ResultTracker = @ptrCast(@alignCast(req.userdata));
                if (!processed[tracker.index]) {
                  processed[tracker.index] = true;
                  if (req.has_error or req.status_code != 200) {
                    callback(tracker.name, null, true, userdata);
                  } else if (req.content_encoding == .gzip) {
                    decompress_buf.clearRetainingCapacity();
                    const decomp = extractor.GzipDecompressor.init(allocator) catch {
                      callback(tracker.name, null, true, userdata);
                      continue;
                    };
                    defer decomp.deinit();
                    _ = decomp.decompress(req.response_body.items, struct {
                      fn onChunk(data: []const u8, ctx: ?*anyopaque) anyerror!void {
                        const buf: *std.ArrayListUnmanaged(u8) = @ptrCast(@alignCast(ctx));
                        try buf.appendSlice(c_allocator, data);
                      }
                    }.onChunk, &decompress_buf) catch {
                      callback(tracker.name, null, true, userdata);
                      continue;
                    };
                    callback(tracker.name, decompress_buf.items, false, userdata);
                  } else {
                    callback(tracker.name, req.response_body.items, false, userdata);
                  }
                }
              }
            }
          }
        }

        all_done = true;
        for (self.meta_clients) |maybe_client| {
          if (maybe_client) |c| {
            for (c.requests[0..c.request_count]) |*req| {
              if (!req.done and !req.has_error) {
                all_done = false;
                break;
              }
            }
            if (!all_done) break;
          }
        }
      }

      const elapsed_ns: u64 = @intCast(@as(i128, @intCast(std.time.nanoTimestamp())) - @as(i128, run_start));
      debug.log("    h2: run complete in {d}ms, {d} loops", .{ elapsed_ns / 1_000_000, loops });

      var slow_count: usize = 0;
      var max_req_ms: u64 = 0;
      var max_req_name: []const u8 = "";
      var total_bytes: usize = 0;
      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| {
          for (c.requests[0..c.request_count]) |*req| {
            const tracker: *ResultTracker = @ptrCast(@alignCast(req.userdata));
            const end_ns = if (req.end_ns != 0) req.end_ns else @as(u64, @intCast(std.time.nanoTimestamp()));
            const duration_ms: u64 = @intCast((end_ns - req.start_ns) / 1_000_000);
            total_bytes += req.response_body.items.len;
            if (duration_ms > max_req_ms) {
              max_req_ms = duration_ms;
              max_req_name = tracker.name;
            }
            if (duration_ms >= META_SLOW_LOG_MS) {
              slow_count += 1;
              debug.log("    meta: slow {s} {d}ms {d} bytes status={d}", .{
                tracker.name,
                duration_ms,
                req.response_body.items.len,
                req.status_code,
              });
            }
          }
        }
      }
      debug.log("    meta: summary slow={d} max={s} {d}ms total_bytes={d}", .{ slow_count, max_req_name, max_req_ms, total_bytes });

      for (self.meta_clients) |maybe_client| {
        if (maybe_client) |c| c.resetRequests();
      }

      offset = end;
      batch_num += 1;
    }
  }

  pub fn fetchTarball(self: *Fetcher, url: []const u8, handler: StreamHandler) !void {
    try self.pending.append(self.allocator, .{ .url = try self.allocator.dupe(u8, url), .handler = handler });
  }

  pub fn run(self: *Fetcher) !void {
    if (self.pending.items.len == 0 and self.tarball_contexts.items.len == 0) return;

    const run_start: u64 = @intCast(std.time.nanoTimestamp());
    const total_requests = self.pending.items.len + self.tarball_contexts.items.len;
    
    debug.log("fetcher: {d} tarballs to download (pending={d}, in-flight={d})", .{
      total_requests,
      self.pending.items.len,
      self.tarball_contexts.items.len,
    });

    try self.ensureTarballClients();
    self.finishTarballs();

    const elapsed_ns: u64 = @intCast(@as(i128, @intCast(std.time.nanoTimestamp())) - @as(i128, run_start));
    debug.log("fetcher: {d} tarballs complete in {d}ms", .{ total_requests, elapsed_ns / 1_000_000 });
  }
};
