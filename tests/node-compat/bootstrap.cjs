'use strict';

const target = process.argv[2];
if (!target) throw new Error('node compatibility bootstrap requires a test path');

if (!process.config) {
  Object.defineProperty(process, 'config', {
    configurable: true,
    enumerable: true,
    value: {
      target_defaults: { default_configuration: 'Release' },
      variables: {
        node_module_version: 0,
        node_quic: 0,
        node_shared: false,
        node_shared_openssl: false,
        node_shared_sqlite: false,
        node_use_openssl: Boolean(process.versions?.openssl),
        openssl_is_fips: false,
        v8_enable_i18n_support: typeof Intl === 'object'
      }
    }
  });
}

process.argv.splice(1, 1);
require(target);
