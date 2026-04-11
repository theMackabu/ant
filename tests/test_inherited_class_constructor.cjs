const assert = require("node:assert");

class BaseBox {
  constructor(left, right) {
    this.left = left;
    this.right = right;
  }

  get pair() {
    return `${this.left}:${this.right}`;
  }
}

class DerivedBox extends BaseBox {}

const derived = new DerivedBox("a", "b");

assert.equal(derived.left, "a");
assert.equal(derived.right, "b");
assert.equal(derived.pair, "a:b");

class URIBase {
  constructor(scheme, authority, path, query, fragment, strict = false, platform) {
    if (typeof scheme === "object") {
      this.scheme = scheme.scheme || "";
      this.authority = scheme.authority || "";
      this.path = scheme.path || "";
      this.query = scheme.query || "";
      this.fragment = scheme.fragment || "";
      this.platform = scheme.platform;
    } else {
      this.scheme = scheme || "";
      this.authority = authority || "";
      this.path = path || "";
      this.query = query || "";
      this.fragment = fragment || "";
      this.platform = platform;
      this.strict = strict;
    }
  }

  get fsPath() {
    if (this.authority && this.path.length > 1 && this.scheme === "file") {
      return `//${this.authority}${this.path}`;
    }
    if (
      this.path.charCodeAt(0) === 47 &&
      ((this.path.charCodeAt(1) >= 65 && this.path.charCodeAt(1) <= 90) ||
        (this.path.charCodeAt(1) >= 97 && this.path.charCodeAt(1) <= 122)) &&
      this.path.charCodeAt(2) === 58
    ) {
      return this.path[1].toLowerCase() + this.path.substring(2);
    }
    return this.path;
  }
}

class URIChild extends URIBase {}

const uri = new URIChild("file", "", "/Users/test/project", "", "", false, "posix");

assert.equal(uri.scheme, "file");
assert.equal(uri.authority, "");
assert.equal(uri.path, "/Users/test/project");
assert.equal(uri.platform, "posix");
assert.equal(uri.fsPath, "/Users/test/project");

console.log("ok");

class FieldBase {
  scheme;
  authority;
  path;
  query;
  fragment;
  platform;

  constructor(scheme, authority, path, query, fragment, strict = false, platform) {
    if (typeof scheme === "object") {
      this.scheme = scheme.scheme || "";
      this.authority = scheme.authority || "";
      this.path = scheme.path || "";
      this.query = scheme.query || "";
      this.fragment = scheme.fragment || "";
      this.platform = scheme.platform;
    } else {
      this.scheme = scheme || "";
      this.authority = authority || "";
      this.path = path || "";
      this.query = query || "";
      this.fragment = fragment || "";
      this.platform = platform;
      this.strict = strict;
    }
  }
}

class FieldChild extends FieldBase {
  _formatted = null;
  _fsPath = null;

  get fsPath() {
    if (!this._fsPath) {
      if (
        this.path.charCodeAt(0) === 47 &&
        ((this.path.charCodeAt(1) >= 65 && this.path.charCodeAt(1) <= 90) ||
          (this.path.charCodeAt(1) >= 97 && this.path.charCodeAt(1) <= 122)) &&
        this.path.charCodeAt(2) === 58
      ) {
        this._fsPath = this.path[1].toLowerCase() + this.path.substring(2);
      } else {
        this._fsPath = this.path;
      }
    }
    return this._fsPath;
  }

  toJSON() {
    return {
      scheme: this.scheme,
      authority: this.authority,
      path: this.path,
      query: this.query,
      fragment: this.fragment,
      platform: this.platform,
      _formatted: this._formatted,
      _fsPath: this._fsPath,
    };
  }
}

const fieldUri = new FieldChild("file", "", "/Users/test/project", "", "", false, "posix");

assert.equal(fieldUri.scheme, "file");
assert.equal(fieldUri.authority, "");
assert.equal(fieldUri.path, "/Users/test/project");
assert.equal(fieldUri.query, "");
assert.equal(fieldUri.fragment, "");
assert.equal(fieldUri.platform, "posix");
assert.equal(fieldUri._formatted, null);
assert.equal(fieldUri._fsPath, null);
assert.equal(fieldUri.fsPath, "/Users/test/project");
assert.equal(fieldUri._fsPath, "/Users/test/project");

console.log("ok-fields");
