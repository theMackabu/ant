use std::ffi::{CStr, c_char, c_int};
use std::ptr;

use crate::collector::{collect_var_names, collect_var_names_from_func};
use crate::strip::strip_types_internal;

pub const OXC_ERR_NULL_INPUT: c_int = -1;
pub const OXC_ERR_INVALID_UTF8: c_int = -2;
pub const OXC_ERR_TRANSFORM_FAILED: c_int = -4;
pub const OXC_ERR_OUTPUT_TOO_LARGE: c_int = -5;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_strip_types(input: *const c_char, filename: *const c_char, output: *mut c_char, output_len: usize) -> c_int {
  if input.is_null() || output.is_null() {
    return OXC_ERR_NULL_INPUT;
  }

  let filename_str = match unsafe { CStr::from_ptr(filename).to_str() } {
    Ok(s) => s,
    Err(_) => return OXC_ERR_INVALID_UTF8,
  };

  let input_str = match unsafe { CStr::from_ptr(input).to_str() } {
    Ok(s) => s,
    Err(_) => return OXC_ERR_INVALID_UTF8,
  };

  match strip_types_internal(input_str, filename_str) {
    Ok(result) => {
      let bytes = result.as_bytes();
      if bytes.len() + 1 > output_len {
        return OXC_ERR_OUTPUT_TOO_LARGE;
      }
      unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), output as *mut u8, bytes.len());
        *output.add(bytes.len()) = 0;
      }
      bytes.len() as c_int
    }
    Err(_) => OXC_ERR_TRANSFORM_FAILED,
  }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_get_hoisted_vars(input: *const c_char, input_len: usize, out_len: *mut usize) -> *mut c_char {
  if input.is_null() || out_len.is_null() {
    return ptr::null_mut();
  }

  let input_slice = unsafe { std::slice::from_raw_parts(input as *const u8, input_len) };
  let input_str = match std::str::from_utf8(input_slice) {
    Ok(s) => s,
    Err(_) => return ptr::null_mut(),
  };

  match collect_var_names(input_str) {
    Ok(vars) => {
      if vars.is_empty() {
        return ptr::null_mut();
      }
      let total_len: usize = vars.iter().map(|s| s.len() + 1).sum::<usize>() + 1;

      let layout = std::alloc::Layout::from_size_align(total_len, 1).unwrap();
      let ptr = unsafe { std::alloc::alloc(layout) as *mut c_char };
      if ptr.is_null() {
        return ptr::null_mut();
      }

      let mut offset = 0;
      for v in &vars {
        unsafe {
          ptr::copy_nonoverlapping(v.as_ptr(), ptr.add(offset) as *mut u8, v.len());
          *ptr.add(offset + v.len()) = 0;
        }
        offset += v.len() + 1;
      }
      unsafe {
        *ptr.add(offset) = 0;
        *out_len = total_len;
      }

      ptr
    }
    Err(_) => ptr::null_mut(),
  }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_get_func_hoisted_vars(input: *const c_char, input_len: usize, out_len: *mut usize) -> *mut c_char {
  if input.is_null() || out_len.is_null() {
    return ptr::null_mut();
  }

  let input_slice = unsafe { std::slice::from_raw_parts(input as *const u8, input_len) };
  let input_str = match std::str::from_utf8(input_slice) {
    Ok(s) => s,
    Err(_) => return ptr::null_mut(),
  };

  match collect_var_names_from_func(input_str) {
    Ok(vars) => {
      if vars.is_empty() {
        return ptr::null_mut();
      }
      let total_len: usize = vars.iter().map(|s| s.len() + 1).sum::<usize>() + 1;

      let layout = std::alloc::Layout::from_size_align(total_len, 1).unwrap();
      let ptr = unsafe { std::alloc::alloc(layout) as *mut c_char };

      if ptr.is_null() {
        return ptr::null_mut();
      }

      let mut offset = 0;
      for v in &vars {
        unsafe {
          ptr::copy_nonoverlapping(v.as_ptr(), ptr.add(offset) as *mut u8, v.len());
          *ptr.add(offset + v.len()) = 0;
        }
        offset += v.len() + 1;
      }

      unsafe {
        *ptr.add(offset) = 0;
        *out_len = total_len;
      }

      ptr
    }
    Err(_) => ptr::null_mut(),
  }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_free_hoisted_vars(ptr: *mut c_char, len: usize) {
  if ptr.is_null() || len == 0 {
    return;
  }
  let layout = std::alloc::Layout::from_size_align(len, 1).unwrap();
  unsafe {
    std::alloc::dealloc(ptr as *mut u8, layout);
  }
}
