import bench from '../../tests/bench_server.js';

export default {
  port: 34117,
  fetch(req) {
    if (req.url.endsWith('/quit')) process.exit(0);
    return bench.fetch(req);
  }
};
