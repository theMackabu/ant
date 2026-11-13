const VALUE_UNDEFINED: u8 = 0;
const VALUE_NULL: u8 = 1;
const VALUE_BOOL: u8 = 2;
const VALUE_NUMBER: u8 = 3;
const VALUE_STRING: u8 = 4;
const VALUE_DISPLAY: u8 = 5;

pub(crate) const ERROR_FIELDS: usize = 4;
pub(crate) const ERROR_DISPLAY: usize = 3;

struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
  fn take(&mut self, len: usize) -> Option<&'a [u8]> {
    let (head, rest) = self.0.split_at_checked(len)?;
    self.0 = rest;
    Some(head)
  }

  fn u8(&mut self) -> Option<u8> {
    self.take(1).map(|bytes| bytes[0])
  }

  fn u32(&mut self) -> Option<u32> {
    Some(u32::from_le_bytes(*self.take(4)?.first_chunk()?))
  }

  fn u64(&mut self) -> Option<u64> {
    Some(u64::from_le_bytes(*self.take(8)?.first_chunk()?))
  }

  fn string(&mut self) -> Option<&'a [u8]> {
    let len = usize::try_from(self.u32()?).ok()?;
    self.take(len)
  }

  fn finish<T>(&self, value: T) -> Option<T> {
    self.0.is_empty().then_some(value)
  }
}

#[derive(Debug, PartialEq)]
pub(crate) struct ResultPayload<'a> {
  pub kind: u8,
  pub bits: u64,
  pub text: &'a [u8],
  pub display: &'a [u8],
}

pub(crate) fn parse_result(payload: &[u8]) -> Option<ResultPayload<'_>> {
  let mut reader = Reader(payload);
  let kind = reader.u8()?;
  let mut bits = 0;
  let mut text: &[u8] = &[];
  match kind {
    VALUE_UNDEFINED | VALUE_NULL => {}
    VALUE_BOOL => bits = u64::from(reader.u8()? != 0),
    VALUE_NUMBER => bits = reader.u64()?,
    VALUE_STRING => text = reader.string()?,
    VALUE_DISPLAY => {
      let display = reader.string()?;
      return reader.finish(ResultPayload { kind, bits, text: display, display });
    }
    _ => return None,
  }
  let display = reader.string()?;
  reader.finish(ResultPayload { kind, bits, text, display })
}

pub(crate) fn parse_error(payload: &[u8]) -> Option<[&[u8]; ERROR_FIELDS]> {
  let mut reader = Reader(payload);
  let mut fields: [&[u8]; ERROR_FIELDS] = [&[]; ERROR_FIELDS];
  for field in &mut fields {
    *field = reader.string()?;
  }
  reader.finish(fields)
}

#[cfg(not(test))]
mod ffi {
  use super::*;
  use core::ffi::{c_char, c_void};

  unsafe fn payload<'a>(ptr: *const c_void, len: usize) -> Option<&'a [u8]> {
    (!ptr.is_null()).then(|| core::slice::from_raw_parts(ptr.cast::<u8>(), len))
  }

  unsafe fn store(out: *mut *const c_char, out_len: *mut usize, bytes: &[u8]) {
    *out = bytes.as_ptr().cast();
    *out_len = bytes.len();
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_sandbox_result_payload_parse(
    ptr: *const c_void,
    len: usize,
    kind: *mut u8,
    bits: *mut u64,
    text: *mut *const c_char,
    text_len: *mut usize,
  ) -> bool {
    if kind.is_null() || bits.is_null() || text.is_null() || text_len.is_null() {
      return false;
    }
    let Some(parsed) = payload(ptr, len).and_then(parse_result) else {
      return false;
    };
    *kind = parsed.kind;
    *bits = parsed.bits;
    store(text, text_len, parsed.text);
    true
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_sandbox_result_payload_display(
    ptr: *const c_void,
    len: usize,
    out: *mut *const c_char,
    out_len: *mut usize,
  ) -> bool {
    if out.is_null() || out_len.is_null() {
      return false;
    }
    let Some(parsed) = payload(ptr, len).and_then(parse_result) else {
      return false;
    };
    store(out, out_len, parsed.display);
    true
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_sandbox_error_payload_field(
    ptr: *const c_void,
    len: usize,
    index: usize,
    out: *mut *const c_char,
    out_len: *mut usize,
  ) -> bool {
    if out.is_null() || out_len.is_null() {
      return false;
    }
    let Some(field) = payload(ptr, len).and_then(parse_error).and_then(|fields| fields.get(index).copied()) else {
      return false;
    };
    store(out, out_len, field);
    true
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_sandbox_error_payload_display(
    ptr: *const c_void,
    len: usize,
    out: *mut *const c_char,
    out_len: *mut usize,
  ) -> bool {
    ant_sandbox_error_payload_field(ptr, len, ERROR_DISPLAY, out, out_len)
  }
}

#[cfg(test)]
#[path = "tests/wire.rs"]
mod tests;
