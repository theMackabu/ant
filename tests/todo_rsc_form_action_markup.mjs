function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function readUntil(response, marker, limit = 32768) {
  const stream = response.body;
  assert(stream, 'expected response body stream');

  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let text = '';

  while (text.length < limit) {
    const chunk = await reader.read();
    if (chunk.done) break;
    text += decoder.decode(chunk.value, { stream: true });
    if (text.includes(marker)) break;
  }

  text += decoder.decode();
  return text;
}

const mod = await import('../todo/rsc/dist/rsc/index.js');
const handler = mod.default;

assert(handler && typeof handler.fetch === 'function', 'expected todo/rsc handler.fetch');

const response = await handler.fetch(new Request('http://localhost/'));
assert(response.status === 200, `expected status 200, got ${response.status}`);

const html = await readUntil(response, '</form>');
const formStart = html.indexOf('<form');
assert(formStart !== -1, 'expected rendered form');

const formEnd = html.indexOf('</form>', formStart);
assert(formEnd !== -1, 'expected form closing tag');

const snippet = html.slice(formStart, formEnd + 7);
console.log(snippet);

assert(
  snippet.includes('$ACTION_') ||
    snippet.includes('javascript:throw new Error') ||
    snippet.includes('formAction'),
  'expected server action metadata or replay marker in form markup',
);
