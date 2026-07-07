/**
 * Hand-written type declarations for handler.mjs, the mock node's plain-JS
 * RPC handler module. handler.mjs is consumed from two very different
 * runtimes: node (mock/server.mjs, server/unified-server.mjs) and the
 * TypeScript webapp bundle (src/transport/MockTransport.ts, the in-page
 * mock used by embedded shells). Keeping the implementation as plain JS
 * avoids a build step for the standalone mock/ docker service; this file
 * gives the webapp side type-checking without duplicating the handler.
 */

/** Minimal shape of the ws-like socket handler.mjs expects (see ws's WebSocket). */
export interface MockSocketLike {
  readyState: number;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  on(event: 'message', listener: (data: { toString(): string } | string) => void): void;
  on(event: 'close', listener: () => void): void;
  on(event: 'error', listener: (err: Error) => void): void;
  _authed?: boolean;
}

/** Minimal shape of the http.IncomingMessage handler.mjs reads (url, remote addr). */
export interface MockRequestLike {
  url?: string;
  socket?: { remoteAddress?: string };
}

export type MockRpcHandler = (
  params: Record<string, unknown> | undefined,
  ctx?: { ws: MockSocketLike }
) => unknown;

export const handlers: Record<string, MockRpcHandler>;

export function handleConnection(ws: MockSocketLike, req?: MockRequestLike): void;
