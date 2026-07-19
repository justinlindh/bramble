// @vitest-environment node
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import openapiTS, { astToString } from 'openapi-typescript';

/**
 * Pin src/types/rpcContract.generated.ts to api/openapi.yaml.
 *
 * The firmware side of the contract is gated by scripts/check-rpc-contract.sh
 * (the spec must list exactly the methods main/rpc_methods.c registers). This
 * test closes the webapp side of the loop: the committed generated types must
 * match what openapi-typescript produces from the spec, so a spec change
 * cannot land without regenerating (npm run gen:rpc-types) and the typed RPC
 * boundary cannot drift from the contract.
 *
 * Regeneration happens in-process via the openapi-typescript node API: no
 * network, no CLI, same pinned package version as the npm script.
 */
describe('RPC contract generated types', () => {
  it('match api/openapi.yaml exactly (regenerate with: npm run gen:rpc-types)', async () => {
    const specUrl = new URL('../../api/openapi.yaml', import.meta.url);
    const generatedPath = fileURLToPath(new URL('../src/types/rpcContract.generated.ts', import.meta.url));

    const ast = await openapiTS(specUrl);
    const regenerated = astToString(ast);

    const committed = readFileSync(generatedPath, 'utf8');
    // The CLI prepends a banner comment the node API does not; compare from
    // the first declaration onward.
    const marker = 'export interface paths';
    const committedBody = committed.slice(committed.indexOf(marker));
    const regeneratedBody = regenerated.slice(regenerated.indexOf(marker));

    expect(committedBody).toBe(regeneratedBody);
  });
});
