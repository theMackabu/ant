import { createRouter, addRoute, findRoute } from './';

const router = createRouter();

addRoute(router, 'GET', '/path', { payload: 'this path' });
addRoute(router, 'POST', '/path/:name', { payload: 'named route' });
addRoute(router, 'GET', '/path/foo/**', { payload: 'wildcard route' });
addRoute(router, 'GET', '/path/foo/**:name', { payload: 'named wildcard route' });

console.log(findRoute(router, 'GET', '/path'));
console.log(findRoute(router, 'POST', '/path/fooval'));
console.log(findRoute(router, 'GET', '/path/foo/bar/baz'));
console.log(findRoute(router, 'GET', '/'));
