/**
 * Inline SVG icon components: monochrome, Lucide-style.
 * All icons use currentColor for stroke so they inherit text color.
 * ViewBox: 0 0 24 24, stroke-based, strokeWidth=2.
 */

import type { ReactNode } from 'react';

interface IconProps {
  size?: number;
  className?: string;
}

function Svg({ size = 16, className, children }: IconProps & { children: ReactNode }) {
  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
      aria-hidden="true"
    >
      {children}
    </svg>
  );
}

/** 💬 Chat / speech bubble */
export function IconChat({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z" />
    </Svg>
  );
}

/** 📡 Nodes / antenna / signal */
export function IconNodes({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M5 12.55a11 11 0 0 1 14.08 0" />
      <path d="M1.42 9a16 16 0 0 1 21.16 0" />
      <path d="M8.53 16.11a6 6 0 0 1 6.95 0" />
      <line x1="12" y1="20" x2="12" y2="22" />
    </Svg>
  );
}

/** ⚙️ Config / settings / gear */
export function IconConfig({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
    </Svg>
  );
}

/** 📊 Stats / bar chart */
export function IconStats({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <line x1="18" y1="20" x2="18" y2="10" />
      <line x1="12" y1="20" x2="12" y2="4" />
      <line x1="6" y1="20" x2="6" y2="14" />
    </Svg>
  );
}

/** 📢 Broadcast / megaphone */
export function IconBroadcast({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <circle cx="12" cy="12" r="2" />
      <path d="M16.24 7.76a6 6 0 0 1 0 8.49" />
      <path d="M7.76 16.24a6 6 0 0 1 0-8.49" />
      <path d="M19.07 4.93a10 10 0 0 1 0 14.14" />
      <path d="M4.93 19.07a10 10 0 0 1 0-14.14" />
    </Svg>
  );
}

/** 👤 User / person */
export function IconUser({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2" />
      <circle cx="12" cy="7" r="4" />
    </Svg>
  );
}

/** 🔴 Critical / alert circle */
export function IconCritical({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <circle cx="12" cy="12" r="10" />
      <line x1="12" y1="8" x2="12" y2="12" />
      <line x1="12" y1="16" x2="12.01" y2="16" />
    </Svg>
  );
}

/** 📬 Mailbox / inbox */
export function IconMailbox({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <polyline points="22 12 16 12 14 15 10 15 8 12 2 12" />
      <path d="M5.45 5.11L2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z" />
    </Svg>
  );
}

/** 📦 Packets / package / box */
export function IconPackets({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M16.5 9.4l-9-5.19" />
      <path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z" />
      <polyline points="3.27 6.96 12 12.01 20.73 6.96" />
      <line x1="12" y1="22.08" x2="12" y2="12" />
    </Svg>
  );
}

/** ⏱ Clock / timer */
export function IconClock({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <circle cx="12" cy="12" r="10" />
      <polyline points="12 6 12 12 16 14" />
    </Svg>
  );
}

/** 🗺 Routes / map */
export function IconRoutes({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <polygon points="3 6 9 3 15 6 21 3 21 18 15 21 9 18 3 21" />
      <line x1="9" y1="3" x2="9" y2="18" />
      <line x1="15" y1="6" x2="15" y2="21" />
    </Svg>
  );
}

/** Bluetooth symbol */
export function IconBluetooth({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <polyline points="6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5" />
    </Svg>
  );
}

/** 🔌 USB / plug */
export function IconUsb({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M7 5V2" />
      <path d="M17 5V2" />
      <rect x="5" y="5" width="14" height="6" rx="1" />
      <path d="M12 11v8" />
      <path d="M9 19h6" />
    </Svg>
  );
}

/** 🖥️ Monitor / desktop */
export function IconMonitor({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <rect x="2" y="3" width="20" height="14" rx="2" ry="2" />
      <line x1="8" y1="21" x2="16" y2="21" />
      <line x1="12" y1="17" x2="12" y2="21" />
    </Svg>
  );
}

/** WiFi signal icon */
export function IconWifi({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M5 12.55a11 11 0 0 1 14.08 0" />
      <path d="M1.42 9a16 16 0 0 1 21.16 0" />
      <path d="M8.53 16.11a6 6 0 0 1 6.95 0" />
      <circle cx="12" cy="20" r="1" fill="currentColor" />
    </Svg>
  );
}

/** # Hash / channel */
export function IconHash({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <line x1="4" y1="9" x2="20" y2="9" />
      <line x1="4" y1="15" x2="20" y2="15" />
      <line x1="10" y1="3" x2="8" y2="21" />
      <line x1="16" y1="3" x2="14" y2="21" />
    </Svg>
  );
}

/** + Plus / add */
export function IconPlus({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <line x1="12" y1="5" x2="12" y2="19" />
      <line x1="5" y1="12" x2="19" y2="12" />
    </Svg>
  );
}

/** ➤ Send / paper plane */
export function IconSend({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <line x1="22" y1="2" x2="11" y2="13" />
      <polygon points="22 2 15 22 11 13 2 9 22 2" />
    </Svg>
  );
}

/** 🪪 Identity / ID card */
export function IconIdentity({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <rect x="2" y="4" width="20" height="16" rx="2" ry="2" />
      <circle cx="8" cy="11" r="2.5" />
      <path d="M13 9h5" />
      <path d="M13 13h5" />
      <path d="M5 19c0-1.66 1.34-3 3-3s3 1.34 3 3" />
    </Svg>
  );
}

/** 📻 Radio */
export function IconRadio({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M4.9 19.1C1 15.2 1 8.8 4.9 4.9" />
      <path d="M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.4" />
      <circle cx="12" cy="12" r="2" />
      <path d="M16.2 7.8c2.3 2.3 2.3 6.1 0 8.4" />
      <path d="M19.1 4.9C23 8.8 23 15.1 19.1 19" />
    </Svg>
  );
}

/** 👥 Peers / group of people */
export function IconPeers({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2" />
      <circle cx="9" cy="7" r="4" />
      <path d="M23 21v-2a4 4 0 0 0-3-3.87" />
      <path d="M16 3.13a4 4 0 0 1 0 7.75" />
    </Svg>
  );
}

/** 📨 Envelope / send DM */
export function IconEnvelope({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2z" />
      <polyline points="22,6 12,13 2,6" />
    </Svg>
  );
}

/** 📡 Probe / radar: concentric arcs with center dot */
export function IconProbe({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <circle cx="12" cy="18" r="2" />
      <path d="M8 14a5.66 5.66 0 0 1 8 0" />
      <path d="M5 11a9.9 9.9 0 0 1 14 0" />
      <path d="M2 8a14.14 14.14 0 0 1 20 0" />
    </Svg>
  );
}

/** 📍 Location pin */
export function IconLocation({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z" />
      <circle cx="12" cy="10" r="3" />
    </Svg>
  );
}

/** 📍🚫 Location off / disabled */
export function IconLocationOff({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z" opacity="0.4" />
      <circle cx="12" cy="10" r="3" opacity="0.4" />
      <line x1="2" y1="2" x2="22" y2="22" />
    </Svg>
  );
}

/** 🗺️ Map */
export function IconMap({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <polygon points="1 6 1 22 8 18 16 22 23 18 23 2 16 6 8 2 1 6" />
      <line x1="8" y1="2" x2="8" y2="18" />
      <line x1="16" y1="6" x2="16" y2="22" />
    </Svg>
  );
}

/** 🗄️ Database / storage */
export function IconDatabase({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <ellipse cx="12" cy="5" rx="8" ry="3" />
      <path d="M4 5v14c0 1.66 3.58 3 8 3s8-1.34 8-3V5" />
      <path d="M4 12c0 1.66 3.58 3 8 3s8-1.34 8-3" />
    </Svg>
  );
}

/** ⚠️ Warning / alert triangle */
export function IconWarning({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z" />
      <line x1="12" y1="9" x2="12" y2="13" />
      <line x1="12" y1="17" x2="12.01" y2="17" />
    </Svg>
  );
}

/** 🔒 Lock / protected */
export function IconLock({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <rect x="4" y="11" width="16" height="10" rx="2" />
      <path d="M8 11V7a4 4 0 0 1 8 0v4" />
    </Svg>
  );
}

/** 🔑 Key / security */
export function IconKey({ size = 16, className }: IconProps) {
  return (
    <Svg size={size} className={className}>
      <path d="M21 2l-2 2m-7.61 7.61a5.5 5.5 0 1 1-7.778 7.778 5.5 5.5 0 0 1 7.777-7.777zm0 0L15.5 7.5m0 0l3 3L22 7l-3-3m-3.5 3.5L19 4" />
    </Svg>
  );
}
