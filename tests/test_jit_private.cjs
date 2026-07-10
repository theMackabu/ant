function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function assertThrowsTypeError(fn, message) {
  try {
    fn();
  } catch (error) {
    if (error instanceof TypeError) return;
    throw new Error(`${message}: expected TypeError, got ${error}`);
  }
  throw new Error(`${message}: expected TypeError`);
}

class PrivateBox {
  #value = 0;

  #twice(value) {
    return value * 2;
  }

  read() {
    return this.#value;
  }

  write(value) {
    return (this.#value = value);
  }

  add(value) {
    return (this.#value += value);
  }

  postIncrement() {
    return this.#value++;
  }

  callPrivate(value) {
    return this.#twice(value) + this.#value;
  }

  readOther(other) {
    return other.#value;
  }

  writeOther(other, value) {
    return (other.#value = value);
  }

  catchRead(other) {
    try {
      return other.#value;
    } catch (error) {
      return error.name;
    }
  }

  catchWrite(other, value) {
    try {
      return (other.#value = value);
    } catch (error) {
      return error.name;
    }
  }

  catchMethodWrite(value) {
    try {
      return (this.#twice = value);
    } catch (error) {
      return error.name;
    }
  }
}

class PrivateAccessorBox {
  #backing = 0;

  get #value() {
    return this.#backing;
  }

  set #value(value) {
    this.#backing = value;
  }

  read() {
    return this.#value;
  }

  write(value) {
    return (this.#value = value);
  }
}

class ReadOnlyPrivateAccessor {
  #backing = 7;

  get #value() {
    return this.#backing;
  }

  catchWrite(value) {
    try {
      return (this.#value = value);
    } catch (error) {
      return error.name;
    }
  }
}

class ThrowingPrivateAccessor {
  get #value() {
    throw new TypeError("private getter failed");
  }

  set #value(value) {
    throw new TypeError("private setter failed: " + value);
  }

  catchRead() {
    try {
      return this.#value;
    } catch (error) {
      return error.message;
    }
  }

  catchWrite(value) {
    try {
      return (this.#value = value);
    } catch (error) {
      return error.message;
    }
  }
}

class StaticPrivateBox {
  static #value = 0;

  static read() {
    return this.#value;
  }

  static write(value) {
    return (this.#value = value);
  }
}

const box = new PrivateBox();
const accessor = new PrivateAccessorBox();
const readOnly = new ReadOnlyPrivateAccessor();
const throwing = new ThrowingPrivateAccessor();

for (let i = 0; i < 500; i++) {
  assertEq(box.write(i), i, "warm field write value");
  assertEq(box.read(), i, "warm field read");
  assertEq(box.add(1), i + 1, "warm compound field write");
  assertEq(box.postIncrement(), i + 1, "warm post-increment value");
  assertEq(box.callPrivate(i), i * 2 + i + 2, "warm private method read");
  assertEq(box.readOther(box), i + 2, "warm other private read");
  assertEq(box.writeOther(box, i), i, "warm other private write");
  assertEq(box.catchRead(box), i, "warm caught read success");
  assertEq(box.catchWrite(box, i), i, "warm caught write success");
  assertEq(box.catchMethodWrite(i), "TypeError", "warm private method write error");

  assertEq(accessor.write(i), i, "warm accessor write value");
  assertEq(accessor.read(), i, "warm accessor read");
  assertEq(readOnly.catchWrite(i), "TypeError", "warm missing setter error");
  assertEq(throwing.catchRead(), "private getter failed", "warm throwing getter");
  assertEq(throwing.catchWrite(i), "private setter failed: " + i, "warm throwing setter");

  assertEq(StaticPrivateBox.write(i), i, "warm static write value");
  assertEq(StaticPrivateBox.read(), i, "warm static read");
}

assertEq(box.write(40), 40, "hot field write value");
assertEq(box.read(), 40, "hot field read");
assertEq(box.add(2), 42, "hot compound field write");
assertEq(box.postIncrement(), 42, "hot post-increment value");
assertEq(box.read(), 43, "hot post-increment stored value");
assertEq(box.callPrivate(2), 47, "hot private method read");
assertEq(box.readOther(box), 43, "hot other private read");
assertEq(box.writeOther(box, 44), 44, "hot other private write value");
assertEq(box.read(), 44, "hot other private write stored value");

assertEq(accessor.write(41), 41, "hot accessor write value");
assertEq(accessor.read(), 41, "hot accessor read");
assertEq(readOnly.catchWrite(1), "TypeError", "hot missing setter error");
assertEq(box.catchMethodWrite(1), "TypeError", "hot private method write error");
assertEq(throwing.catchRead(), "private getter failed", "hot throwing getter");
assertEq(throwing.catchWrite(42), "private setter failed: 42", "hot throwing setter");

assertEq(StaticPrivateBox.write(42), 42, "hot static write value");
assertEq(StaticPrivateBox.read(), 42, "hot static read");

assertThrowsTypeError(() => box.readOther({}), "hot uncaught wrong-brand read");
assertThrowsTypeError(() => box.writeOther({}, 1), "hot uncaught wrong-brand write");
assertThrowsTypeError(() => box.readOther(null), "hot uncaught primitive read");
assertThrowsTypeError(() => box.writeOther(0, 1), "hot uncaught primitive write");
assertEq(box.catchRead({}), "TypeError", "hot caught wrong-brand read");
assertEq(box.catchWrite({}, 1), "TypeError", "hot caught wrong-brand write");

console.log("OK: test_jit_private");
