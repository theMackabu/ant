import { join } from 'ant:path';
import { dlopen, suffix, FFIType } from 'ant:ffi';

// compile the C library first:
// clang -shared -fPIC -o mathlib.(so/dylib/dll) mathlib.c

const mathlib = dlopen(join(import.meta.dirname, `mathlib.${suffix}`));

mathlib.define('add', {
    args: [FFIType.int, FFIType.int],
    returns: FFIType.int
});
mathlib.define('multiply', {
    args: [FFIType.int, FFIType.int],
    returns: FFIType.int
});
mathlib.define('greet', {
    args: [FFIType.string],
    returns: FFIType.void
});
mathlib.define('divide', {
    args: [FFIType.double, FFIType.double],
    returns: FFIType.double
});

const result1 = mathlib.call('add', 5, 3);
console.log(`add(5, 3) = ${result1}`);

const result2 = mathlib.call('multiply', 4, 7);
console.log(`multiply(4, 7) = ${result2}`);

console.log('Calling greet function:');
mathlib.call('greet', 'World');

const result3 = mathlib.call('divide', 10.0, 2.0);
console.log(`divide(10.0, 2.0) = ${result3}`);

const result4 = mathlib.call('divide', 10.0, 0.0);
console.log(`divide(10.0, 0.0) = ${result4}`);
