process.features = {
  uv: true,
  tls: 'BoringSSL',
  typescript: 'transform'
};

// TODO: make most of these dynamic
// polyfill process.versions to satisfy modules that rely on node
// runtime detection. values mirror node 25.9.0 defaults
process.version = 'v25.9.0';

process.versions = {
  ant: import.meta.env.VERSION,
  silver: import.meta.env.VERSION,
  node: '25.9.0',
  brotli: '1.1.0',
  llhttp: '9.3.1',
  nghttp2: '1.68.0',
  simdjson: '0.12.0',
  pcre2: '10.47',
  libffi: '3.5.2',
  lmdb: '0.9.33',
  utf8proc: '2.10.0',
  zlib: '2.3.3',
  v8: '14.1.146.11-node.25',
  uv: '1.52.0',
  modules: '141',
  napi: '10',
  wamr: '92f40918bbfad35546a1512b10bd25eaa31add4d',
  boringssl: '297b11798a0ed6bc7736aa57328909a4afbbf67a'
};
