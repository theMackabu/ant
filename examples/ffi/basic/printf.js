import { dlopen, FFIType } from 'ant:ffi';

let libcName;
if (process.platform === 'darwin') {
  libcName = 'libSystem.dylib';
} else if (process.platform === 'linux') {
  libcName = 'libc.so.6';
} else if (process.platform === 'win32') {
  libcName = 'msvcrt.dll';
} else {
  throw new Error(`Unsupported platform: ${process.platform}`);
}

const libc = dlopen(libcName);

libc.define('putchar', {
  args: [FFIType.int],
  returns: FFIType.int
});

libc.define('printf', {
  args: [FFIType.string, FFIType.spread],
  returns: FFIType.int
});

console.log('calling putchar(65):');
libc.call('putchar', 65); // 'A'

console.log('\ncalling printf:');
libc.call('printf', 'Hello FFI! I see %d\n', 42);

console.log('calling putchar(66):');
libc.call('putchar', 66); // 'B'
