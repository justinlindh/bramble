// @testing-library/jest-dom publishes its matcher types by augmenting the
// global `jest.Matchers` interface, and vitest no longer declares one. Attach
// the same matcher types to vitest's own extension point so assertions like
// `expect(el).toBeInTheDocument()` type-check against the matchers that
// test/setup.ts registers at runtime.
import type { TestingLibraryMatchers } from '@testing-library/jest-dom/matchers';

declare module 'vitest' {
  interface Matchers<R extends void | Promise<void> = void | Promise<void>, T = unknown>
    extends TestingLibraryMatchers<unknown, R> {}
}
