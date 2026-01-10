use std::ffi::{c_char, c_int, CStr};
use std::path::Path;
use std::ptr;

use oxc_allocator::Allocator;
use oxc_codegen::Codegen;
use oxc_parser::Parser;
use oxc_semantic::SemanticBuilder;
use oxc_span::SourceType;
use oxc_transformer::{TransformOptions, Transformer, TypeScriptOptions};

pub const OXC_ERR_NULL_INPUT: c_int = -1;
pub const OXC_ERR_INVALID_UTF8: c_int = -2;
pub const OXC_ERR_TRANSFORM_FAILED: c_int = -4;
pub const OXC_ERR_OUTPUT_TOO_LARGE: c_int = -5;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn OXC_strip_types(
  input: *const c_char,
  filename: *const c_char,
  output: *mut c_char,
  output_len: usize,
) -> c_int {
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
      if bytes.len() + 1 > output_len { return OXC_ERR_OUTPUT_TOO_LARGE; }
      unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), output as *mut u8, bytes.len());
        *output.add(bytes.len()) = 0;
      }
      bytes.len() as c_int
    }
    Err(_) => OXC_ERR_TRANSFORM_FAILED,
  }
}

fn strip_types_internal(source: &str, filename: &str) -> Result<String, String> {
  let allocator = Allocator::default();
  let source_type = SourceType::from_path(filename).unwrap_or_else(|_| SourceType::ts());
  let parser_ret = Parser::new(&allocator, source, source_type).parse();

  if !parser_ret.errors.is_empty() {
    let errors: Vec<String> = parser_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Parse errors: {}", errors.join("; ")));
  }

  let mut program = parser_ret.program;
  let semantic_ret = SemanticBuilder::new().build(&program);

  if !semantic_ret.errors.is_empty() {
    let errors: Vec<String> = semantic_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Semantic errors: {}", errors.join("; ")));
  }

  let scoping = semantic_ret.semantic.into_scoping();
  let transform_options = TransformOptions {
    typescript: TypeScriptOptions {
      only_remove_type_imports: true,
      ..Default::default()
    },
    ..Default::default()
  };

  let source_path = Path::new(filename);
  let transformer = Transformer::new(&allocator, source_path, &transform_options);
  let ret = transformer.build_with_scoping(scoping, &mut program);

  if !ret.errors.is_empty() {
    let errors: Vec<String> = ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Transform errors: {}", errors.join("; ")));
  }

  let output = Codegen::new().build(&program).code;
  Ok(output)
}
