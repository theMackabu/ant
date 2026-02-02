pub const loop_t = opaque {};
pub const handle_t = opaque {};
pub const stream_t = opaque {};

pub const buf_t = extern struct {
    base: [*c]u8,
    len: usize,
};

pub const connect_t = extern struct {
    data: ?*anyopaque = null,
    _pad: [256]u8 = undefined,
};

pub const write_t = extern struct {
    data: ?*anyopaque = null,
    _pad: [256]u8 = undefined,
};

pub const RUN_DEFAULT: c_int = 0;
pub const RUN_ONCE: c_int = 1;
pub const RUN_NOWAIT: c_int = 2;

pub const connect_cb = ?*const fn (*connect_t, c_int) callconv(.c) void;
pub const close_cb = ?*const fn (*handle_t) callconv(.c) void;
pub const alloc_cb = ?*const fn (*handle_t, usize, *buf_t) callconv(.c) void;
pub const read_cb = ?*const fn (*stream_t, isize, *const buf_t) callconv(.c) void;
pub const write_cb = ?*const fn (*write_t, c_int) callconv(.c) void;

pub extern fn uv_default_loop() *loop_t;
pub extern fn uv_run(*loop_t, c_int) c_int;