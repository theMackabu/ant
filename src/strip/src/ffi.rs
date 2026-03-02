use std::alloc::{Layout, alloc, dealloc};
use std::ffi::{CStr, c_char, c_int};
use std::ptr;

use crate::collector::{collect_var_names, collect_var_names_from_func};
use crate::strip::strip_types_internal;

pub const OXC_ERR_NULL_INPUT: c_int = -1;
pub const OXC_ERR_INVALID_UTF8: c_int = -2;
pub const OXC_ERR_PARSE_FAILED: c_int = -3;
pub const OXC_ERR_TRANSFORM_FAILED: c_int = -4;
pub const OXC_ERR_OUTPUT_TOO_LARGE: c_int = -5;

fn classify_strip_error(err: &str) -> c_int {
  match ["Parse errors:", "Semantic errors:"].iter().any(|p| err.starts_with(p)) {
    true => OXC_ERR_PARSE_FAILED,
    false => OXC_ERR_TRANSFORM_FAILED,
  }
}

unsafe fn write_error(output: *mut c_char, output_len: usize, msg: &str) {
  if output.is_null() || output_len == 0 {
    return;
  }

  let bytes = msg.as_bytes();
  let copy_len = bytes.len().min(output_len.saturating_sub(1));

  unsafe {
    ptr::copy_nonoverlapping(bytes.as_ptr(), output as *mut u8, copy_len);
    *output.add(copy_len) = 0;
  }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_strip_types_owned(
  input: *const c_char, filename: *const c_char, out_len: *mut usize, out_error: *mut c_int, error_output: *mut c_char, error_output_len: usize,
) -> *mut c_char {
  if !out_error.is_null() {
    unsafe { *out_error = OXC_ERR_NULL_INPUT };
  }

  if !out_len.is_null() {
    unsafe { *out_len = 0 };
  }

  if input.is_null() || filename.is_null() || out_len.is_null() {
    unsafe { write_error(error_output, error_output_len, "null input/output passed") };
    return ptr::null_mut();
  }

  let filename_str = match unsafe { CStr::from_ptr(filename).to_str() } {
    Ok(s) => s,
    Err(_) => {
      unsafe { write_error(error_output, error_output_len, "filename is not valid UTF-8") };
      if !out_error.is_null() {
        unsafe { *out_error = OXC_ERR_INVALID_UTF8 };
      }
      return ptr::null_mut();
    }
  };

  let input_str = match unsafe { CStr::from_ptr(input).to_str() } {
    Ok(s) => s,
    Err(_) => {
      unsafe { write_error(error_output, error_output_len, "source input is not valid UTF-8") };
      if !out_error.is_null() {
        unsafe { *out_error = OXC_ERR_INVALID_UTF8 };
      }
      return ptr::null_mut();
    }
  };

  match strip_types_internal(input_str, filename_str) {
    Ok(result) => {
      let bytes = result.as_bytes();
      let alloc_len = bytes.len() + 1;
      let layout = Layout::from_size_align(alloc_len, 1).unwrap();
      let out_ptr = unsafe { alloc(layout) as *mut c_char };
      if out_ptr.is_null() {
        unsafe { write_error(error_output, error_output_len, "out of memory allocating strip output") };
        if !out_error.is_null() {
          unsafe { *out_error = OXC_ERR_OUTPUT_TOO_LARGE };
        }
        return ptr::null_mut();
      }

      unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), out_ptr as *mut u8, bytes.len());
        *out_ptr.add(bytes.len()) = 0;
        *out_len = bytes.len();
      }
      if !out_error.is_null() {
        unsafe { *out_error = 0 };
      }
      out_ptr
    }
    Err(err) => {
      unsafe { write_error(error_output, error_output_len, &err) };
      if !out_error.is_null() {
        unsafe { *out_error = classify_strip_error(&err) };
      }
      ptr::null_mut()
    }
  }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_free_stripped_output(ptr: *mut c_char, len: usize) {
  if ptr.is_null() {
    return;
  }

  let alloc_len = len.saturating_add(1);
  let layout = Layout::from_size_align(alloc_len, 1).unwrap();

  unsafe {
    dealloc(ptr as *mut u8, layout);
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
