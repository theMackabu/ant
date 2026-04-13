function afterTransform(er, data) {
  const ts = this._transformState;
  ts.transforming = false;
  const cb = ts.writecb;
  if (cb === null) throw new Error('multiple callback');
  ts.writechunk = null;
  ts.writecb = null;
  cb(er);
  const rs = this._readableState;
  rs.reading = false;
  if (rs.needReadable || rs.length < rs.highWaterMark) {
    this._read(rs.highWaterMark);
  }
}

function doWrite(stream, state, len, chunk, encoding, cb) {
  state.writelen = len;
  state.writecb = cb;
  state.writing = true;
  stream._write(chunk, encoding, state.onwrite);
}

function onwriteStateUpdate(state) {
  state.writing = false;
  state.writecb = null;
  state.length -= state.writelen;
  state.writelen = 0;
}

function afterWrite(stream, state, cb) {
  state.pendingcb--;
  cb();
}

function clearBuffer(stream, state) {
  state.bufferProcessing = true;
  let entry = state.bufferedRequest;
  while (entry) {
    const chunk = entry.chunk;
    const encoding = entry.encoding;
    const cb = entry.callback;
    const len = chunk.length;
    doWrite(stream, state, len, chunk, encoding, cb);
    entry = entry.next;
    state.bufferedRequestCount--;
    if (state.writing) break;
  }
  if (entry === null) state.lastBufferedRequest = null;
  state.bufferedRequest = entry;
  state.bufferProcessing = false;
}

function onwrite(stream, er) {
  const state = stream._writableState;
  const cb = state.writecb;
  if (typeof cb !== 'function') throw new Error('bad cb');
  onwriteStateUpdate(state);
  if (er) throw er;
  if (!state.corked && !state.bufferProcessing && state.bufferedRequest) {
    clearBuffer(stream, state);
  }
  afterWrite(stream, state, cb);
}

function onwriteForward(er) {
  return onwrite(onwriteForward.owner, er);
}

function afterTransformForward(er, data) {
  return afterTransform.call(afterTransformForward.owner, er, data);
}

function writeOrBuffer(stream, state, chunk, encoding, cb) {
  const len = chunk.length;
  state.length += len;
  if (state.writing || state.corked) {
    const req = { chunk, encoding, callback: cb, next: null };
    const last = state.lastBufferedRequest;
    state.lastBufferedRequest = req;
    if (last) last.next = req;
    else state.bufferedRequest = req;
    state.bufferedRequestCount++;
  } else {
    doWrite(stream, state, len, chunk, encoding, cb);
  }
  return true;
}

function makeState(owner, onwriteFn, afterTransformFn) {
  owner._readableState = {
    needReadable: true,
    reading: false,
    length: 0,
    highWaterMark: 16,
  };
  owner._transformState = {
    afterTransform: afterTransformFn,
    needTransform: false,
    transforming: false,
    writecb: null,
    writechunk: null,
    writeencoding: null,
  };
  owner._writableState = {
    length: 0,
    highWaterMark: 16,
    writing: false,
    corked: 0,
    bufferProcessing: false,
    writecb: null,
    writelen: 0,
    bufferedRequest: null,
    lastBufferedRequest: null,
    bufferedRequestCount: 0,
    pendingcb: 0,
    onwrite: onwriteFn,
  };
}

function ClosureForwardingTransformLike() {
  makeState(
    this,
    (er) => onwrite(this, er),
    (er, data) => afterTransform.call(this, er, data),
  );
}

function OwnerForwardingTransformLike() {
  makeState(this, onwriteForward, afterTransformForward);
  this._writableState.onwrite.owner = this;
  this._transformState.afterTransform.owner = this;
}

ClosureForwardingTransformLike.prototype.write = OwnerForwardingTransformLike.prototype.write =
function(chunk, cb) {
  const state = this._writableState;
  state.pendingcb++;
  return writeOrBuffer(this, state, chunk, 'utf8', cb);
};

ClosureForwardingTransformLike.prototype._write = OwnerForwardingTransformLike.prototype._write =
function(chunk, encoding, cb) {
  const ts = this._transformState;
  ts.writecb = cb;
  ts.writechunk = chunk;
  ts.writeencoding = encoding;
  if (!ts.transforming) {
    const rs = this._readableState;
    if (ts.needTransform || rs.needReadable || rs.length < rs.highWaterMark) {
      this._read(rs.highWaterMark);
    }
  }
};

ClosureForwardingTransformLike.prototype._read = OwnerForwardingTransformLike.prototype._read =
function() {
  const ts = this._transformState;
  if (ts.writechunk !== null && !ts.transforming) {
    ts.transforming = true;
    this._transform(ts.writechunk, ts.writeencoding, ts.afterTransform);
  } else {
    ts.needTransform = true;
  }
};

ClosureForwardingTransformLike.prototype._transform = OwnerForwardingTransformLike.prototype._transform =
function(chunk, encoding, cb) {
  cb(null, chunk);
};

function exercise(instance, count) {
  let callbacks = 0;

  for (let i = 0; i < count; i++) {
    instance.write('x', (err) => {
      if (err) throw err;
      callbacks++;
    });
  }

  if (callbacks !== count) {
    throw new Error(`expected ${count} callbacks, got ${callbacks}`);
  }
}

exercise(new OwnerForwardingTransformLike(), 512);
exercise(new ClosureForwardingTransformLike(), 128);

console.log('closure forwarder reentrant callbacks ok');
