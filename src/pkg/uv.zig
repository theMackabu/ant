const builtin = @import("builtin");

const c = @cImport({
  if (builtin.os.tag.isDarwin()) {
    @cInclude("mach/port.h");
    @cUndef("xnu_static_assert_struct_size");
    @cDefine("xnu_static_assert_struct_size(name, expected_size)", "");
  }
  @cInclude("uv.h");
});

pub const loop_t = c.uv_loop_t;
pub const handle_t = c.uv_handle_t;
pub const stream_t = c.uv_stream_t;
pub const buf_t = c.uv_buf_t;
pub const connect_t = c.uv_connect_t;
pub const write_t = c.uv_write_t;

pub const RUN_DEFAULT = c.UV_RUN_DEFAULT;
pub const RUN_ONCE = c.UV_RUN_ONCE;
pub const RUN_NOWAIT = c.UV_RUN_NOWAIT;

pub const connect_cb = ?*const fn (*connect_t, c_int) callconv(.c) void;
pub const close_cb = ?*const fn (*handle_t) callconv(.c) void;
pub const alloc_cb = ?*const fn (*handle_t, usize, *buf_t) callconv(.c) void;
pub const read_cb = ?*const fn (*stream_t, isize, *const buf_t) callconv(.c) void;
pub const write_cb = ?*const fn (*write_t, c_int) callconv(.c) void;

pub const uv_default_loop = c.uv_default_loop;
pub const uv_run = c.uv_run;
