import { Hono } from 'hono';

const app = new Hono();
app.get('/', context => context.text('ant'));

export default app;
