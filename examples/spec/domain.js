import { test, testDeep, summary } from './helpers.js';
import domain, { Domain, create } from 'node:domain';
import bareDomain from 'domain';

console.log('node:domain\n');

test('bare and node specifiers share the module', bareDomain, domain);
test('exports Domain', typeof Domain, 'function');
test('exports create', typeof create, 'function');

const outer = create();
const inner = domain.createDomain();
const transitions = [];

outer.run(() => {
  transitions.push(domain.active === outer, process.domain === outer);
  inner.run(() => transitions.push(domain.active === inner, process.domain === inner));
  transitions.push(domain.active === outer, process.domain === outer);
});

transitions.push(domain.active == null, process.domain == null);
testDeep(
  'run restores nested active domains',
  transitions,
  [true, true, true, true, true, true, true, true]
);

const member = {};
outer.add(member);
test('add assigns the member domain', member.domain, outer);
outer.remove(member);
test('remove clears the member domain', member.domain, null);

let intercepted = 0;
let interceptedError = '';
outer.on('error', error => {
  interceptedError = error.message;
});
outer.intercept(value => {
  intercepted = value;
})(null, 42);
outer.intercept(() => {})(new Error('domain boom'));
test('intercept calls the callback without the error argument', intercepted, 42);
test('intercept emits errors', interceptedError, 'domain boom');

summary();
