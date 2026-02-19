# Bramble Webapp: Unread Count Persistence Fix

**Date:** 2026-02-19  
**Commit:** `3920d93`  
**Issue:** Webapp showed all conversations as read after page refresh, even when unread messages existed in IndexedDB.

## Problem

The Bramble webapp uses two storage systems:
1. **IndexedDB** (via `messageDb.ts`) — Persists messages across page refreshes
2. **Zustand store** (via `store/index.ts`) — In-memory state including conversation metadata

When the page loaded:
- Messages were correctly restored from IndexedDB
- **BUT** unread counts were reset to 0 in the `loadCachedMessages` function

This created a poor UX where users would refresh the page and lose track of which conversations had new messages.

## Solution

Added **localStorage persistence** for unread counts, keyed by node address for multi-device support.

### New File: `webapp/src/store/unreadStore.ts`

Provides three functions:
- `saveUnreadCounts(nodeAddr, counts)` — Persist unreads to localStorage
- `loadUnreadCounts(nodeAddr)` — Restore unreads from localStorage
- `clearUnreadCounts(nodeAddr)` — Clear persisted unreads for a node

### Updated: `webapp/src/store/index.ts`

Added `persistUnreads()` helper that:
1. Extracts node address from config
2. Builds a map of conversation ID → unread count
3. Saves to localStorage via `saveUnreadCounts()`

Updated three actions to trigger persistence:

#### 1. `addMessage` — Persist when unreads increase
When an incoming message arrives, the unread count increments and is immediately persisted.

#### 2. `setActiveConversation` — Persist when unreads clear
When a user opens a conversation, the unread count is cleared (set to 0) and persisted.

#### 3. `loadCachedMessages` — Restore from localStorage
On page load, after rebuilding conversations from IndexedDB:
- Load persisted unreads via `loadUnreadCounts(nodeAddr)`
- Apply saved counts to each conversation: `unreadCount: savedUnreads[convId] ?? 0`

## Testing

Added comprehensive unit tests in `webapp/test/store/unreadStore.test.ts`:
- ✅ Save and load unread counts
- ✅ Return empty object when no data exists
- ✅ Isolate counts by node address (multi-device support)
- ✅ Clear counts for specific node
- ✅ Handle undefined node address gracefully

Updated `webapp/test/setup.ts` to mock localStorage for testing.

All tests pass:
```
Test Files  1 passed (1)
     Tests  5 passed (5)
```

## Verification Steps

To verify this fix works with real hardware:

1. **Connect to a Bramble node** via webapp
2. **Receive some messages** in different conversations (DMs, channels, broadcast)
3. **Don't open** those conversations (leave them unread)
4. **Refresh the page** (F5 or Ctrl+R)
5. **Verify:** Unread badges should persist across the refresh

### Expected Behavior

**Before fix:**
- All conversations show 0 unreads after page refresh
- User loses track of which conversations have new messages

**After fix:**
- Unread counts persist across page refreshes
- Opening a conversation clears its unread count (and persists that change)
- Switching between nodes maintains separate unread counts per node

## Implementation Details

### Storage Key Format
```
localStorage key: bramble-unreads-<NODE_ADDRESS_HEX>
Example: bramble-unreads-12345678
```

### Data Format
```json
{
  "dm:100": 5,
  "ch:1": 3,
  "broadcast": 2
}
```

- Keys are conversation IDs (same format as in-memory state)
- Values are unread counts (integers)
- Zeroed unreads are removed to keep storage lean

### Node Address Isolation

Unreads are keyed by node address (8-char hex string) so that:
- Each ESP32 node has its own unread state
- Switching between nodes in the same browser doesn't mix their unreads
- User can have multiple tabs open to different nodes without conflicts

### Graceful Degradation

If localStorage is unavailable (private browsing, quota exceeded, etc.):
- Errors are caught and logged to console
- App continues to function normally
- Unreads simply aren't persisted (original behavior)

## Files Changed

```
webapp/src/store/index.ts              — Updated to persist/restore unreads
webapp/src/store/unreadStore.ts        — New: localStorage persistence layer
webapp/test/store/unreadStore.test.ts  — New: Unit tests
webapp/test/setup.ts                   — Updated: Mock localStorage
```

## Known Limitations

1. **localStorage quota:** If a user has many nodes with many conversations, they could theoretically exceed the ~5-10MB localStorage limit. In practice, this is unlikely (100 conversations × 10 chars per entry = ~1KB).

2. **Manual localStorage edits:** If a user manually edits localStorage and corrupts the JSON, the app will log a warning and fall back to zero unreads.

3. **Cross-browser:** Unreads are not synced across different browsers on the same machine (this is expected localStorage behavior).

## Future Enhancements

Potential improvements for future commits:
- Sync unreads to IndexedDB instead of localStorage for better quota management
- Add a "mark all as read" button that persists across refresh
- Store last-read message ID instead of unread count for more accurate tracking
- Expose unread state in config API for cross-device sync

## Related Issues

This fix addresses the "Stale UI state" issue mentioned in:
- `memory/alice-bob-usernames-assigned-to-esp32-nodes-webapp-verified.md`

While that doc mentioned "Switching between nodes without a page reload shows stale channel/conversation data in the sidebar," this fix ensures that even WITH a page reload, unread state is preserved.
