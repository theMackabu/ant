'use strict';

var EventEmitter = require('node:events').EventEmitter;
var BufferCtor = typeof Buffer !== 'undefined' ? Buffer : require('node:buffer').Buffer;

function noop() {}

function nextTick(fn) {
  if (typeof process !== 'undefined' && process && typeof process.nextTick === 'function') {
    var args = Array.prototype.slice.call(arguments, 1);
    process.nextTick(function () {
      fn.apply(undefined, args);
    });
    return;
  }

  Promise.resolve().then(function () {
    fn();
  });
}

function once(fn) {
  var called = false;
  return function () {
    if (called) return;
    called = true;
    return fn.apply(this, arguments);
  };
}

function inherits(target, source) {
  Object.getOwnPropertyNames(source).forEach(function (name) {
    if (name === 'constructor') return;
    if (Object.prototype.hasOwnProperty.call(target, name)) return;
    Object.defineProperty(target, name, Object.getOwnPropertyDescriptor(source, name));
  });
}

function normalizeChunk(chunk, objectMode, encoding) {
  if (objectMode || chunk == null || BufferCtor.isBuffer(chunk) || chunk instanceof Uint8Array) return chunk;
  if (typeof chunk === 'string') return BufferCtor.from(chunk, encoding || 'utf8');
  return BufferCtor.from(String(chunk), encoding || 'utf8');
}

function toAsyncIterable(source) {
  if (source && typeof source[Symbol.asyncIterator] === 'function') return source;
  if (source && typeof source[Symbol.iterator] === 'function') {
    return {
      async *[Symbol.asyncIterator]() {
        for (var value of source) yield value;
      }
    };
  }

  if (source && typeof source.getReader === 'function') {
    return {
      async *[Symbol.asyncIterator]() {
        var reader = source.getReader();
        try {
          while (true) {
            var result = await reader.read();
            if (!result || result.done) break;
            yield result.value;
          }
        } finally {
          if (reader.releaseLock) reader.releaseLock();
        }
      }
    };
  }

  return {
    async *[Symbol.asyncIterator]() {
      if (source !== undefined) yield source;
    }
  };
}

function Stream(options) {
  if (!(this instanceof Stream)) return new Stream(options);
  this.readable = true;
  this.writable = true;
  this.destroyed = false;
  this._paused = false;
  this._pipes = [];
  this._streamOptions = options || {};
}

Stream.prototype = Object.create(EventEmitter.prototype);
Stream.prototype.constructor = Stream;

Stream.prototype.pipe = function (dest, options) {
  var source = this;
  var end = !options || options.end !== false;

  function onData(chunk) {
    if (!dest || typeof dest.write !== 'function') return;
    var ok = dest.write(chunk);
    if (ok === false && typeof source.pause === 'function') source.pause();
  }

  function onDrain() {
    if (typeof source.resume === 'function') source.resume();
  }

  function onEnd() {
    cleanup();
    if (end && dest && typeof dest.end === 'function') dest.end();
  }

  function onClose() {
    cleanup();
  }

  function onError(err) {
    cleanup();
    if (dest && typeof dest.emit === 'function' && dest.listenerCount('error') > 0) dest.emit('error', err);
  }

  function cleanup() {
    source.removeListener('data', onData);
    source.removeListener('end', onEnd);
    source.removeListener('close', onClose);
    source.removeListener('error', onError);
    if (dest && typeof dest.removeListener === 'function') dest.removeListener('drain', onDrain);
    source._pipes = source._pipes.filter(function (entry) {
      return entry.dest !== dest;
    });
  }

  this._pipes.push({ dest: dest, cleanup: cleanup });
  this.on('data', onData);
  this.once('end', onEnd);
  this.once('close', onClose);
  this.on('error', onError);
  if (dest && typeof dest.on === 'function') dest.on('drain', onDrain);
  if (dest && typeof dest.emit === 'function') dest.emit('pipe', this);
  if (typeof this.resume === 'function') this.resume();
  return dest;
};

Stream.prototype.unpipe = function (dest) {
  var pipes = this._pipes.slice();
  for (var i = 0; i < pipes.length; i++) {
    if (!dest || pipes[i].dest === dest) pipes[i].cleanup();
  }
  return this;
};

Stream.prototype.pause = function () {
  this._paused = true;
  this.emit('pause');
  return this;
};

Stream.prototype.resume = function () {
  this._paused = false;
  this.emit('resume');
  return this;
};

Stream.prototype.isPaused = function () {
  return !!this._paused;
};

Stream.prototype.destroy = function (err) {
  if (this.destroyed) return this;

  var self = this;
  this.destroyed = true;

  function done(destroyErr) {
    if (destroyErr) self.emit('error', destroyErr);
    self.emit('close');
  }

  if (typeof this._destroy === 'function') {
    this._destroy(err || null, once(done));
  } else done(err || null);

  return this;
};

function Readable(options) {
  Stream.call(this, options);
  options = options || {};
  this.readable = true;
  this.writable = false;
  this.readableEnded = false;
  this._readableState = {
    objectMode: !!options.objectMode,
    ended: false,
    endEmitted: false,
    flowing: false,
    reading: false,
    highWaterMark: options.highWaterMark || 16384,
    buffer: []
  };

  if (typeof options.read === 'function') this._read = options.read;
}

Readable.prototype = Object.create(Stream.prototype);
Readable.prototype.constructor = Readable;

Readable.prototype._read = noop;

Readable.prototype._flushReadable = function () {
  while (this._readableState.flowing && this._readableState.buffer.length > 0) {
    var chunk = this._readableState.buffer.shift();
    this.emit('data', chunk);
  }

  if (this._readableState.ended && this._readableState.buffer.length === 0 && !this._readableState.endEmitted) {
    this._readableState.endEmitted = true;
    this.readableEnded = true;
    this.emit('end');
    this.emit('close');
  }
};

Readable.prototype._maybeRead = function () {
  if (this.destroyed || this._readableState.reading || this._readableState.ended) return;
  if (this._readableState.buffer.length > 0) return;

  this._readableState.reading = true;
  try {
    this._read(this._readableState.highWaterMark);
  } finally {
    this._readableState.reading = false;
  }
};

Readable.prototype.push = function (chunk, encoding) {
  if (this.destroyed) return false;

  if (chunk === null) {
    this._readableState.ended = true;
    this._flushReadable();
    return false;
  }

  this._readableState.buffer.push(normalizeChunk(chunk, this._readableState.objectMode, encoding));
  if (this._readableState.flowing) this._flushReadable();
  return this._readableState.flowing;
};

Readable.prototype.read = function () {
  if (this._readableState.buffer.length === 0) this._maybeRead();
  if (this._readableState.buffer.length === 0) return null;

  var chunk = this._readableState.buffer.shift();
  if (this._readableState.flowing) this._flushReadable();
  return chunk;
};

Readable.prototype.on = function (event, listener) {
  var result = Stream.prototype.on.call(this, event, listener);
  if (event === 'data') this.resume();
  return result;
};

Readable.prototype.resume = function () {
  this._readableState.flowing = true;
  Stream.prototype.resume.call(this);
  this._maybeRead();
  this._flushReadable();
  return this;
};

Readable.prototype.pause = function () {
  this._readableState.flowing = false;
  return Stream.prototype.pause.call(this);
};

Readable.from = function (source, options) {
  var readable = new Readable(options);

  nextTick(function () {
    (async function () {
      try {
        for await (var chunk of toAsyncIterable(source)) {
          if (readable.destroyed) return;
          readable.push(chunk);
        }
        readable.push(null);
      } catch (err) {
        readable.destroy(err);
      }
    })();
  });

  return readable;
};

Readable.fromWeb = function (source, options) {
  return Readable.from(source, options);
};

function Writable(options) {
  Stream.call(this, options);
  options = options || {};
  this.readable = false;
  this.writable = true;
  this.writableEnded = false;
  this.writableFinished = false;
  this._writableState = {
    objectMode: !!options.objectMode || !!options.writableObjectMode,
    finished: false,
    ended: false
  };

  if (typeof options.write === 'function') this._write = options.write;
  if (typeof options.final === 'function') this._final = options.final;
  if (typeof options.destroy === 'function') this._destroy = options.destroy;
}

Writable.prototype = Object.create(Stream.prototype);
Writable.prototype.constructor = Writable;

Writable.prototype._write = function (_chunk, _encoding, callback) {
  callback();
};

Writable.prototype._final = function (callback) {
  callback();
};

Writable.prototype.write = function (chunk, encoding, callback) {
  if (typeof encoding === 'function') {
    callback = encoding;
    encoding = undefined;
  }

  if (this.writableEnded || this.destroyed) {
    var writeErr = new Error('write after end');
    if (typeof callback === 'function') callback(writeErr);
    else this.emit('error', writeErr);
    return false;
  }

  var self = this;
  var done = once(function (err) {
    if (err) {
      self.destroy(err);
      if (typeof callback === 'function') callback(err);
      return;
    }
    if (typeof callback === 'function') callback();
    self.emit('drain');
  });

  try {
    this._write(normalizeChunk(chunk, this._writableState.objectMode, encoding), encoding || 'utf8', done);
  } catch (err) {
    done(err);
    return false;
  }

  return !this.destroyed;
};

Writable.prototype.end = function (chunk, encoding, callback) {
  if (typeof chunk === 'function') {
    callback = chunk;
    chunk = undefined;
    encoding = undefined;
  } else if (typeof encoding === 'function') {
    callback = encoding;
    encoding = undefined;
  }

  if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
  if (this.writableEnded) {
    if (typeof callback === 'function') callback();
    return this;
  }

  var self = this;
  this.writableEnded = true;
  this._writableState.ended = true;

  this._final(
    once(function (err) {
      if (err) {
        self.destroy(err);
        if (typeof callback === 'function') callback(err);
        return;
      }

      self.writableFinished = true;
      self._writableState.finished = true;
      self.emit('finish');
      if (typeof callback === 'function') callback();
      if (!self.readable) self.emit('close');
    })
  );

  return this;
};

Writable.prototype.cork = noop;
Writable.prototype.uncork = noop;

function Duplex(options) {
  Readable.call(this, options);
  Writable.call(this, options);
  options = options || {};
  this.readable = true;
  this.writable = true;
  this.allowHalfOpen = options.allowHalfOpen !== false;
}

Duplex.prototype = Object.create(Readable.prototype);
Duplex.prototype.constructor = Duplex;
inherits(Duplex.prototype, Writable.prototype);

function Transform(options) {
  Duplex.call(this, options);
  options = options || {};
  if (typeof options.transform === 'function') this._transform = options.transform;
  if (typeof options.flush === 'function') this._flush = options.flush;
}

Transform.prototype = Object.create(Duplex.prototype);
Transform.prototype.constructor = Transform;

Transform.prototype._transform = function (chunk, _encoding, callback) {
  callback(null, chunk);
};

Transform.prototype._write = function (chunk, encoding, callback) {
  var self = this;
  this._transform(chunk, encoding, function (err, data) {
    if (err) {
      callback(err);
      return;
    }
    if (data !== undefined && data !== null) self.push(data);
    callback();
  });
};

Transform.prototype._final = function (callback) {
  var self = this;
  if (typeof this._flush === 'function') {
    this._flush(function (err, data) {
      if (err) {
        callback(err);
        return;
      }
      if (data !== undefined && data !== null) self.push(data);
      self.push(null);
      callback();
    });
    return;
  }

  this.push(null);
  callback();
};

function PassThrough(options) {
  Transform.call(this, options);
}

PassThrough.prototype = Object.create(Transform.prototype);
PassThrough.prototype.constructor = PassThrough;

PassThrough.prototype._transform = function (chunk, _encoding, callback) {
  callback(null, chunk);
};

function finished(stream, callback) {
  callback = once(typeof callback === 'function' ? callback : noop);

  function onFinish() {
    cleanup();
    callback();
  }

  function onError(err) {
    cleanup();
    callback(err);
  }

  function cleanup() {
    stream.removeListener('end', onFinish);
    stream.removeListener('finish', onFinish);
    stream.removeListener('close', onFinish);
    stream.removeListener('error', onError);
  }

  stream.on('end', onFinish);
  stream.on('finish', onFinish);
  stream.on('close', onFinish);
  stream.on('error', onError);
  return stream;
}

function pipeline() {
  var args = Array.prototype.slice.call(arguments);
  var callback = typeof args[args.length - 1] === 'function' ? args.pop() : noop;

  if (args.length < 2) {
    nextTick(callback);
    return args[0];
  }

  var done = once(callback);
  for (var i = 0; i < args.length - 1; i++) {
    finished(args[i], function (err) {
      if (err) done(err);
    });
    args[i].pipe(args[i + 1]);
  }

  finished(args[args.length - 1], done);
  return args[args.length - 1];
}

var promises = {
  pipeline: function () {
    var args = Array.prototype.slice.call(arguments);
    return new Promise(function (resolve, reject) {
      args.push(function (err) {
        if (err) reject(err);
        else resolve();
      });
      pipeline.apply(undefined, args);
    });
  },
  finished: function (stream) {
    return new Promise(function (resolve, reject) {
      finished(stream, function (err) {
        if (err) reject(err);
        else resolve();
      });
    });
  }
};

Stream.Stream = Stream;
Stream.Readable = Readable;
Stream.Writable = Writable;
Stream.Duplex = Duplex;
Stream.Transform = Transform;
Stream.PassThrough = PassThrough;
Stream.pipeline = pipeline;
Stream.finished = finished;
Stream.promises = promises;

module.exports = Stream;
module.exports.default = Stream;
module.exports.Stream = Stream;
module.exports.Readable = Readable;
module.exports.Writable = Writable;
module.exports.Duplex = Duplex;
module.exports.Transform = Transform;
module.exports.PassThrough = PassThrough;
module.exports.pipeline = pipeline;
module.exports.finished = finished;
module.exports.promises = promises;
