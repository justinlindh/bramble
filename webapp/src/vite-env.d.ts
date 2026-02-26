/// <reference types="vite/client" />

declare const __APP_VERSION__: string;

// CSS Modules
declare module '*.module.css' {
  const classes: Record<string, string>;
  export default classes;
}
