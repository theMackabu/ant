import { dlopen, suffix, FFIType } from 'ant:ffi';

let libcName;
if (process.platform === 'darwin') {
  libcName = `libSystem.${suffix}`;
} else if (process.platform === 'linux') {
  libcName = `libc.${suffix}`;
} else if (process.platform === 'win32') {
  libcName = `msvcrt.${suffix}`;
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

console.log(libc);

console.log('calling putchar(65):');
libc.putchar(65); // 'A'

console.log('\ncalling printf:');
libc.printf('Hello FFI! I see %d\n', 42);

console.log('calling putchar(66):');
libc.call('putchar', 66); // 'B'
