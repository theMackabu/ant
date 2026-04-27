function assertEq(actual, expected, label) {
  if (actual !== expected) throw new Error(`${label}: expected ${JSON.stringify(expected)} got ${JSON.stringify(actual)}`);
}

const usingDeclaration = Ant.highlight.tags('using handle = resource;');
assertEq(
  usingDeclaration,
  '<#65B2FF>using</> handle <#8CB2D8>=</> resource<#B2CCE5>;</>',
  'using declaration keyword'
);

const awaitUsingDeclaration = Ant.highlight.tags('await using handle = resource;');
assertEq(
  awaitUsingDeclaration,
  '<#65B2FF>await</> <#65B2FF>using</> handle <#8CB2D8>=</> resource<#B2CCE5>;</>',
  'await using declaration keyword'
);

const usingArrowDeclaration = Ant.highlight.tags('using cleanup = () => {};');
assertEq(
  usingArrowDeclaration,
  '<#65B2FF>using</> <#30E8AA>cleanup</> <#8CB2D8>=</> <#8CB2D8>(</><#8CB2D8>)</> <#8CB2D8>=>></> <#8CB2D8>{</><#8CB2D8>}</><#B2CCE5>;</>',
  'using arrow declaration name'
);

const propertyAccess = Ant.highlight.tags('resource.using();');
assertEq(
  propertyAccess,
  'resource.<#30E8AA>using</><#8CB2D8>(</><#8CB2D8>)</><#B2CCE5>;</>',
  'using property access remains method-highlighted'
);

console.log('ok');
