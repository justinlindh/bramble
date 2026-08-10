/// <reference types="vite/client" />

declare const __APP_VERSION__: string;

interface ImportMetaEnv {
  // Days of delivery-event history to retain locally; parsed with Number().
  readonly VITE_DELIVERY_EVENT_RETENTION_DAYS?: string;
}

// CSS Modules
declare module '*.module.css' {
  const classes: Record<string, string>;
  export default classes;
}
