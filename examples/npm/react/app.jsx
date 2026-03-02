import { renderToHTML } from './client';

const App = () => (
  <div id="root">
    <h1>Hello from React!</h1>
    <p style={{ color: 'red', fontSize: 16 }}>Styled text</p>
    <input type="text" disabled />
    <ul>
      <li>No Babel</li>
      <li>No Webpack</li>
      <li>Just React</li>
    </ul>
  </div>
);

const html = renderToHTML(<App />);
console.log(html);
