const NullProtoObj = /* @__PURE__ */ (() => {
  const e = function () {};
  return ((e.prototype = Object.create(null)), Object.freeze(e.prototype), e);
})();

function createRouter() {
  return {
    root: { key: '' },
    static: new NullProtoObj()
  };
}

function splitPath(path) {
  const [_, ...s] = path.split('/');
  return s[s.length - 1] === '' ? s.slice(0, -1) : s;
}
function getMatchParams(segments, paramsMap) {
  const params = new NullProtoObj();
  for (const [index, name] of paramsMap) {
    const segment = index < 0 ? segments.slice(-(index + 1)).join('/') : segments[index];
    if (typeof name === 'string') params[name] = segment;
    else {
      const match = segment.match(name);
      if (match) for (const key in match.groups) params[key] = match.groups[key];
    }
  }
  return params;
}

function addRoute(ctx, method = '', path, data) {
  method = method.toUpperCase();
  if (path.charCodeAt(0) !== 47) path = `/${path}`;
  path = path.replace(/\\:/g, '%3A');
  const segments = splitPath(path);
  let node = ctx.root;
  let _unnamedParamIndex = 0;
  const paramsMap = [];
  const paramsRegexp = [];
  for (let i = 0; i < segments.length; i++) {
    let segment = segments[i];
    if (segment.startsWith('**')) {
      if (!node.wildcard) node.wildcard = { key: '**' };
      node = node.wildcard;
      paramsMap.push([-(i + 1), segment.split(':')[1] || '_', segment.length === 2]);
      break;
    }
    if (segment === '*' || segment.includes(':')) {
      if (!node.param) node.param = { key: '*' };
      node = node.param;
      if (segment === '*') paramsMap.push([i, `_${_unnamedParamIndex++}`, true]);
      else if (segment.includes(':', 1)) {
        const regexp = getParamRegexp(segment);
        paramsRegexp[i] = regexp;
        node.hasRegexParam = true;
        paramsMap.push([i, regexp, false]);
      } else paramsMap.push([i, segment.slice(1), false]);
      continue;
    }
    if (segment === '\\*') segment = segments[i] = '*';
    else if (segment === '\\*\\*') segment = segments[i] = '**';
    const child = node.static?.[segment];
    if (child) node = child;
    else {
      const staticNode = { key: segment };
      if (!node.static) node.static = new NullProtoObj();
      node.static[segment] = staticNode;
      node = staticNode;
    }
  }
  const hasParams = paramsMap.length > 0;
  if (!node.methods) node.methods = new NullProtoObj();
  node.methods[method] ??= [];
  node.methods[method].push({
    data: data || null,
    paramsRegexp,
    paramsMap: hasParams ? paramsMap : void 0
  });
  if (!hasParams) ctx.static['/' + segments.join('/')] = node;
}
function getParamRegexp(segment) {
  const regex = segment.replace(/:(\w+)/g, (_, id) => `(?<${id}>[^/]+)`).replace(/\./g, '\\.');
  return /* @__PURE__ */ new RegExp(`^${regex}$`);
}

function findRoute(ctx, method = '', path, opts) {
  if (path.charCodeAt(path.length - 1) === 47) path = path.slice(0, -1);
  const staticNode = ctx.static[path];
  if (staticNode && staticNode.methods) {
    const staticMatch = staticNode.methods[method] || staticNode.methods[''];
    if (staticMatch !== void 0) return staticMatch[0];
  }
  const segments = splitPath(path);
  const match = _lookupTree(ctx, ctx.root, method, segments, 0)?.[0];
  if (match === void 0) return;
  if (opts?.params === false) return match;
  return {
    data: match.data,
    params: match.paramsMap ? getMatchParams(segments, match.paramsMap) : void 0
  };
}
function _lookupTree(ctx, node, method, segments, index) {
  if (index === segments.length) {
    if (node.methods) {
      const match = node.methods[method] || node.methods[''];
      if (match) return match;
    }
    if (node.param && node.param.methods) {
      const match = node.param.methods[method] || node.param.methods[''];
      if (match) {
        const pMap = match[0].paramsMap;
        if (pMap?.[pMap?.length - 1]?.[2]) return match;
      }
    }
    if (node.wildcard && node.wildcard.methods) {
      const match = node.wildcard.methods[method] || node.wildcard.methods[''];
      if (match) {
        const pMap = match[0].paramsMap;
        if (pMap?.[pMap?.length - 1]?.[2]) return match;
      }
    }
    return;
  }
  const segment = segments[index];
  if (node.static) {
    const staticChild = node.static[segment];
    if (staticChild) {
      const match = _lookupTree(ctx, staticChild, method, segments, index + 1);
      if (match) return match;
    }
  }
  if (node.param) {
    const match = _lookupTree(ctx, node.param, method, segments, index + 1);
    if (match) {
      if (node.param.hasRegexParam) {
        const exactMatch = match.find(m => m.paramsRegexp[index]?.test(segment)) || match.find(m => !m.paramsRegexp[index]);
        return exactMatch ? [exactMatch] : void 0;
      }
      return match;
    }
  }
  if (node.wildcard && node.wildcard.methods) return node.wildcard.methods[method] || node.wildcard.methods[''];
}

function removeRoute(ctx, method, path) {
  const segments = splitPath(path);
  return _remove(ctx.root, method || '', segments, 0);
}
function _remove(node, method, segments, index) {
  if (index === segments.length) {
    if (node.methods && method in node.methods) {
      delete node.methods[method];
      if (Object.keys(node.methods).length === 0) node.methods = void 0;
    }
    return;
  }
  const segment = segments[index];
  if (segment === '*') {
    if (node.param) {
      _remove(node.param, method, segments, index + 1);
      if (_isEmptyNode(node.param)) node.param = void 0;
    }
    return;
  }
  if (segment.startsWith('**')) {
    if (node.wildcard) {
      _remove(node.wildcard, method, segments, index + 1);
      if (_isEmptyNode(node.wildcard)) node.wildcard = void 0;
    }
    return;
  }
  const childNode = node.static?.[segment];
  if (childNode) {
    _remove(childNode, method, segments, index + 1);
    if (_isEmptyNode(childNode)) {
      delete node.static[segment];
      if (Object.keys(node.static).length === 0) node.static = void 0;
    }
  }
}
function _isEmptyNode(node) {
  return node.methods === void 0 && node.static === void 0 && node.param === void 0 && node.wildcard === void 0;
}

function findAllRoutes(ctx, method = '', path, opts) {
  if (path.charCodeAt(path.length - 1) === 47) path = path.slice(0, -1);
  const segments = splitPath(path);
  const matches = _findAll(ctx, ctx.root, method, segments, 0);
  if (opts?.params === false) return matches;
  return matches.map(m => {
    return {
      data: m.data,
      params: m.paramsMap ? getMatchParams(segments, m.paramsMap) : void 0
    };
  });
}
function _findAll(ctx, node, method, segments, index, matches = []) {
  const segment = segments[index];
  if (node.wildcard && node.wildcard.methods) {
    const match = node.wildcard.methods[method] || node.wildcard.methods[''];
    if (match) matches.push(...match);
  }
  if (node.param) {
    _findAll(ctx, node.param, method, segments, index + 1, matches);
    if (index === segments.length && node.param.methods) {
      const match = node.param.methods[method] || node.param.methods[''];
      if (match) {
        const pMap = match[0].paramsMap;
        if (pMap?.[pMap?.length - 1]?.[2]) matches.push(...match);
      }
    }
  }
  const staticChild = node.static?.[segment];
  if (staticChild) _findAll(ctx, staticChild, method, segments, index + 1, matches);
  if (index === segments.length && node.methods) {
    const match = node.methods[method] || node.methods[''];
    if (match) matches.push(...match);
  }
  return matches;
}

function routeToRegExp(route = '/') {
  const reSegments = [];
  let idCtr = 0;
  for (const segment of route.split('/')) {
    if (!segment) continue;
    if (segment === '*') reSegments.push(`(?<_${idCtr++}>[^/]*)`);
    else if (segment.startsWith('**')) reSegments.push(segment === '**' ? '?(?<_>.*)' : `?(?<${segment.slice(3)}>.+)`);
    else if (segment.includes(':')) reSegments.push(segment.replace(/:(\w+)/g, (_, id) => `(?<${id}>[^/]+)`).replace(/\./g, '\\.'));
    else reSegments.push(segment);
  }
  return /* @__PURE__ */ new RegExp(`^/${reSegments.join('/')}/?$`);
}

export { NullProtoObj, addRoute, createRouter, findAllRoutes, findRoute, removeRoute, routeToRegExp };
