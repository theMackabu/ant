const VOID = new Set(['br', 'hr', 'img', 'input', 'meta', 'link', 'area', 'base', 'col', 'embed', 'source', 'track', 'wbr']);

const ESCAPE_MAP = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;'
};

const ATTR_MAP = {
  className: 'class',
  htmlFor: 'for',
  tabIndex: 'tabindex',
  readOnly: 'readonly',
  maxLength: 'maxlength',
  cellSpacing: 'cellspacing',
  cellPadding: 'cellpadding',
  rowSpan: 'rowspan',
  colSpan: 'colspan',
  encType: 'enctype',
  contentEditable: 'contenteditable',
  crossOrigin: 'crossorigin',
  accessKey: 'accesskey',
  autoComplete: 'autocomplete',
  autoFocus: 'autofocus',
  autoPlay: 'autoplay',
  formAction: 'formaction',
  noValidate: 'novalidate'
};

const UNITLESS = new Set([
  'animationIterationCount',
  'columns',
  'columnCount',
  'flex',
  'flexGrow',
  'flexShrink',
  'fontWeight',
  'gridColumn',
  'gridRow',
  'lineHeight',
  'opacity',
  'order',
  'orphans',
  'tabSize',
  'widows',
  'zIndex'
]);

function camelToKebab(str) {
  return str.replace(/[A-Z]/g, ch => `-${ch.toLowerCase()}`);
}

function escapeHTML(str) {
  return String(str).replace(/[&<>"']/g, ch => ESCAPE_MAP[ch]);
}

function formatValue(key, val) {
  if (typeof val === 'number' && val !== 0 && !UNITLESS.has(key)) return `${val}px`;
  return val;
}

function styleToString(style) {
  return Object.entries(style)
    .filter(([, v]) => v != null && v !== '')
    .map(([k, v]) => `${camelToKebab(k)}:${formatValue(k, v)}`)
    .join(';');
}

function renderAttrs(props) {
  let result = '';
  for (const [key, val] of Object.entries(props)) {
    if (key === 'children' || key === 'dangerouslySetInnerHTML') continue;
    if (key.startsWith('on') && key[2] >= 'A' && key[2] <= 'Z') continue;
    if (val == null || val === false) continue;

    if (key === 'style' && typeof val === 'object') {
      result += ` style="${escapeHTML(styleToString(val))}"`;
      continue;
    }

    const attr = ATTR_MAP[key] || key;
    if (val === true) result += ` ${attr}`;
    else result += ` ${attr}="${escapeHTML(val)}"`;
  }
  return result;
}

export function renderToHTML(element) {
  if (element == null || typeof element === 'boolean') return '';
  if (typeof element === 'string') return escapeHTML(element);
  if (typeof element === 'number') return String(element);
  if (Array.isArray(element)) return element.map(renderToHTML).join('');

  const { type, props } = element;

  if (type === Symbol.for('react.fragment') || type === undefined) {
    return renderChildren(props?.children);
  }

  if (typeof type === 'function') {
    if (type.prototype && type.prototype.isReactComponent) {
      const instance = new type(props);
      return renderToHTML(instance.render());
    }
    return renderToHTML(type(props));
  }

  const attrStr = renderAttrs(props || {});
  if (VOID.has(type)) return `<${type}${attrStr}>`;

  if (props?.dangerouslySetInnerHTML) {
    return `<${type}${attrStr}>${props.dangerouslySetInnerHTML.__html}</${type}>`;
  }

  return `<${type}${attrStr}>${renderChildren(props?.children)}</${type}>`;
}

function renderChildren(children) {
  if (children == null) return '';
  if (Array.isArray(children)) return children.map(renderToHTML).join('');
  return renderToHTML(children);
}
