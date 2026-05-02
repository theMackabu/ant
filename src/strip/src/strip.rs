use std::path::Path;

use oxc_allocator::Allocator;
use oxc_ast::ast::{
  BindingPattern, Declaration, ExportDefaultDeclarationKind, FormalParameter, Function, Program,
  Statement, TSType, TSTypeAnnotation,
};
use oxc_codegen::Codegen;
use oxc_parser::Parser;
use oxc_semantic::SemanticBuilder;
use oxc_span::SourceType;
use oxc_transformer::{TransformOptions, Transformer, TypeScriptOptions};

pub struct StripTypesResult {
  pub code: String,
  pub hints: String,
}

fn type_hint_from_type(ts_type: &TSType<'_>) -> char {
  match ts_type {
    TSType::TSNumberKeyword(_) => 'N',
    TSType::TSStringKeyword(_) => 'S',
    TSType::TSBooleanKeyword(_) => 'B',
    TSType::TSArrayType(_) | TSType::TSTupleType(_) => 'A',
    TSType::TSObjectKeyword(_) | TSType::TSTypeLiteral(_) => 'O',
    TSType::TSUndefinedKeyword(_) | TSType::TSVoidKeyword(_) => 'V',
    TSType::TSNullKeyword(_) => '0',
    TSType::TSParenthesizedType(parenthesized) => type_hint_from_type(&parenthesized.type_annotation),
    _ => 'U',
  }
}

fn type_hint_from_annotation(annotation: Option<&TSTypeAnnotation<'_>>) -> char {
  annotation.map_or('U', |annotation| type_hint_from_type(&annotation.type_annotation))
}

fn binding_name<'a>(pattern: &'a BindingPattern<'a>) -> Option<&'a str> {
  match pattern {
    BindingPattern::BindingIdentifier(ident) => Some(ident.name.as_str()),
    BindingPattern::AssignmentPattern(assign) => binding_name(&assign.left),
    _ => None,
  }
}

fn param_hint(param: &FormalParameter<'_>) -> char {
  if binding_name(&param.pattern).is_none() {
    return 'U';
  }
  type_hint_from_annotation(param.type_annotation.as_deref())
}

fn collect_function_hint(func: &Function<'_>, out: &mut String) {
  let Some(id) = &func.id else {
    return;
  };
  if func.body.is_none() {
    return;
  }

  let mut params = String::new();
  let mut has_hint = false;
  for param in &func.params.items {
    let hint = param_hint(param);
    if hint != 'U' {
      has_hint = true;
    }
    params.push(hint);
  }

  if let Some(rest) = &func.params.rest {
    let hint = type_hint_from_annotation(rest.type_annotation.as_deref());
    if hint != 'U' {
      has_hint = true;
    }
    params.push(hint);
  }

  let ret = type_hint_from_annotation(func.return_type.as_deref());
  if ret != 'U' {
    has_hint = true;
  }
  if !has_hint {
    return;
  }

  out.push_str("fn:");
  out.push_str(id.name.as_str());
  out.push_str("|p:");
  out.push_str(&params);
  out.push_str("|r:");
  out.push(ret);
  out.push('\n');
}

fn collect_statement_hints(stmt: &Statement<'_>, out: &mut String) {
  match stmt {
    Statement::FunctionDeclaration(func) => collect_function_hint(func, out),
    Statement::ExportNamedDeclaration(export) => {
      if let Some(Declaration::FunctionDeclaration(func)) = &export.declaration {
        collect_function_hint(func, out);
      }
    }
    Statement::ExportDefaultDeclaration(export) => {
      if let ExportDefaultDeclarationKind::FunctionDeclaration(func) = &export.declaration {
        collect_function_hint(func, out);
      }
    }
    _ => {}
  }
}

fn collect_type_hints(program: &Program<'_>) -> String {
  let mut hints = String::new();
  for stmt in &program.body {
    collect_statement_hints(stmt, &mut hints);
  }
  hints
}

pub fn strip_types_internal(source: &str, filename: &str, is_module: bool) -> Result<String, String> {
  strip_types_with_hints_internal(source, filename, is_module).map(|result| result.code)
}

pub fn strip_types_with_hints_internal(source: &str, filename: &str, is_module: bool) -> Result<StripTypesResult, String> {
  let allocator = Allocator::default();
  let source_type = SourceType::from_path(filename).unwrap_or_else(|_| SourceType::ts()).with_module(is_module);
  let parser_ret = Parser::new(&allocator, source, source_type).parse();

  if !parser_ret.errors.is_empty() {
    let errors: Vec<String> = parser_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Parse errors: {}", errors.join("; ")));
  }

  let mut program = parser_ret.program;
  let hints = collect_type_hints(&program);
  let semantic_ret = SemanticBuilder::new().build(&program);

  if !semantic_ret.errors.is_empty() {
    let errors: Vec<String> = semantic_ret.errors.iter().map(|e| e.to_string()).collect();
    return Err(format!("Semantic errors: {}", errors.join("; ")));
  }

  let scoping = semantic_ret.semantic.into_scoping();
  let transform_options = TransformOptions {
    typescript: TypeScriptOptions {
      only_remove_type_imports: false,
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
  Ok(StripTypesResult { code: output, hints })
}
