declare module 'ant:ffi' {
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

  type FFIType = FFITypeConstants[keyof FFITypeConstants];
  type FFIValueType = Exclude<FFIType, '...'>;

  interface FunctionSignature {
    returns: FFIValueType;
    args: FFIType[];
  }

  interface FFIPointer {
    address(): number;
    isNull(): boolean;
    offset(bytes: number): FFIPointer;
    read(type?: FFIValueType): unknown;
    write(type: FFIValueType, value: unknown): FFIPointer;
    free(): FFIPointer;
  }

  interface FFICallback {
    address(): number;
    close(): FFICallback;
  }

  interface FFIFunction<Args extends unknown[] = unknown[], Return = unknown> {
    (...args: Args): Return;
    address(): number;
  }

  interface FFILibrary {
    define(name: string, signature: FunctionSignature | [FFIValueType, FFIType[]]): FFIFunction;
    call(name: string, ...args: unknown[]): unknown;
    close(): FFILibrary;
  }

  const suffix: 'dylib' | 'so' | 'dll';
  const FFIType: FFITypeConstants;

  function alloc(size: number): FFIPointer;
  function pointer(value?: string | FFIPointer | FFICallback | ArrayBuffer | ArrayBufferView | null): FFIPointer;
  function callback(signature: FunctionSignature | [FFIValueType, FFIType[]], fn: (...args: unknown[]) => unknown): FFICallback;
  function callback(fn: (...args: unknown[]) => unknown, signature: FunctionSignature | [FFIValueType, FFIType[]]): FFICallback;
  function dlopen<T extends object = {}>(libraryPath: string): FFILibrary & T;
}
