mod collector;
mod ffi;
mod strip;

pub use collector::{collect_var_names, collect_var_names_from_func};
pub use ffi::*;
pub use strip::strip_types_internal;
