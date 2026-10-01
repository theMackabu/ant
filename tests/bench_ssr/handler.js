import { createElement } from 'react';
import { renderToString } from 'react-dom/server.edge';

function App() {
  return createElement('h1', null, 'Hello, world!');
}

// A plain request handler: no web standards and no promises.
export default function handle(request) {
  return { status: 200, headers: { 'content-type': 'text/html' }, body: renderToString(createElement(App)) };
}
