import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('Completion Value Tests\n');

test('empty blocks produce undefined', eval('{}{}{};'), undefined);
test('label inside block completion', eval("{foo: 'bar'}{};"), 'bar');
test('block before labelled completion', eval("{}{foo: 'bar'};"), 'bar');
test('last non-empty block completion', eval("{a: 'b'}{c: 'd'}{};"), 'd');

test('nested label passes expression completion', eval('a: b: c: 1;'), 1);
test('nested label passes comma expression completion', eval('a: b: c: (1, 2, 3);'), 3);

test('statement-position braces parse as block label', eval('{ a: 1 }'), 1);
test('multiple labels in block preserve last completion', eval('{ a: 1; b: 2 }'), 2);
testDeep('parenthesized braces parse as object literal', eval('({ a: 1, b: 2 })'), { a: 1, b: 2 });
testThrows('comma cannot separate labelled statements', () => eval('{ a: 1, b: 2 }'));

test('declaration empty completion preserves previous value', eval("'a'; var x = 5;"), 'a');
test('if true carries body completion', eval("if (true) 'a';"), 'a');
test('if false has empty completion', eval("if (false) 'a';"), undefined);
test('if false overwrites previous expression with undefined', eval("'a'; if (false) 'b';"), undefined);
test('if true empty body overwrites previous expression with undefined', eval("'a'; if (true) ;"), undefined);
test('for loop completion is last body value', eval('for (let i=0;i<3;i++) i;'), 2);
test('while false overwrites previous expression with undefined', eval("'a'; while (false) 'b';"), undefined);
test('for false overwrites previous expression with undefined', eval("'a'; for (;false;) 'b';"), undefined);
test('for empty body overwrites previous expression with undefined', eval("'a'; for (let i=0;i<1;i++);"), undefined);
test('do while empty body overwrites previous expression with undefined', eval("'a'; do ; while(false);"), undefined);
test('with empty body overwrites previous expression with undefined', eval("'a'; with ({}) ;"), undefined);

test('empty finally preserves try completion', eval("try { 'a' } finally { }"), 'a');
test('non-empty finally overrides try completion', eval("try { 'a' } finally { 'b' }"), 'b');
test('empty try finally overwrites previous expression with undefined', eval("'a'; try { } finally { }"), undefined);

test('break label preserves previous non-empty completion', eval("outer: { 'before'; break outer; 'after'; }"), 'before');
test('nested block completion flows outward', eval("outer: { { 'inner'; } ; }"), 'inner');
test('empty block after expression preserves value', eval("'a'; { var x; }"), 'a');

test('switch fallthrough preserves last non-empty case value', eval("switch (1) { case 0: 'zero'; case 1: 'one'; case 2: ; }"), 'one');
test('switch with no matching case is empty', eval("switch (9) { case 0: 'zero'; }"), undefined);
test('switch with no matching case overwrites previous expression with undefined', eval("'a'; switch (9) { case 0: 'zero'; }"), undefined);
test('switch default fallthrough can be overwritten', eval("switch (2) { case 1: 'one'; default: 'default'; case 2: 'two'; }"), 'two');

test('function block body ignores completion value', eval("(function(){ 'block'; })()"), undefined);
test('arrow block body ignores completion value', eval("(()=>{ 'block'; })()"), undefined);
test('arrow expression body returns expression value', eval("(()=> 'expr')()"), 'expr');

summary();
