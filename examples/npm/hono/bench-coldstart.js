import { Hono } from 'hono';

const app = new Hono();

app.get('/', c => c.text('hello hono!'));
app.get('/json', c => c.json({ message: 'hello' }));

// signal that startup is complete, then exit
console.log('ready');
process.exit(0);
