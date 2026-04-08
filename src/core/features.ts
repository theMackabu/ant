process.features = {
  uv: true,
  tls: 'BoringSSL',
  typescript: 'transform'
};

// polyfill process.versions to satisfy modules that rely on node
// runtime detection. values mirror node 25.2.0 defaults
process.versions = {
  node: '25.2.0',
  ant: import.meta.env.VERSION,
  v8: '14.1.146.11-node.14',
  uv: '1.52.0',
  modules: '141'
};
