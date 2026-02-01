const uv = @import("uv.zig");

pub const stream_t = extern struct {
  data: ?*anyopaque = null,
  _pad: [1024]u8 = undefined,
};

pub extern fn tlsuv_stream_init(*uv.loop_t, *stream_t, ?*anyopaque) c_int;
pub extern fn tlsuv_stream_set_hostname(*stream_t, [*:0]const u8) c_int;
pub extern fn tlsuv_stream_set_protocols(*stream_t, c_int, [*]const [*:0]const u8) c_int;
pub extern fn tlsuv_stream_connect(*uv.connect_t, *stream_t, [*:0]const u8, c_int, uv.connect_cb) c_int;
pub extern fn tlsuv_stream_read_start(*stream_t, uv.alloc_cb, uv.read_cb) c_int;
pub extern fn tlsuv_stream_write(*uv.write_t, *stream_t, *uv.buf_t, uv.write_cb) c_int;
pub extern fn tlsuv_stream_close(*stream_t, uv.close_cb) c_int;