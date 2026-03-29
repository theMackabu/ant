console.log('started on http://localhost:3000');

require('node:http')
  .createServer((_req, res) => res.end('ant!'))
  .listen(3000);
