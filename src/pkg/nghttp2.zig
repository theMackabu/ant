pub const session = opaque {};
pub const session_callbacks = opaque {};

pub const frame_hd = extern struct {
  length: usize,
  stream_id: i32,
  type: u8,
  flags: u8,
  reserved: u8,
};

pub const frame = extern struct {
  hd: frame_hd,
  _pad: [256]u8 = undefined,
};

pub const nv = extern struct {
  name: [*c]u8,
  value: [*c]u8,
  namelen: usize,
  valuelen: usize,
  flags: u8,
};

pub const settings_entry = extern struct {
  settings_id: i32,
  value: u32,
};

pub const FLAG_NONE: u8 = 0;
pub const FLAG_END_STREAM: u8 = 0x01;
pub const NV_FLAG_NONE: u8 = 0;
pub const HEADERS: u8 = 0x01;
pub const SETTINGS_MAX_CONCURRENT_STREAMS: i32 = 0x03;
pub const SETTINGS_INITIAL_WINDOW_SIZE: i32 = 0x04;
pub const ERR_NOMEM: isize = -901;

pub const send_callback2 = ?*const fn (
  ?*session,
  [*c]const u8,
  usize,
  c_int,
  ?*anyopaque,
) callconv(.c) isize;

pub const on_frame_recv_callback = ?*const fn (
  ?*session,
  *const frame,
  ?*anyopaque,
) callconv(.c) c_int;

pub const on_data_chunk_recv_callback = ?*const fn (
  ?*session,
  u8,
  i32,
  [*c]const u8,
  usize,
  ?*anyopaque,
) callconv(.c) c_int;

pub const on_header_callback = ?*const fn (
  ?*session,
  *const frame,
  [*c]const u8,
  usize,
  [*c]const u8,
  usize,
  u8,
  ?*anyopaque,
) callconv(.c) c_int;

pub const on_stream_close_callback = ?*const fn (
  ?*session,
  i32,
  u32,
  ?*anyopaque,
) callconv(.c) c_int;

pub extern fn nghttp2_session_callbacks_new(**session_callbacks) c_int;
pub extern fn nghttp2_session_callbacks_del(*session_callbacks) void;
pub extern fn nghttp2_session_callbacks_set_send_callback2(*session_callbacks, send_callback2) void;
pub extern fn nghttp2_session_callbacks_set_on_frame_recv_callback(*session_callbacks, on_frame_recv_callback) void;
pub extern fn nghttp2_session_callbacks_set_on_data_chunk_recv_callback(*session_callbacks, on_data_chunk_recv_callback) void;
pub extern fn nghttp2_session_callbacks_set_on_header_callback(*session_callbacks, on_header_callback) void;
pub extern fn nghttp2_session_callbacks_set_on_stream_close_callback(*session_callbacks, on_stream_close_callback) void;
pub extern fn nghttp2_session_client_new(**session, *session_callbacks, ?*anyopaque) c_int;
pub extern fn nghttp2_session_del(*session) void;
pub extern fn nghttp2_session_send(*session) c_int;
pub extern fn nghttp2_session_mem_recv(*session, [*]const u8, usize) isize;
pub extern fn nghttp2_session_want_write(*session) c_int;
pub extern fn nghttp2_session_consume(*session, i32, usize) c_int;
pub extern fn nghttp2_submit_settings(*session, u8, [*]settings_entry, usize) c_int;
pub extern fn nghttp2_submit_request(*session, ?*anyopaque, [*]nv, usize, ?*anyopaque, ?*anyopaque) i32;
pub extern fn nghttp2_submit_window_update(*session, u8, i32, i32) c_int;