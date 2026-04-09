function assertEq(actual, expected, label) {
  if (actual !== expected) throw new Error(`${label}: expected ${JSON.stringify(expected)} got ${JSON.stringify(actual)}`);
}

const lineComment = Ant.highlight.tags('//hello\nworld');
assertEq(lineComment, '<#758CA3>//hello</>\nworld', 'line comment stops at newline');

const blockComment = Ant.highlight.tags('/*hello\nworld');
assertEq(blockComment, '<#758CA3>/*hello\nworld</>', 'block comment continues across newline');

console.log('ok');
