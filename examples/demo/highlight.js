const hl = Ant.highlight;
const render = hl.render;

function assertEq(actual, expected, label) {
  if (actual !== expected) throw new Error(`${label}: expected ${JSON.stringify(expected)} got ${JSON.stringify(actual)}`);
}

console.log(render('<red>Red text</red> and back to normal'));
console.log(render('<green>Green</green>, <blue>Blue</blue>, <yellow>Yellow</yellow>'));

console.log(render('<bright_red>Bright Red</bright_red> vs <red>Normal Red</red>'));

console.log(render('<bg_red>Red background</bg_red> with text'));
console.log(render('<bg_blue><white>Blue bg with white text</white></bg_blue>'));

console.log(render('<bold>Bold text</bold> and <dim>dim text</dim>'));
console.log(render('<ul>Underlined</ul> text'));
console.log(render('<i>Italic</i> or <italic>italic</italic>, <strike>strikethrough</strike>, <invert>inverted</invert>'));

console.log(render('<bold_red>Bold red</bold_red> or <bold+blue>bold blue</bold+blue>'));
console.log(render('<dim_cyan>Dim cyan</dim_cyan> and <i+bright_green>italic green</i+bright_green>'));

console.log(render('<#ff8800>Orange hex color</#ff8800>'));
console.log(render('<#0f0>Bright green shorthand</#0f0>'));
console.log(render('<bg_#ff00ff>Magenta background</bg_#ff00ff>'));

console.log(render('<bold>Number: 42, String: hello</bold>'));
console.log(render('<green>Hex: 0xff, Float: 3.14</green>'));

console.log(render('<red>red <reset/>back to normal immediately'));

console.log(render('Literal <<: less than, >>: greater than'));
console.log(render('Literal %%: percent sign'));

const code = `\nconst x = 42;
function greet(name) {
  console.log(\`Hello, \${name}!\`);
}`;

console.log(hl(code));

const lineComment = hl.tags('//hello\nworld');
assertEq(lineComment, '<#758CA3>//hello</>\nworld', 'line comment stops at newline');

const blockComment = hl.tags('/*hello\nworld');
assertEq(blockComment, '<#758CA3>/*hello\nworld</>', 'block comment continues across newline');

console.log('highlight comment specs ok');
