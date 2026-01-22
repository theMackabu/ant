use std::ffi::{CStr, c_char, c_int};
use std::path::Path;
use std::ptr;

use oxc_allocator::Allocator;
use oxc_ast::ast::{BindingPattern, VariableDeclarationKind};
use oxc_ast_visit::{Visit, walk};
use oxc_codegen::Codegen;
use oxc_parser::Parser;
use oxc_semantic::{ScopeFlags, SemanticBuilder};
use oxc_span::SourceType;
use oxc_transformer::{TransformOptions, Transformer, TypeScriptOptions};

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

struct VarCollector<'a> {
  vars: Vec<&'a str>,
  func_depth: usize,
  target_depth: usize,
}

impl<'a> VarCollector<'a> {
  fn new() -> Self {
    Self {
      vars: Vec::new(),
      func_depth: 0,
      target_depth: 0,
    }
  }
  fn for_func_body() -> Self {
    Self {
      vars: Vec::new(),
      func_depth: 0,
      target_depth: 1,
    }
  }
}

impl<'a> Visit<'a> for VarCollector<'a> {
  fn visit_function(&mut self, func: &oxc_ast::ast::Function<'a>, flags: ScopeFlags) {
    self.func_depth += 1;
    walk::walk_function(self, func, flags);
    self.func_depth -= 1;
  }

  fn visit_arrow_function_expression(&mut self, expr: &oxc_ast::ast::ArrowFunctionExpression<'a>) {
    self.func_depth += 1;
    walk::walk_arrow_function_expression(self, expr);
    self.func_depth -= 1;
  }

  fn visit_variable_declaration(&mut self, decl: &oxc_ast::ast::VariableDeclaration<'a>) {
    if decl.kind == VariableDeclarationKind::Var && self.func_depth == self.target_depth {
      for declarator in &decl.declarations {
        self.extract_binding_names(&declarator.id);
      }
    }
    walk::walk_variable_declaration(self, decl);
  }
}

impl<'a> VarCollector<'a> {
  fn extract_binding_names(&mut self, pattern: &BindingPattern<'a>) {
    use BindingPattern::*;
    match pattern {
      BindingIdentifier(id) => self.vars.push(id.name.as_str()),
      ObjectPattern(obj) => {
        for prop in &obj.properties {
          self.extract_binding_names(&prop.value);
        }
        if let Some(rest) = &obj.rest {
          self.extract_binding_names(&rest.argument);
        }
      }
      ArrayPattern(arr) => {
        for elem in arr.elements.iter().flatten() {
          self.extract_binding_names(elem);
        }
        if let Some(rest) = &arr.rest {
          self.extract_binding_names(&rest.argument);
        }
      }
      AssignmentPattern(assign) => {
        self.extract_binding_names(&assign.left);
      }
    }
  }
}

fn collect_var_names(source: &str) -> Result<Vec<String>, String> {
  let allocator = Allocator::default();
  let source_type = SourceType::mjs();
  let parser_ret = Parser::new(&allocator, source, source_type).parse();

  if !parser_ret.errors.is_empty() {
    let errors: Vec<String> = parser_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Parse errors: {}", errors.join("; ")));
  }

  let mut collector = VarCollector::new();
  collector.visit_program(&parser_ret.program);

  Ok(collector.vars.into_iter().map(String::from).collect())
}

fn collect_var_names_from_func(source: &str) -> Result<Vec<String>, String> {
  let allocator = Allocator::default();
  let source_type = SourceType::mjs();

  let wrapped = format!("(function{})", source);
  let parser_ret = Parser::new(&allocator, &wrapped, source_type).parse();

  if !parser_ret.errors.is_empty() {
    let errors: Vec<String> = parser_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Parse errors: {}", errors.join("; ")));
  }

  let mut collector = VarCollector::for_func_body();
  collector.visit_program(&parser_ret.program);

  Ok(collector.vars.into_iter().map(String::from).collect())
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
      }

      unsafe {
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
      }
      unsafe {
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
