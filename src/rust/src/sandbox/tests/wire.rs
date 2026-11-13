use super::*;

fn string(text: &str) -> Vec<u8> {
  let mut out = (text.len() as u32).to_le_bytes().to_vec();
  out.extend_from_slice(text.as_bytes());
  out
}

fn result(kind: u8, body: &[u8], display: &str) -> Vec<u8> {
  let mut out = vec![kind];
  out.extend_from_slice(body);
  out.extend(string(display));
  out
}

#[test]
fn result_values_decode() {
  let payload = result(VALUE_UNDEFINED, &[], "undefined");
  let parsed = parse_result(&payload).unwrap();
  assert_eq!((parsed.kind, parsed.display), (VALUE_UNDEFINED, &b"undefined"[..]));

  let payload = result(VALUE_BOOL, &[7], "true");
  let parsed = parse_result(&payload).unwrap();
  assert_eq!((parsed.kind, parsed.bits), (VALUE_BOOL, 1));

  let number = 1.5f64.to_bits();
  let payload = result(VALUE_NUMBER, &number.to_le_bytes(), "1.5");
  let parsed = parse_result(&payload).unwrap();
  assert_eq!(f64::from_bits(parsed.bits), 1.5);

  let payload = result(VALUE_STRING, &string("hi"), "'hi'");
  let parsed = parse_result(&payload).unwrap();
  assert_eq!((parsed.text, parsed.display), (&b"hi"[..], &b"'hi'"[..]));
}

#[test]
fn display_only_result_uses_one_string() {
  let mut payload = vec![VALUE_DISPLAY];
  payload.extend(string("{ a: 1 }"));
  let parsed = parse_result(&payload).unwrap();
  assert_eq!(parsed.text, b"{ a: 1 }");
  assert_eq!(parsed.display, b"{ a: 1 }");

  payload.extend(string("extra"));
  assert_eq!(parse_result(&payload), None);
}

#[test]
fn malformed_results_are_rejected() {
  assert_eq!(parse_result(&[]), None);
  assert_eq!(parse_result(&[9, 0, 0, 0, 0]), None);
  assert_eq!(parse_result(&[VALUE_BOOL]), None);
  assert_eq!(parse_result(&[VALUE_NUMBER, 1, 2, 3]), None);
  assert_eq!(parse_result(&[VALUE_NULL, 0xff, 0xff, 0xff, 0xff]), None);
  assert_eq!(parse_result(&[VALUE_STRING, 5, 0, 0, 0, b'a']), None);

  let mut trailing = result(VALUE_NULL, &[], "null");
  trailing.push(0);
  assert_eq!(parse_result(&trailing), None);
}

#[test]
fn error_fields_decode_in_order() {
  let payload: Vec<u8> = ["TypeError", "bad", "at x", "TypeError: bad"].iter().flat_map(|text| string(text)).collect();
  let fields = parse_error(&payload).unwrap();
  assert_eq!(fields[0], b"TypeError");
  assert_eq!(fields[1], b"bad");
  assert_eq!(fields[2], b"at x");
  assert_eq!(fields[ERROR_DISPLAY], b"TypeError: bad");
}

#[test]
fn malformed_errors_are_rejected() {
  let three: Vec<u8> = ["a", "b", "c"].iter().flat_map(|text| string(text)).collect();
  assert_eq!(parse_error(&three), None);

  let mut five: Vec<u8> = ["a", "b", "c", "d"].iter().flat_map(|text| string(text)).collect();
  five.push(1);
  assert_eq!(parse_error(&five), None);

  assert_eq!(parse_error(&[0xff, 0xff, 0xff, 0x7f]), None);
}
