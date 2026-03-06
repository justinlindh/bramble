function startOfDay(d: Date): Date {
  const copy = new Date(d);
  copy.setHours(0, 0, 0, 0);
  return copy;
}

function dayDiffFromToday(messageDate: Date, now = new Date()): number {
  const today = startOfDay(now).getTime();
  const msgDay = startOfDay(messageDate).getTime();
  return Math.round((today - msgDay) / (24 * 60 * 60 * 1000));
}

function formatTimeOnly(date: Date): string {
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
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

  return date.toLocaleString([], {
    month: 'short',
    day: 'numeric',
    hour: 'numeric',
    minute: '2-digit',
  });
}

export function formatDaySeparatorLabel(timestampMs: number): string {
  return new Date(timestampMs).toLocaleDateString([], {
    month: 'long',
    day: 'numeric',
    year: 'numeric',
  });
}

export function shouldInsertDaySeparator(previousTimestampMs: number | undefined, currentTimestampMs: number): boolean {
  if (previousTimestampMs == null) return true;

  const prev = new Date(previousTimestampMs);
  const curr = new Date(currentTimestampMs);

  return !(
    prev.getFullYear() === curr.getFullYear() &&
    prev.getMonth() === curr.getMonth() &&
    prev.getDate() === curr.getDate()
  );
}
