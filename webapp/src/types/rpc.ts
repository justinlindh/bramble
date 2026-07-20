// Typed view over the generated RPC contract (rpcContract.generated.ts, which
// is produced from api/openapi.yaml by `npm run gen:rpc-types`). The firmware
// side of that contract is gated by scripts/check-rpc-contract.sh, and
// test/rpcContractCoverage.test.ts pins the generated file's method set,
// per-method response wiring, and schema set to the spec, so the RPC surface
// here can only change by changing api/openapi.yaml and regenerating.
//
// Usage: `client.rpc('bramble.getConfig')` resolves its result type from the
// method name. Call sites that intentionally send params outside the contract
// (legacy snake_case fallbacks for older firmware) keep working through the
// explicit-generic overload, `client.rpc<T>(method, params)`.
import type { paths, components } from './rpcContract.generated';

export type RpcSchemas = components['schemas'];

// '/rpc/bramble.getConfig' -> 'bramble.getConfig', keyed to each POST op.
type MethodOps = {
  [K in keyof paths as K extends `/rpc/${infer M}` ? M : never]: paths[K] extends { post: infer Op }
    ? Op
    : never;
};

export type RpcMethod = keyof MethodOps & string;

export type RpcParams<M extends RpcMethod> = MethodOps[M] extends {
  requestBody?: { content: { 'application/json': infer B } };
}
  ? B
  : never;

export type RpcResult<M extends RpcMethod> = MethodOps[M] extends {
  responses: { 200: { content: { 'application/json': infer R } } };
}
  ? R
  : never;

// Firmware responses have historically drifted through several key spellings
// (snake_case vs camelCase, and renames like channel id/index), and the
// normalizers in store/actions keep defensive fallbacks for nodes running
// older firmware. WirePartial<T> is the contract shape with every field made
// deep-optional, because older firmware may omit canonical keys and send a
// legacy spelling instead. Normalizer input types are built as
// WirePartial<Schema> intersected with an explicit, documented set of legacy
// alias keys: the canonical shape comes from the contract, and the aliases
// are visible instead of hidden in `any`.
export type WirePartial<T> = T extends (infer E)[]
  ? WirePartial<E>[]
  : T extends object
    ? { [K in keyof T]?: WirePartial<T[K]> }
    : T;
