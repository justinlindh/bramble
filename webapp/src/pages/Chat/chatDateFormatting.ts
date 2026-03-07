const CHAT_TIME_ZONE = 'America/Los_Angeles';
const DATE_KEY_LOCALE = 'en-CA'; // YYYY-MM-DD
const DISPLAY_LOCALE = 'en-US';

function pacificDateKey(date: Date): string {
  return date.toLocaleDateString(DATE_KEY_LOCALE, {
    timeZone: CHAT_TIME_ZONE,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
  });
}

function startOfDay(d: Date): Date {
  const dateKey = pacificDateKey(d);
  return new Date(`${dateKey}T00:00:00`);
}

function dayDiffFromToday(messageDate: Date, now = new Date()): number {
  const today = startOfDay(now).getTime();
  const msgDay = startOfDay(messageDate).getTime();
  return Math.round((today - msgDay) / (24 * 60 * 60 * 1000));
}

function formatTimeOnly(date: Date): string {
  return date.toLocaleTimeString(DISPLAY_LOCALE, {
    timeZone: CHAT_TIME_ZONE,
    hour: '2-digit',
    minute: '2-digit',
  });
}

export function formatMessageTimestamp(timestampMs: number, now = new Date()): string {
  const date = new Date(timestampMs);
  const diffDays = dayDiffFromToday(date, now);

  if (diffDays <= 0) {
    return formatTimeOnly(date);
  }

  if (diffDays === 1) {
    return `Yesterday ${formatTimeOnly(date)}`;
  }

  return date.toLocaleString(DISPLAY_LOCALE, {
    timeZone: CHAT_TIME_ZONE,
    month: 'short',
    day: 'numeric',
    hour: 'numeric',
    minute: '2-digit',
  });
}

export function formatDaySeparatorLabel(timestampMs: number): string {
  return new Date(timestampMs).toLocaleDateString(DISPLAY_LOCALE, {
    timeZone: CHAT_TIME_ZONE,
    month: 'long',
    day: 'numeric',
    year: 'numeric',
  });
}

export function shouldInsertDaySeparator(previousTimestampMs: number | undefined, currentTimestampMs: number): boolean {
  if (previousTimestampMs == null) return true;

  const prev = new Date(previousTimestampMs);
  const curr = new Date(currentTimestampMs);

  return pacificDateKey(prev) !== pacificDateKey(curr);
}
