import { stripTypes } from 'ant:syntax';

const javascript = stripTypes(`
  enum Color { Red = "#FF0000", Blue = "#0000FF" }
  const color: Color = Color.Red;
  console.log(color)
`);

console.log(Ant.highlight(javascript));
new Function(javascript)();
