import * as http from 'node:http';
import * as https from 'node:https';

function clientNotImplemented() {
  throw new Error('node:http2 client transport is not implemented yet');
}

// compatibility stub only
export class Http2Server extends http.Server {}
export class Http2SecureServer extends https.Server {}

export class Http2ServerRequest {
  constructor() {
    this.stream = undefined;
    this.authority = undefined;
  }
}

export class Http2ServerResponse {}

export class ClientHttp2Session {}
export class ClientHttp2Stream {}
export class ServerHttp2Session {}
export class ServerHttp2Stream {}

export function createServer(options, requestListener) {
  return http.createServer(options, requestListener);
}

export function createSecureServer(options, requestListener) {
  return https.createServer(options, requestListener);
}

export function connect() {
  clientNotImplemented();
}

export function getDefaultSettings() {
  return {};
}

export function getPackedSettings() {
  return new Uint8Array(0);
}

export function getUnpackedSettings() {
  return {};
}

export const constants = Object.freeze({
  NGHTTP2_NO_ERROR: 0,
  HTTP_STATUS_OK: 200,
  HTTP_STATUS_NO_CONTENT: 204,
  HTTP_STATUS_NOT_MODIFIED: 304
});

export const sensitiveHeaders = Object.freeze([]);

export default {
  ClientHttp2Session,
  ClientHttp2Stream,
  Http2Server,
  Http2ServerRequest,
  Http2ServerResponse,
  Http2SecureServer,
  ServerHttp2Session,
  ServerHttp2Stream,
  connect,
  constants,
  createServer,
  createSecureServer,
  getDefaultSettings,
  getPackedSettings,
  getUnpackedSettings,
  sensitiveHeaders
};
