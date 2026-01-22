use std::path::Path;

use oxc_allocator::Allocator;
use oxc_codegen::Codegen;
use oxc_parser::Parser;
use oxc_semantic::SemanticBuilder;
use oxc_span::SourceType;
use oxc_transformer::{TransformOptions, Transformer, TypeScriptOptions};

pub fn strip_types_internal(source: &str, filename: &str) -> Result<String, String> {
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
