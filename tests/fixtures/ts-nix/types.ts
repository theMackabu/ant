import { Flake, Input, NixFunction, Shell, ref } from '../../../packages/ts-nix/index.ts';

new Flake('typed').inputs(new Input('nixpkgs', 'github:NixOS/nixpkgs')).outputs(inputs => {
  inputs.nixpkgs.get('legacyPackages');
  inputs.self.get('shortRev');
  // @ts-expect-error Inputs retain literal names; unregistered inputs are not available.
  inputs.missing;
  return {};
});
new NixFunction(['pkgs', 'system'], args => {
  args.pkgs.get('mkShell');
  args.system.equals('aarch64-darwin');
  // @ts-expect-error Callback handles retain the argument pattern.
  args.unknown;
  return {};
});
new NixFunction('x', x => x.get('value'));
// @ts-expect-error A function argument handle is an expression, not a literal string.
new NixFunction('x', (x: string) => x);
// @ts-expect-error Shell environment values must be strings or Nix expressions.
new Shell(ref('pkgs')).env({ PORT: 123 });

new Flake('constructor inputs', [new Input('utils', 'github:numtide/flake-utils')])
  .outputs(inputs => inputs.utils.get('lib'));
