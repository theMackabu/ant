const http = require('node:http');

if (http.maxHeaderSize !== 16 * 1024) {
  throw new Error(`expected node:http maxHeaderSize to be 16384, got ${http.maxHeaderSize}`);
}

console.log('node:http:max-header-size:ok');
