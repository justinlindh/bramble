/// <reference types="vite/client" />

// Declares Vite's ambient module types, including the `*.css` side-effect
// imports used by App.tsx and the device view. TypeScript 5.x tolerated a
// side-effect import of an unknown extension; TypeScript 7 requires a
// declaration and errors with TS2882 without one.
//
// The reference is needed explicitly because tsconfig.json sets an explicit
// `types` array (vitest/globals, @testing-library/jest-dom), which suppresses
// automatic inclusion of vite/client. This mirrors webapp/src/vite-env.d.ts.
