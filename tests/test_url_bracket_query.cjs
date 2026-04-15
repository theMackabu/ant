function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const input = 'https://e621.net/pools.json?search[id]=14032,20025,26727';

const url = new URL(input);
assert(url.href === input, `expected href to preserve bracketed query, got ${url.href}`);
assert(url.search === '?search[id]=14032,20025,26727', `unexpected search: ${url.search}`);
assert(
  url.searchParams.get('search[id]') === '14032,20025,26727',
  `unexpected search param value: ${url.searchParams.get('search[id]')}`
);

assert(URL.canParse(input) === true, 'expected URL.canParse to accept bracketed query URL');

const parsed = URL.parse(input);
assert(parsed !== null, 'expected URL.parse to return a URL object');
assert(parsed.href === input, `expected URL.parse href to preserve bracketed query, got ${parsed.href}`);

const request = new Request(input);
assert(request.url === input, `expected Request.url to preserve bracketed query, got ${request.url}`);

console.log('url bracket query test passed');
