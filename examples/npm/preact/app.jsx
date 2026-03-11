import { render } from 'https://esm.sh/preact-render-to-string';

const App = () => (
  <div id="root">
    <h1>Hello from Preact!</h1>
    <p style="color:red;font-size:16px">Styled text</p>
    <input type="text" disabled />
    <ul>
      <li>No Babel</li>
      <li>No Webpack</li>
      <li>Just Preact</li>
    </ul>
  </div>
);

const html = render(<App />);
console.log(html);
