import { parseJavaScript } from 'ant:syntax';

const tree = parseJavaScript('const answer = 42;');
console.log(JSON.stringify(tree, null, 2));
