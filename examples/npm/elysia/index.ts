import { Elysia } from 'elysia';
import { AntAdapter } from './modules/adapter';
import { logger } from './modules/logger';

new Elysia({ adapter: AntAdapter })
  .use(logger())
  .get('/', () => `hello elysia!!\n\n🐜 ${Ant.version}\n`)
  .listen(3000);

console.log('started on http://localhost:3000');
