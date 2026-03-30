import { createServer } from 'node:http';

const server = createServer(async (req, res) => {
  const chunks = [];

  for await (const chunk of req) chunks.push(chunk);

  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end(Buffer.concat(chunks).toString('utf8'));
});

server.listen(0, async () => {
  const address = server.address();
  const response = await fetch(`http://127.0.0.1:${address.port}/`, {
    method: 'POST',
    body: 'hello-body',
  });

  console.log(`status:${response.status}`);
  console.log(`text:${await response.text()}`);
  server.close();
});
