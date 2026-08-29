import { createElement } from 'react';
import { renderToString } from 'react-dom/server.edge';

function render() {
  return renderToString(createElement('div', null, 'hello world'));
}

console.log('started server on http://localhost:3000');
const options = { headers: { 'content-type': 'text/html; charset=utf-8' } };

export default {
  port: 3000,
  fetch() {
    return new Response(render(), options);
  }
};
