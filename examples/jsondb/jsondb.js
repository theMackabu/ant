import fs from 'fs';

const defaultOptions = {
  asyncWrite: false,
  syncOnWrite: true,
  jsonSpaces: 4,
  stringify: JSON.stringify,
  parse: JSON.parse
};

class JSONdb {
  #filePath;
  #options;
  #storage;

  constructor(filePath, options) {
    if (!filePath || !filePath.length) {
      throw new Error('Missing file path argument.');
    }
    this.#filePath = filePath;
    this.#options = { ...defaultOptions, ...options };
    this.#storage = {};

    let stats;
    try {
      stats = fs.statSync(filePath);
    } catch (err) {
      if (err.code === 'ENOENT') return;
      if (err.code === 'EACCES') throw new Error(`Cannot access path "${filePath}".`);
      throw new Error(`Error while checking for existence of path "${filePath}": ${err}`);
    }

    try {
      fs.accessSync(filePath, fs.constants.R_OK | fs.constants.W_OK);
    } catch (err) {
      throw new Error(`Cannot read & write on path "${filePath}". Check permissions!`);
    }

    if (stats.size > 0) {
      const data = fs.readFileSync(filePath);
      this.#validateJSON(data);
      this.#storage = this.#options.parse(data);
    }
  }

  #validateJSON(fileContent) {
    try {
      this.#options.parse(fileContent);
    } catch (e) {
      console.error('Given filePath is not empty and its content is not valid JSON.');
      throw e;
    }
    return true;
  }

  set(key, value) {
    this.#storage[key] = value;
    if (this.#options.syncOnWrite) this.sync();
  }

  get(key) {
    return Object.hasOwn(this.#storage, key) ? this.#storage[key] : undefined;
  }

  has(key) {
    return Object.hasOwn(this.#storage, key);
  }

  delete(key) {
    const existed = Object.hasOwn(this.#storage, key);
    if (!existed) return undefined;
    delete this.#storage[key];
    if (this.#options.syncOnWrite) this.sync();
    return true;
  }

  deleteAll() {
    for (const key of Object.keys(this.#storage)) {
      this.delete(key);
    }
    return this;
  }

  sync() {
    const data = this.#options.stringify(this.#storage, null, this.#options.jsonSpaces);
    if (this.#options.asyncWrite) {
      fs.writeFile(this.#filePath, data, err => {
        if (err) throw err;
      });
    } else {
      try {
        fs.writeFileSync(this.#filePath, data);
      } catch (err) {
        if (err.code === 'EACCES') throw new Error(`Cannot access path "${this.#filePath}".`);
        throw new Error(`Error while writing to path "${this.#filePath}": ${err}`);
      }
    }
  }

  JSON(storage) {
    if (storage) {
      try {
        JSON.parse(this.#options.stringify(storage));
        this.#storage = storage;
      } catch (err) {
        throw new Error('Given parameter is not a valid JSON object.');
      }
    }
    return JSON.parse(this.#options.stringify(this.#storage));
  }
}

export default JSONdb;
