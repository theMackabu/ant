import type { AddressType, GetConnInfo } from 'hono/conninfo';
import { getAntServer } from './server.js';

const addressType = (address: string): AddressType => {
  if (address.includes(':')) return 'IPv6';
  if (/^\d{1,3}(?:\.\d{1,3}){3}$/.test(address)) return 'IPv4';
  return undefined;
};

/**
 * Returns the remote address and port of the current request.
 * It uses `server.requestIP()`.
 *
 * ```ts
 * app.get('/', c => c.text(getConnInfo(c).remote.address ?? 'unknown'));
 * ```
 */
export const getConnInfo: GetConnInfo = c => {
  const server = getAntServer(c);
  if (typeof server.requestIP !== 'function') {
    throw new TypeError('server.requestIP is not a function.');
  }

  const info = server.requestIP(c.req.raw);
  if (!info) return { remote: {} };

  return {
    remote: {
      address: info.address,
      addressType: addressType(info.address),
      port: info.port,
      transport: 'tcp'
    }
  };
};
