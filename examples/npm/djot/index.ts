import djot from '@djot/djot';

const parsed = djot.parse('hi _there_');
const ast = djot.renderAST(parsed);
const html = djot.renderHTML(parsed);

console.log(parsed);
console.log(ast);
console.log(html);
