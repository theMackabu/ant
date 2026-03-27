require('node:http')
  .createServer((_req, res) => res.end('ant!'))
  .listen(3000);
