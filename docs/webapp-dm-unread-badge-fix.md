# Bramble Webapp: DM Unread Badge Fix

**Date:** 2026-02-22  
**Issue:** Non-broadcast chats (DMs) not showing proper unread count

## Problem

When a direct message (DM) arrived from one device to another, the corresponding DM conversation in the chat list did not display the unread count. The messages were incorrectly being filed under the 'broadcast' conversation instead of creating/updating a DM-specific conversation.

### Root Cause

The issue was in how incoming messages were being categorized as broadcast vs DM:

1. **In `handleIncomingMessage` (actions.ts):**
   - The logic `isBroadcast = p.broadcast === true || rawChannel === -1 || toAddr === 0xFFFFFFFF` was incorrect
   - When a DM arrived with `channelIndex: -1` (meaning "not a channel message"), it was being incorrectly marked as a broadcast
   - This caused the message's `to` field to be overwritten with `0xFFFFFFFF`, making it look like a broadcast

2. **In `addMessage` (index.ts):**
   - The logic `isBroadcast = msg.to === 0xffffffff || msg.channelIndex === -1` was also incorrect
   - Even if the incoming message handler was fixed, messages created with `channelIndex: -1` would still be filed under 'broadcast'

### Impact

- DM conversations never showed unread counts
- Users couldn't tell when they received DMs without opening the chat list
- Messages were potentially being shown in the wrong conversation view

## Solution

### Changes Made

1. **Updated `webapp/src/store/actions.ts` (handleIncomingMessage):**
   - Changed broadcast detection to only check `toAddr === 0xFFFFFFFF`
   - Removed the incorrect `rawChannel === -1` check from broadcast detection
   - `channelIndex: -1` now simply means "not a channel message" (could be DM or broadcast)
   - The actual `toAddr` is now preserved instead of being overwritten

2. **Updated `webapp/src/store/index.ts` (addMessage and loadCachedMessages):**
   - Changed broadcast detection to only check `msg.to === 0xffffffff`
   - Removed `msg.channelIndex === -1` from broadcast checks in both `addMessage` and `loadCachedMessages`
   - Added clarifying comment: "channelIndex === -1 means 'not a channel message', not 'broadcast'"

3. **Added tests in `webapp/test/store/actions.test.ts`:**
   - Test: incoming DM with `channelIndex: undefined` creates `dm:` conversation (not broadcast)
   - Test: incoming DM with `channelIndex: -1` creates `dm:` conversation
   - Test: broadcast message with `to: 0xFFFFFFFF` creates broadcast conversation

## Testing Results

```bash
✓ 80 tests passed (including 3 new DM tests)
✓ Build succeeds: dist/ generated successfully
✓ TypeScript compilation: no errors
✓ Docker container rebuilt successfully
```

## Behavior

### Before

- User 1 sends DM to User 2
- User 2's webapp receives the message
- Message appears in broadcast conversation OR conversation doesn't show unread badge
- User 2 doesn't notice the new DM

### After

- User 1 sends DM to User 2
- User 2's webapp receives the message
- Message appears in `dm:{User1Address}` conversation
- Unread badge shows "1" (or increments existing count)
- User 2 can see they have a new DM

### Still Works

- Broadcast messages (to `0xFFFFFFFF`) → filed under 'broadcast'
- Channel messages (channelIndex ≥ 0) → filed under 'ch:{index}'
- Outgoing DMs → filed under `dm:{recipientAddress}`
- Incoming DMs → filed under `dm:{senderAddress}`

## Files Modified

- `webapp/src/store/actions.ts` — Fixed `handleIncomingMessage` broadcast detection
- `webapp/src/store/index.ts` — Fixed `addMessage` and `loadCachedMessages` broadcast detection
- `webapp/test/store/actions.test.ts` — Added 3 new tests for DM conversation creation
- `docs/webapp-dm-unread-badge-fix.md` — This documentation

## Related Issues

This fix complements previous unread count fixes:
- Active chat unread count fix (commit `7568c66`) — prevented incrementing unread for already-visible messages
- Unread count persistence fix (commits `3920d93`, `0047d46`) — persisted unread counts across page refreshes

## Testing Checklist

- [x] Unit tests pass (80 tests)
- [x] Build succeeds with no TypeScript errors
- [x] Docker container builds successfully
- [ ] Manual test: Send DM from Device A to Device B, verify unread badge appears on Device B
- [ ] Manual test: Open DM conversation, verify unread count clears
- [ ] Manual test: Broadcast message still works correctly
- [ ] Manual test: Channel messages still work correctly
