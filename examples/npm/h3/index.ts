import { H3, serve } from 'h3';

const app = new H3({
  onRequest: event => {
    console.log('Request:', event.req.url);
  },
  onResponse: (response, event) => {
    console.log('Response:', event.url.pathname, response.status);
  },
  onError: error => {
    console.error(error);
  }
}).get('/', () => 'hello world');

serve(app, { port: 3000 });
