import { Elysia } from 'elysia';

export const logger = ({ methods = ['GET', 'PUT', 'POST', 'DELETE'] } = {}) =>
  new Elysia()
    .derive({ as: 'global' }, () => ({ start: Date.now() }))
    .onBeforeHandle({ as: 'global' }, ctx => {
      if (!methods.includes(ctx.request.method)) return;
      console.log('<--', ctx.request.method, ctx.path);
    })
    .onAfterHandle({ as: 'global' }, ctx => {
      if (!methods.includes(ctx.request.method)) return;
      console.log('-->', ctx.request.method, ctx.path, ctx.set.status ?? Number.NaN, 'in', Date.now() - ctx.start, 'ms');
    })
    .onError({ as: 'global' }, ctx => {
      if (!methods.includes(ctx.request.method)) return;
      console.log('-->', ctx.request.method, ctx.path, ctx.set.status, 'in', ctx.start ? Date.now() - ctx.start : Number.NaN, 'ms');
    });
