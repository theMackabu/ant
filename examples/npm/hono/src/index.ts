import { Hono } from 'hono';
import { logger } from 'hono/logger';

const app = new Hono();

app.use(logger());
app.get('/', c => c.text('Hello 🐜!'));

console.log('started on http://localhost:3000');

export default app;
