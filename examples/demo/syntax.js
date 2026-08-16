import { stripTypes, parseJavaScript } from 'ant:syntax';

const typedSource = `
interface Sale {
  product: string;
  units: number;
  price: number;
  featured?: boolean;
}

interface Report {
  total: number;
  featured: string[];
  message: string;
}

const serviceFee: number = 1.25;

function buildReport(sales: Sale[], minimumRevenue: number = 20): Report {
  let total: number = 0;
  const featured: string[] = [];

  for (const sale of sales) {
    const revenue: number = sale.units * sale.price;

    if (revenue >= minimumRevenue) {
      total += revenue;

      if (sale.featured) {
        featured.push(sale.product);
      }
    }
  }

  return {
    total: total + serviceFee,
    featured,
    message: \`Processed \${sales.length} sales\`,
  };
}

const report: Report = buildReport([
  { product: 'Tea', units: 4, price: 8, featured: true },
  { product: 'Cake', units: 1, price: 12 },
  { product: 'Coffee', units: 6, price: 5, featured: true },
], 25);

console.log(report.message, report.total);
`.trim();

const javascript = stripTypes(typedSource, {
  filename: 'sales-report.ts',
  sourceType: 'module'
});

const tree = parseJavaScript(javascript, {
  filename: 'sales-report.js',
  sourceType: 'module',
  locations: true
});

const json = JSON.stringify(tree);
const portableTree = JSON.parse(json);

const operatorWords = {
  '===': 'IS',
  '!==': 'IS NOT',
  '==': 'EQUALS',
  '!=': 'DOES NOT EQUAL',
  '>=': 'IS AT LEAST',
  '<=': 'IS AT MOST',
  '>': 'IS GREATER THAN',
  '<': 'IS LESS THAN',
  '&&': 'AND',
  '||': 'OR',
  '??': 'OTHERWISE',
  '+': 'PLUS',
  '-': 'MINUS',
  '*': 'TIMES',
  '/': 'DIVIDED BY'
};

function literal(node) {
  if (node.bigint !== undefined) return `${node.bigint} (BIG INTEGER)`;
  if (node.regex) return `PATTERN /${node.regex.pattern}/${node.regex.flags}`;
  if (node.value === null) return 'NOTHING';
  if (node.value === true) return 'TRUE';
  if (node.value === false) return 'FALSE';
  return JSON.stringify(node.value);
}

function pattern(node) {
  if (!node) return 'NOTHING';

  switch (node.type) {
    case 'Identifier':
    case 'PrivateIdentifier':
      return node.type === 'PrivateIdentifier' ? `PRIVATE ${node.name}` : node.name;
    case 'AssignmentPattern':
      return `${pattern(node.left)} DEFAULTING TO ${expression(node.right)}`;
    case 'RestElement':
      return `THE REST AS ${pattern(node.argument)}`;
    case 'ArrayPattern':
      return `[${node.elements.map(pattern).join(', ')}]`;
    case 'ObjectPattern':
      return `{ ${node.properties.map(property => pattern(property.value)).join(', ')} }`;
    default:
      return expression(node);
  }
}

function propertyName(node) {
  if (node.type === 'Identifier' || node.type === 'PrivateIdentifier') return node.name;
  return expression(node);
}

function expression(node) {
  if (!node) return 'NOTHING';

  switch (node.type) {
    case 'Identifier':
      return node.name;
    case 'PrivateIdentifier':
      return `PRIVATE ${node.name}`;
    case 'Literal':
      return literal(node);
    case 'ThisExpression':
      return 'THIS OBJECT';
    case 'ArrayExpression':
      return `[${node.elements.map(item => (item ? expression(item) : 'EMPTY')).join(', ')}]`;
    case 'ObjectExpression':
      return `{ ${node.properties.map(expression).join(', ')} }`;
    case 'Property': {
      const key = propertyName(node.key);
      if (node.shorthand) return key;
      return `${key}: ${expression(node.value)}`;
    }
    case 'SpreadElement':
      return `ALL FIELDS FROM ${expression(node.argument)}`;
    case 'MemberExpression':
      return node.computed
        ? `${expression(node.object)} AT ${expression(node.property)}`
        : `${expression(node.object)}.${propertyName(node.property)}`;
    case 'CallExpression':
      return `CALL ${expression(node.callee)} WITH (${node.arguments.map(expression).join(', ')})`;
    case 'NewExpression':
      return `CREATE ${expression(node.callee)} WITH (${node.arguments.map(expression).join(', ')})`;
    case 'BinaryExpression':
    case 'LogicalExpression':
      return `(${expression(node.left)} ${operatorWords[node.operator] || node.operator} ${expression(node.right)})`;
    case 'UnaryExpression':
      return `${node.operator.toUpperCase()} ${expression(node.argument)}`;
    case 'ConditionalExpression':
      return `${expression(node.test)} ? ${expression(node.consequent)} : ${expression(node.alternate)}`;
    case 'AssignmentExpression':
      return `${expression(node.left)} ${node.operator} ${expression(node.right)}`;
    case 'UpdateExpression':
      return `${node.operator === '++' ? 'INCREMENT' : 'DECREMENT'} ${expression(node.argument)}`;
    case 'TemplateLiteral': {
      const parts = [];
      for (let i = 0; i < node.quasis.length; i++) {
        const text = node.quasis[i].value.cooked;
        if (text) parts.push(JSON.stringify(text));
        if (i < node.expressions.length) parts.push(expression(node.expressions[i]));
      }
      return `TEXT(${parts.join(', ')})`;
    }
    case 'ArrowFunctionExpression':
    case 'FunctionExpression':
      return `FUNCTION (${node.params.map(pattern).join(', ')})`;
    case 'AwaitExpression':
      return `WAIT FOR ${expression(node.argument)}`;
    default:
      return `<${node.type}>`;
  }
}

function indentation(depth) {
  return '  '.repeat(depth);
}

function renderBody(node, depth) {
  if (!node) return [];
  if (node.type === 'BlockStatement') {
    return node.body.flatMap(statement => renderStatement(statement, depth));
  }
  return renderStatement(node, depth);
}

function assignmentSentence(node) {
  const left = expression(node.left);
  const right = expression(node.right);

  switch (node.operator) {
    case '=':
      return `SET ${left} TO ${right}`;
    case '+=':
      return `INCREASE ${left} BY ${right}`;
    case '-=':
      return `DECREASE ${left} BY ${right}`;
    case '*=':
      return `MULTIPLY ${left} BY ${right}`;
    case '/=':
      return `DIVIDE ${left} BY ${right}`;
    default:
      return `${left} ${node.operator} ${right}`;
  }
}

function declarationName(node) {
  if (node.type !== 'VariableDeclaration') return pattern(node);
  return node.declarations.map(declaration => pattern(declaration.id)).join(', ');
}

function renderStatement(node, depth = 0) {
  const pad = indentation(depth);

  switch (node.type) {
    case 'Program':
      return node.body.flatMap(statement => renderStatement(statement, depth));
    case 'BlockStatement':
      return renderBody(node, depth);
    case 'VariableDeclaration':
      return node.declarations.map(declaration => {
        const value = declaration.init ? expression(declaration.init) : 'NOTHING';
        const permanence = node.kind === 'const' ? 'CONSTANT ' : '';
        return `${pad}SET ${permanence}${pattern(declaration.id)} TO ${value}`;
      });
    case 'FunctionDeclaration': {
      const asyncWord = node.async ? 'ASYNC ' : '';
      return [
        `${pad}DEFINE ${asyncWord}FUNCTION ${node.id.name} WITH (${node.params.map(pattern).join(', ')})`,
        ...renderBody(node.body, depth + 1),
        `${pad}END FUNCTION`
      ];
    }
    case 'IfStatement': {
      const lines = [`${pad}IF ${expression(node.test)} THEN`, ...renderBody(node.consequent, depth + 1)];
      if (node.alternate) {
        lines.push(`${pad}ELSE`);
        lines.push(...renderBody(node.alternate, depth + 1));
      }
      lines.push(`${pad}END IF`);
      return lines;
    }
    case 'ForOfStatement':
      return [`${pad}FOR EACH ${declarationName(node.left)} IN ${expression(node.right)}`, ...renderBody(node.body, depth + 1), `${pad}END FOR`];
    case 'WhileStatement':
      return [`${pad}WHILE ${expression(node.test)}`, ...renderBody(node.body, depth + 1), `${pad}END WHILE`];
    case 'ReturnStatement':
      return [`${pad}RETURN ${expression(node.argument)}`];
    case 'ThrowStatement':
      return [`${pad}THROW ${expression(node.argument)}`];
    case 'ExpressionStatement':
      return [`${pad}${node.expression.type === 'AssignmentExpression' ? assignmentSentence(node.expression) : expression(node.expression)}`];
    case 'BreakStatement':
      return [`${pad}STOP LOOP`];
    case 'ContinueStatement':
      return [`${pad}CONTINUE LOOP`];
    case 'EmptyStatement':
      return [];
    default:
      return [`${pad}<${node.type}>`];
  }
}

function countNodes(node, counts = {}) {
  if (!node || typeof node !== 'object') return counts;
  if (typeof node.type === 'string') counts[node.type] = (counts[node.type] || 0) + 1;

  if (Array.isArray(node)) {
    for (const child of node) countNodes(child, counts);
  } else {
    for (const key of Object.keys(node)) countNodes(node[key], counts);
  }
  return counts;
}

const counts = countNodes(portableTree);
const nodeCount = Object.values(counts).reduce((total, count) => total + count, 0);
const pseudocode = renderStatement(portableTree).join('\n');

console.log('typescript input:');
console.log(typedSource);
console.log('\nstripped javascript:');
console.log(javascript.trim());
console.log('\nportable ast:');
console.log(
  JSON.stringify(
    {
      schema: portableTree.schema,
      sourceType: portableTree.sourceType,
      jsonBytes: json.length,
      nodeCount,
      nodeKinds: Object.keys(counts).length
    },
    null,
    2
  )
);
console.log('\nreconstructed pseudocode:');
console.log(pseudocode);
