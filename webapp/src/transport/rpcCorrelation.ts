/**
 * JSON-RPC request/response correlation shared by the transports.
 *
 * A transport writes a request tagged with a monotonic numeric id and later
 * reads responses back in any order. This owns the id counter and the map of
 * outstanding requests: it hands out ids, matches each response to its waiter,
 * and fails every waiter at once when the link drops.
 *
 * WebSocketTransport, BLETransport and MockTransport share this exactly.
 * SerialTransport keeps its own copy on purpose: its pending entries carry
 * per-call tracing and are coupled to its write-queue release, so it cannot
 * adopt this without dragging that machinery in.
 */

interface PendingRpc {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class RpcCorrelation {
  private pending = new Map<number, PendingRpc>();
  private rpcId = 0;

  /** Next monotonic request id. */
  nextId(): number {
    return ++this.rpcId;
  }

  /**
   * Register request `id` and return the promise that settles when its
   * response arrives, or rejects with an "RPC timeout" error after `timeoutMs`.
   */
  request<T>(id: number, method: string, timeoutMs: number): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`RPC timeout: ${method}`));
      }, timeoutMs);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
    });
  }

  /**
   * If `msg` is a response to a tracked request, settle that request (resolve
   * with its result, or reject with its error message) and return true.
   * Otherwise leave `msg` for the caller to handle (e.g. as a notification)
   * and return false.
   */
  settle(msg: Record<string, unknown>): boolean {
    if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
      const { resolve, reject, timer } = this.pending.get(msg.id)!;
      clearTimeout(timer);
      this.pending.delete(msg.id);
      if (msg.error) {
        reject(new Error((msg.error as { message: string }).message));
      } else {
        resolve(msg.result);
      }
      return true;
    }
    return false;
  }

  /** Fail every outstanding request at once (the link dropped). */
  rejectAll(err: Error): void {
    for (const [, { reject, timer }] of this.pending) {
      clearTimeout(timer);
      reject(err);
    }
    this.pending.clear();
  }
}
