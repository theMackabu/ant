declare module 'path' {
  interface ParsedPath {
    root: string;
    dir: string;
    base: string;
    ext: string;
    name: string;
  }

  const sep: string;
  const delimiter: string;

  function basename(path: string, ext?: string): string;
  function dirname(path: string): string;
  function extname(path: string): string;
  function join(...paths: string[]): string;
  function normalize(path: string): string;
  function resolve(...paths: string[]): string;
  function relative(from: string, to: string): string;
  function isAbsolute(path: string): boolean;
  function parse(path: string): ParsedPath;
  function format(pathObject: Partial<ParsedPath>): string;
}
