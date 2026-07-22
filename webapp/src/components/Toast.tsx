import { useEffect, useState, useCallback } from 'react';
import { IconWarning } from './Icons';
import styles from './Toast.module.css';

export type ToastLevel = 'error' | 'warning' | 'info' | 'success';

export interface ToastItem {
  id: string;
  message: string;
  level: ToastLevel;
  autoHideMs?: number;
}

// ── Global toast API ────────────────────────────────────────────────────

type Listener = (toasts: ToastItem[]) => void;
let toastList: ToastItem[] = [];
const listeners = new Set<Listener>();
let idCounter = 0;

function emit() {
  for (const fn of listeners) fn([...toastList]);
}

export function showToast(message: string, level: ToastLevel = 'info', autoHideMs = 5000): string {
  const id = `toast-${++idCounter}`;
  toastList = [...toastList, { id, message, level, autoHideMs }];
  emit();
  return id;
}

export function dismissToast(id: string) {
  toastList = toastList.filter(t => t.id !== id);
  emit();
}

// ── Single toast renderer ──────────────────────────────────────────────

function ToastEntry({ toast, onDismiss }: { toast: ToastItem; onDismiss: (id: string) => void }) {
  const [leaving, setLeaving] = useState(false);

  useEffect(() => {
    if (!toast.autoHideMs || toast.autoHideMs <= 0) return;
    const t = setTimeout(() => {
      setLeaving(true);
      setTimeout(() => onDismiss(toast.id), 200);
    }, toast.autoHideMs);
    return () => clearTimeout(t);
  }, [toast.id, toast.autoHideMs, onDismiss]);

  const handleDismiss = () => {
    setLeaving(true);
    setTimeout(() => onDismiss(toast.id), 200);
  };

  return (
    <div className={`${styles.toast} ${styles[toast.level]} ${leaving ? styles.leaving : ''}`} role="alert">
      <span className={styles.icon}><IconWarning size={16} /></span>
      <span className={styles.message}>{toast.message}</span>
      <button className={styles.dismiss} onClick={handleDismiss} aria-label="Dismiss">✕</button>
    </div>
  );
}

// ── Container component ────────────────────────────────────────────────

export function ToastContainer() {
  const [toasts, setToasts] = useState<ToastItem[]>([]);

  useEffect(() => {
    listeners.add(setToasts);
    return () => { listeners.delete(setToasts); };
  }, []);

  const handleDismiss = useCallback((id: string) => dismissToast(id), []);

  if (toasts.length === 0) return null;

  return (
    <div className={styles.container}>
      {toasts.map(t => (
        <ToastEntry key={t.id} toast={t} onDismiss={handleDismiss} />
      ))}
    </div>
  );
}
