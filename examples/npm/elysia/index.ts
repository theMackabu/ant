import { Elysia } from 'elysia';
import { logger } from './logger';

console.log('started on http://localhost:3000');

export default new Elysia().use(logger()).get('/', () => 'hello elysia!!\n🐜\n');
