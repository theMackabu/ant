process.features = {
  uv: true,
  tls_mbedtls: import.meta.env.MBEDTLS as unknown as boolean,
  typescript: 'transform'
};
