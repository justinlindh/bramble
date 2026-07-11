// uiActions.ts
//
// Shared browser-driving helpers used by both specs: load a scenario through
// the REAL ScenarioLoader dropdown+button (the same two clicks a human makes
// per emulator/README.md), and press a face button on a specific device card.
// Deliberately UI-driven, not a raw WebSocket injection, since the whole
// point of Task 13 is proving the thing a human actually operates works.

import type { Page } from '@playwright/test';

export async function loadScenario(page: Page, name: string): Promise<void> {
  const select = page.getByTestId('scenario-select');
  // The dropdown populates asynchronously from GET /api/scenarios; wait for
  // the target option to actually exist before selecting it (selectOption
  // does not retry on a missing <option>).
  await select.locator(`option[value="${name}"]`).waitFor({ state: 'attached', timeout: 10_000 });
  await select.selectOption(name);
  await page.getByTestId('scenario-load-btn').click();

  // Loading only reaches gosim's StateLoaded; the simulated radio/PHY event
  // pump (packet propagation between nodes) only runs once StateRunning,
  // which requires an explicit "play" command (PlaybackControls.tsx's
  // Play/Pause button, which starts paused). Firmware processes boot and
  // stream console/fb regardless of sim state, but nothing is ever
  // delivered between them until Play is pressed -- exactly the step a
  // human takes per emulator/README.md's walkthrough.
  const playBtn = page.getByTestId('play-pause-btn');
  await playBtn.waitFor({ state: 'visible', timeout: 10_000 });
  if ((await playBtn.getAttribute('title')) === 'Play') {
    await playBtn.click();
  }
}

export type FaceButtonId = 'up' | 'down' | 'select' | 'reset';

// clickButton presses and releases a face button on the given node's card
// (a real mousedown+mouseup, exactly what PagerDevice.tsx's onMouseDown/
// onMouseUp handlers expect).
export async function clickButton(page: Page, nodeId: string, id: FaceButtonId): Promise<void> {
  const card = page.getByTestId(`device-card-${nodeId}`);
  await card.getByTestId(`btn-${id}`).click();
}

// holdReset presses and holds the RESET pinhole for longer than
// PagerDevice.tsx's RESET_HOLD_MS (800ms) so the hold-to-confirm fires.
export async function holdReset(page: Page, nodeId: string): Promise<void> {
  const card = page.getByTestId(`device-card-${nodeId}`);
  const target = card.getByTestId('btn-reset');
  await target.hover();
  await page.mouse.down();
  await page.waitForTimeout(950);
  await page.mouse.up();
}
