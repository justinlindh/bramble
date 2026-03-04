# Webapp UX Audit Fixes Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Implement the top Webapp UX audit improvements: safe chat auto-scroll with jump control, route visibility persistence, richer map route legend, and fragmentation guidance in compose UI.

**Architecture:** Keep behavior localized to existing React pages/components and centralize route visibility persistence in the Zustand store so Chat and Map share one source of truth. Add small, testable utility functions and targeted component/store tests before implementation changes.

**Tech Stack:** React 19, TypeScript, Zustand, Vitest, Testing Library.

---

### Task 1: Chat auto-scroll should respect user scroll position + provide jump-to-latest control

**Files:**
- Modify: `webapp/src/pages/Chat/Chat.tsx`
- Modify: `webapp/src/pages/Chat/Chat.module.css`
- Test: `webapp/test/chat/chatScrollBehavior.test.tsx`

**Step 1: Write failing tests**
- Add tests for:
  - `isNearBottom()` helper threshold behavior.
  - `shouldAutoScroll` behavior when user is scrolled up.
  - visibility of a `Scroll to latest` button when user is not near bottom.

**Step 2: Run test to verify it fails**
- Run: `npm test -- test/chat/chatScrollBehavior.test.tsx`
- Expected: FAIL for missing helper/button behavior.

**Step 3: Write minimal implementation**
- Add near-bottom detection in message list scroll handler.
- Only auto-scroll on new messages when currently near bottom.
- Add sticky "↓ New messages" button to jump to bottom.

**Step 4: Run test to verify it passes**
- Run: `npm test -- test/chat/chatScrollBehavior.test.tsx`
- Expected: PASS.

**Step 5: Commit**
- `git add webapp/src/pages/Chat/Chat.tsx webapp/src/pages/Chat/Chat.module.css webapp/test/chat/chatScrollBehavior.test.tsx`
- `git commit -m "feat(webapp): add respectful chat auto-scroll and jump-to-latest"`

### Task 2: Persist route visibility toggle across refresh and unify Chat/Map behavior

**Files:**
- Modify: `webapp/src/store/index.ts`
- Modify: `webapp/src/pages/Map/Map.tsx`
- Test: `webapp/test/store/routeVisibilityStore.test.ts`

**Step 1: Write failing tests**
- Add store tests for:
  - initial `showRoutes` loading from localStorage.
  - `setShowRoutes` writing updated value to localStorage.

**Step 2: Run test to verify it fails**
- Run: `npm test -- test/store/routeVisibilityStore.test.ts`
- Expected: FAIL due to missing persistence.

**Step 3: Write minimal implementation**
- Add `ROUTE_VISIBILITY_KEY` and safe load/save helpers in store.
- Initialize `showRoutes` from localStorage.
- Persist inside `setShowRoutes`.
- In `Map.tsx`, remove local `useState(true)` and use global store state/actions.

**Step 4: Run test to verify it passes**
- Run: `npm test -- test/store/routeVisibilityStore.test.ts`
- Expected: PASS.

**Step 5: Commit**
- `git add webapp/src/store/index.ts webapp/src/pages/Map/Map.tsx webapp/test/store/routeVisibilityStore.test.ts`
- `git commit -m "feat(webapp): persist route visibility across sessions"`

### Task 3: Improve map route legend + compose fragmentation guidance

**Files:**
- Modify: `webapp/src/pages/Map/Map.tsx`
- Modify: `webapp/src/pages/Map/Map.module.css`
- Modify: `webapp/src/pages/Chat/ComposeBar.tsx`
- Test: `webapp/test/chat/composeCounterTooltip.test.tsx`

**Step 1: Write failing tests**
- Add compose test asserting counter includes fragmentation tooltip/title text.

**Step 2: Run test to verify it fails**
- Run: `npm test -- test/chat/composeCounterTooltip.test.tsx`
- Expected: FAIL since tooltip text is missing.

**Step 3: Write minimal implementation**
- Add route legend entries in map for line semantics:
  - direct vs multi-hop style
  - active/stale/broken/discovering colors
- Add title/aria-label to byte counter explaining fragmentation behavior.

**Step 4: Run test to verify it passes**
- Run: `npm test -- test/chat/composeCounterTooltip.test.tsx`
- Expected: PASS.

**Step 5: Commit**
- `git add webapp/src/pages/Map/Map.tsx webapp/src/pages/Map/Map.module.css webapp/src/pages/Chat/ComposeBar.tsx webapp/test/chat/composeCounterTooltip.test.tsx`
- `git commit -m "feat(webapp): document route semantics and message fragmentation"`

### Task 4: Full verification

**Files:**
- No source changes expected

**Step 1: Run focused and full tests**
- `npm test -- test/chat/chatScrollBehavior.test.tsx`
- `npm test -- test/store/routeVisibilityStore.test.ts`
- `npm test -- test/chat/composeCounterTooltip.test.tsx`
- `npm test`

**Step 2: Run build**
- `npm run build`

**Step 3: Final commit (if needed)**
- `git add -A`
- `git commit -m "test(webapp): verify UX audit improvements"`
