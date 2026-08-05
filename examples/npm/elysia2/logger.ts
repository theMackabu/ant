import { Elysia } from 'elysia';

export const logger = ({ methods = ['GET', 'PUT', 'POST', 'DELETE'] } = {}) =>
  new Elysia()
    .request(ctx => {
      if (!methods.includes(ctx.request.method)) return;
      ctx.start = performance.now();
      console.log('<--', ctx.request.method, ctx.path);
    })
    .afterHandle('global', ctx => {
      if (!methods.includes(ctx.request.method)) return;
      console.log('-->', ctx.request.method, ctx.path, ctx.set.status ?? 200, 'in', Number((performance.now() - ctx.start).toFixed(2)), 'ms');
    })
    .error('global', ctx => {
      if (!methods.includes(ctx.request.method)) return;
      console.log('-->', ctx.request.method, ctx.path, ctx.set.status, 'in', ctx.start ? Number((performance.now() - ctx.start).toFixed(2)) : Number.NaN, 'ms');
    });
