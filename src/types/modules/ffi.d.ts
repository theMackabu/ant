declare module 'ffi' {
  type FFIType =
    | 'void'
    | 'int8'
    | 'int16'
    | 'int'
    | 'int64'
    | 'uint8'
    | 'uint16'
    | 'uint64'
    | 'float'
    | 'double'
    | 'pointer'
    | 'string'
    | '...';

  interface FFITypeConstants {
    void: 'void';
    int8: 'int8';
    int16: 'int16';
    int: 'int';
    int64: 'int64';
    uint8: 'uint8';
    uint16: 'uint16';
    uint64: 'uint64';
    float: 'float';
    double: 'double';
    pointer: 'pointer';
    string: 'string';
    spread: '...';
  }

  interface FunctionSignature {
    returns: FFIType;
    args: FFIType[];
  }

  interface FFILibrary {
    define(name: string, signature: FunctionSignature | [FFIType, FFIType[]]): (...args: unknown[]) => unknown;
    call(name: string, ...args: unknown[]): unknown;
  }

  const suffix: 'dylib' | 'so' | 'dll';
  const FFIType: FFITypeConstants;

  function dlopen(libraryPath: string): FFILibrary;
  function alloc(size: number): number;
  function free(ptr: number): void;
  function read(ptr: number, size: number, type?: FFIType): unknown;
  function write(ptr: number, value: unknown, type?: FFIType): void;
  function pointer(value: unknown): number;
  function callback(signature: FunctionSignature | [FFIType, FFIType[]], fn: (...args: unknown[]) => unknown): number;
  function freeCallback(ptr: number): void;
  function readPtr(ptr: number): number;
}
