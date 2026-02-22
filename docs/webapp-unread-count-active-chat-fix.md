# Bramble Webapp: Active Chat Unread Count Fix

**Date:** 2026-02-22  
**Issue:** Unreads not clearing when messages arrive in the currently active chat

## Problem

When the user has a chat conversation open and new messages arrive in that same active conversation, the unread count was being incremented even though the messages are already visible to the user. This required the user to click away and back to the channel to clear the unread count.

## Root Cause

The `addMessage` action in `webapp/src/store/index.ts` was unconditionally incrementing the unread count for all incoming messages without checking whether the conversation receiving the message was currently active (i.e., being viewed by the user).

```typescript
// Before (incorrect):
unreadCount: (prev?.unreadCount ?? 0) + (msg.direction === 'incoming' ? 1 : 0)
```

## Solution

Modified the `addMessage` action to check if the conversation receiving the incoming message is currently active. If it is, the unread count is not incremented since the user is already viewing those messages.

```typescript
// After (correct):
const isActive = state.activeConversationId === convId;
const shouldIncrementUnread = msg.direction === 'incoming' && !isActive;

unreadCount: (prev?.unreadCount ?? 0) + (shouldIncrementUnread ? 1 : 0)
```

## Files Modified

- `webapp/src/store/index.ts` — Updated `addMessage` action logic
- `webapp/test/store/actions.test.ts` — Added two new tests:
  - `incoming message does not increment unread for active conversation`
  - `incoming message increments unread for inactive conversation`

## Testing

All existing tests pass, plus two new tests verify:
1. Messages arriving in the active conversation keep unread count at 0
2. Messages arriving in inactive conversations still increment unread count correctly

```bash
cd ~/src/bramble/webapp
npm test -- actions.test.ts
# ✓ 11 tests passed
```

## Behavior

- **Before:** User viewing Chat A → new message arrives in Chat A → unread badge shows "1" → user must click Chat A again to clear
- **After:** User viewing Chat A → new message arrives in Chat A → unread badge stays at "0" (messages already visible)
- **Still works:** User viewing Chat A → new message arrives in Chat B → Chat B unread badge shows "1" correctly
