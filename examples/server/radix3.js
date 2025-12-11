class Radix3Node {
  constructor(method) {
    this.method = method;
    this.prefix = '';
    this.handler = undefined;
    this.children = [];
    this.paramChild = undefined;
    this.wildcardChild = undefined;
    this.paramName = undefined;
  }
}

export class Radix3 {
  constructor() {
    this.methods = {};
  }

  longestCommonPrefix(a, b) {
    const minLen = a.length < b.length ? a.length : b.length;
    for (let i = 0; i < minLen; i = i + 1) {
      if (a[i] !== b[i]) return i;
    }
    return minLen;
  }

  insert(path, handler, method = 'GET') {
    if (this.methods[method] === undefined) {
      this.methods[method] = new Radix3Node(method);
    }
    this.insertPath(this.methods[method], path, handler, 0);
  }

  get(path, handler) {
    this.insert(path, handler, 'GET');
  }

  post(path, handler) {
    this.insert(path, handler, 'POST');
  }

  put(path, handler) {
    this.insert(path, handler, 'PUT');
  }

  delete(path, handler) {
    this.insert(path, handler, 'DELETE');
  }

  patch(path, handler) {
    this.insert(path, handler, 'PATCH');
  }

  head(path, handler) {
    this.insert(path, handler, 'HEAD');
  }

  options(path, handler) {
    this.insert(path, handler, 'OPTIONS');
  }

  insertPath(node, path, handler, start) {
    if (start >= path.length) {
      node.handler = handler;
      return;
    }

    const char = path[start];

    if (char === ':') {
      let end = start + 1;
      for (let i = end; i < path.length; i = i + 1) {
        if (path[i] === '/') break;
        end = i + 1;
      }

      const paramName = path.substring(start + 1, end);

      if (node.paramChild === undefined) {
        node.paramChild = new Radix3Node();
        node.paramChild.paramName = paramName;
      }

      this.insertPath(node.paramChild, path, handler, end);
      return;
    }

    if (char === '*') {
      const paramName = path.substring(start + 1, path.length);

      if (node.wildcardChild === undefined) {
        node.wildcardChild = new Radix3Node();
        node.wildcardChild.paramName = paramName;
      }

      node.wildcardChild.handler = handler;
      return;
    }

    let end = start;
    for (let i = start; i < path.length; i = i + 1) {
      if (path[i] === ':' || path[i] === '*') break;
      end = i + 1;
    }

    const segment = path.substring(start, end);

    for (let i = 0; i < node.children.length; i = i + 1) {
      const child = node.children[i];
      const commonLen = this.longestCommonPrefix(child.prefix, segment);

      if (commonLen > 0) {
        if (commonLen < child.prefix.length) {
          const splitNode = new Radix3Node();
          splitNode.prefix = child.prefix.substring(commonLen, child.prefix.length);
          splitNode.handler = child.handler;
          splitNode.children = child.children;
          splitNode.paramChild = child.paramChild;
          splitNode.wildcardChild = child.wildcardChild;

          child.prefix = child.prefix.substring(0, commonLen);
          child.handler = undefined;
          child.children = [splitNode];
          child.paramChild = undefined;
          child.wildcardChild = undefined;
        }

        if (commonLen < segment.length) {
          this.insertPath(child, path, handler, start + commonLen);
        } else {
          this.insertPath(child, path, handler, end);
        }

        return;
      }
    }

    const newChild = new Radix3Node();
    newChild.prefix = segment;
    node.children.push(newChild);
    this.insertPath(newChild, path, handler, end);
  }

  lookup(path, method = 'GET') {
    const methodRoot = this.methods[method];
    if (methodRoot === undefined) return undefined;

    const params = {};
    const handler = this.matchPath(methodRoot, path, 0, params);

    if (!handler) return undefined;
    return { handler, params };
  }

  matchPath(node, path, depth, params) {
    if (depth >= path.length) {
      return node.handler;
    }

    const remaining = path.substring(depth, path.length);
    for (let i = 0; i < node.children.length; i = i + 1) {
      const child = node.children[i];

      if (remaining.length >= child.prefix.length) {
        let matches = true;
        for (let j = 0; j < child.prefix.length; j = j + 1) {
          if (remaining[j] !== child.prefix[j]) {
            matches = false;
            break;
          }
        }

        if (matches) {
          const result = this.matchPath(child, path, depth + child.prefix.length, params);
          if (result !== undefined) return result;
        }
      }
    }

    if (node.paramChild !== undefined) {
      let paramValue = '';
      let offset = depth;

      for (let i = depth; i < path.length; i = i + 1) {
        if (path[i] === '/') break;
        paramValue = paramValue + path[i];
        offset = i + 1;
      }

      if (paramValue !== '') {
        params[node.paramChild.paramName] = paramValue;
        const result = this.matchPath(node.paramChild, path, offset, params);
        if (result !== undefined) return result;
        delete params[node.paramChild.paramName];
      }
    }

    if (node.wildcardChild !== undefined) {
      const wildcardValue = path.substring(depth, path.length);
      params[node.wildcardChild.paramName] = wildcardValue;
      return node.wildcardChild.handler;
    }

    return undefined;
  }

  printTree() {
    const methods = Object.keys(this.methods);
    for (let i = 0; i < methods.length; i = i + 1) {
      const method = methods[i];
      console.log('[' + method + ']');
      this.printNode(this.methods[method], '', true);
      if (i < methods.length - 1) console.log('');
    }
  }

  printNode(node, prefix, isLast) {
    const marker = isLast ? '└─ ' : '├─ ';
    let line = prefix + marker;

    if (node.prefix !== '') {
      line = line + '"' + node.prefix + '"';
    } else {
      line = line + '(root)';
    }

    if (node.handler !== undefined) {
      line = line + ' [HANDLER]';
    }

    if (node.paramName !== undefined) {
      line = line + ' :' + node.paramName;
    }

    console.log(line);
    const childPrefix = prefix + (isLast ? '    ' : '│   ');

    for (let i = 0; i < node.children.length; i = i + 1) {
      const isLastChild = i === node.children.length - 1 && node.paramChild === undefined && node.wildcardChild === undefined;
      this.printNode(node.children[i], childPrefix, isLastChild);
    }

    if (node.paramChild !== undefined) {
      const isLastChild = node.wildcardChild === undefined;
      console.log(childPrefix + (isLastChild ? '└─ ' : '├─ ') + ':' + node.paramChild.paramName);
      this.printNode(node.paramChild, childPrefix + (isLastChild ? '    ' : '│   '), true);
    }

    if (node.wildcardChild !== undefined) {
      console.log(childPrefix + '└─ *' + node.wildcardChild.paramName);
      this.printNode(node.wildcardChild, childPrefix + '    ', true);
    }
  }
}
