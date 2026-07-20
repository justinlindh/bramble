// @vitest-environment node
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

/**
 * Pin src/types/rpcContract.generated.ts to api/openapi.yaml WITHOUT running a
 * code generator.
 *
 * openapi-typescript, which produced the committed file, requires a TypeScript
 * 5 peer and cannot be installed alongside the repo's TypeScript 7 toolchain,
 * so an in-process regenerate-and-diff test is not runnable here. This test is
 * the standing substitute: it parses both the spec and the committed types as
 * text and asserts they describe the same RPC surface.
 *
 * What it enforces (a structural contract, dependency-free):
 *   1. Method set: the /rpc/bramble.* methods in the spec exactly equal the
 *      path keys in the generated `paths` interface.
 *   2. Response wiring: for every method, the 200-response schema the spec
 *      names via $ref equals the one the generated operation references (and
 *      inline, unnamed responses stay inline on both sides).
 *   3. Schema set: the names under `components.schemas` in the spec exactly
 *      equal the schema names in the generated `components["schemas"]`.
 *   4. No dangling refs: every schema the generated file references is defined
 *      in that same file.
 *
 * The one thing it cannot see is a field-level change inside a schema whose
 * name did not change; regenerate with `npm run gen:rpc-types` after any spec
 * edit so the committed types pick those up. scripts/check-rpc-contract.sh
 * separately gates the spec's method list against the firmware registry, so
 * this closes the loop from firmware to webapp types.
 */

const specPath = fileURLToPath(new URL('../../api/openapi.yaml', import.meta.url));
const generatedPath = fileURLToPath(new URL('../src/types/rpcContract.generated.ts', import.meta.url));
const spec = readFileSync(specPath, 'utf8');
const generated = readFileSync(generatedPath, 'utf8');

// Slice [startRe, endRe) out of a file's lines.
function section(text: string, startRe: RegExp, endRe: RegExp): string {
  const lines = text.split('\n');
  const start = lines.findIndex((l) => startRe.test(l));
  if (start < 0) throw new Error(`section start ${startRe} not found`);
  const rel = lines.slice(start + 1).findIndex((l) => endRe.test(l));
  const end = rel < 0 ? lines.length : start + 1 + rel;
  return lines.slice(start, end).join('\n');
}

// ---- Spec: method -> 200 response schema name (null when the response is an
// inline, unnamed object) ----
function specMethodResponses(): Record<string, string | null> {
  const paths = section(spec, /^paths:/, /^components:/);
  const out: Record<string, string | null> = {};
  // Split into per-method blocks at each "  /rpc/...:" header.
  const parts = paths.split(/\n(?=  \/rpc\/)/);
  for (const block of parts) {
    const header = block.match(/^\s{2}(\/rpc\/(bramble\.[A-Za-z0-9_.]+)):/m);
    if (!header) continue;
    const method = header[2];
    // Take everything from the '200' response marker onward, then read the
    // first schema in it: a $ref name, or null if the schema is inline.
    const after200 = block.slice(block.search(/^\s+'200':/m));
    const ref = after200.match(/schema:\s*\n\s*\$ref:\s*'#\/components\/schemas\/([A-Za-z0-9_]+)'/);
    out[method] = ref ? ref[1] : null;
  }
  return out;
}

// ---- Spec: names under components.schemas ----
function specSchemaNames(): Set<string> {
  // Bound to the schemas subsection only: it ends at the next indent-2 key
  // under components (e.g. securitySchemes), not at column 0.
  const schemas = section(spec, /^  schemas:/, /^  [A-Za-z]/);
  const names = new Set<string>();
  for (const m of schemas.matchAll(/^    ([A-Za-z][A-Za-z0-9_]*):/gm)) names.add(m[1]);
  return names;
}

// ---- Generated: method -> operation name (from `post: operations["op"]`) ----
function generatedMethodOps(): Record<string, string> {
  const pathsSection = section(generated, /^export interface paths \{/, /^export interface components /);
  const out: Record<string, string> = {};
  const blocks = pathsSection.split(/\n(?=    "\/rpc\/)/);
  for (const block of blocks) {
    const key = block.match(/^\s{4}"(\/rpc\/(bramble\.[A-Za-z0-9_.]+))":/m);
    if (!key) continue;
    const op = block.match(/post:\s*operations\["([A-Za-z0-9_]+)"\]/);
    if (op) out[key[2]] = op[1];
  }
  return out;
}

// ---- Generated: operation name -> 200 response schema name (null when inline) ----
function generatedOpResponses(): Record<string, string | null> {
  const ops = section(generated, /^export interface operations \{/, /\n\}\s*$/);
  const out: Record<string, string | null> = {};
  const blocks = ops.split(/\n(?=    [A-Za-z0-9_]+: \{)/);
  for (const block of blocks) {
    const name = block.match(/^\s{4}([A-Za-z0-9_]+): \{/m);
    if (!name) continue;
    const after200 = block.slice(block.search(/^\s+200: \{/m));
    const ref = after200.match(/"application\/json":\s*components\["schemas"\]\["([A-Za-z0-9_]+)"\]/);
    out[name[1]] = ref ? ref[1] : null;
  }
  return out;
}

// ---- Generated: schema names defined under components["schemas"] ----
function generatedSchemaNames(): Set<string> {
  const comp = section(generated, /^    schemas: \{/, /^export interface operations /);
  const names = new Set<string>();
  for (const m of comp.matchAll(/^        ([A-Za-z][A-Za-z0-9_]*):/gm)) names.add(m[1]);
  return names;
}

describe('RPC contract generated types match api/openapi.yaml', () => {
  const specResp = specMethodResponses();
  const genOps = generatedMethodOps();
  const genOpResp = generatedOpResponses();

  it('exposes exactly the spec method set (regenerate with: npm run gen:rpc-types)', () => {
    expect(Object.keys(genOps).sort()).toEqual(Object.keys(specResp).sort());
    // operationId equals the method suffix throughout this spec, so the path
    // key must route to the matching operation.
    for (const [method, op] of Object.entries(genOps)) {
      expect(method).toBe(`bramble.${op}`);
    }
  });

  it('wires each method to the spec 200-response schema (regenerate with: npm run gen:rpc-types)', () => {
    const genByMethod: Record<string, string | null> = {};
    for (const [method, op] of Object.entries(genOps)) genByMethod[method] = genOpResp[op] ?? null;
    expect(genByMethod).toEqual(specResp);
  });

  it('defines exactly the spec schema set (regenerate with: npm run gen:rpc-types)', () => {
    expect([...generatedSchemaNames()].sort()).toEqual([...specSchemaNames()].sort());
  });

  it('has no dangling schema references', () => {
    const defined = generatedSchemaNames();
    const referenced = new Set<string>();
    for (const m of generated.matchAll(/components\["schemas"\]\["([A-Za-z0-9_]+)"\]/g)) referenced.add(m[1]);
    const dangling = [...referenced].filter((n) => !defined.has(n));
    expect(dangling).toEqual([]);
  });
});
