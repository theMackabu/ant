declare module 'ant:syntax' {
  type StripTypesSourceType = 'auto' | 'module' | 'script' | 'commonjs';
  type JavaScriptSourceType = 'unambiguous' | 'module' | 'script';

  interface StripTypesOptions {
    filename?: string;
    sourceType?: StripTypesSourceType;
  }

  interface ParseJavaScriptOptions {
    filename?: string;
    sourceType?: JavaScriptSourceType;
    locations?: boolean;
  }

  interface SourcePosition {
    line: number;
    column: number;
  }

  interface SourceLocation {
    start: SourcePosition;
    end: SourcePosition;
  }

  interface SyntaxNode {
    type: string;
    start?: number;
    end?: number;
    loc?: SourceLocation;
    [field: string]: unknown;
  }

  interface Program extends SyntaxNode {
    schema: 'ant.syntax@1';
    type: 'Program';
    sourceType: 'module' | 'script';
    start: 0;
    end: number;
    body: SyntaxNode[];
  }

  function stripTypes(source: string, options?: StripTypesOptions): string;
  function parseJavaScript(source: string, options?: ParseJavaScriptOptions): Program;
}
