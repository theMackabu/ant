use oxc_allocator::Allocator;
use oxc_ast::ast::{BindingPattern, VariableDeclarationKind};
use oxc_ast_visit::{Visit, walk};
use oxc_parser::Parser;
use oxc_semantic::ScopeFlags;
use oxc_span::SourceType;

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

pub fn collect_var_names(source: &str) -> Result<Vec<String>, String> {
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

pub fn collect_var_names_from_func(source: &str) -> Result<Vec<String>, String> {
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
