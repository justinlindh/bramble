import { useState, useEffect } from 'react';
import type { RadioConfig } from '../../types/bramble';
import { saveRadio } from '../../store/actions';
import styles from './RadioForm.module.css';

interface RadioFormProps {
  radio: RadioConfig;
}

export function RadioForm({ radio }: RadioFormProps) {
  const [form, setForm] = useState<RadioConfig>(radio);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState('');

  // Sync if parent config changes (e.g. after re-connect)
  useEffect(() => {
    setForm(radio);
  }, [radio]);

  const handleSave = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    setSaved(false);
    try {
      await saveRadio(form);
      setSaved(true);
      setTimeout(() => setSaved(false), 2000);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setSaving(false);
    }
  };

  return (
    <form className={styles.form} onSubmit={handleSave}>
      {/* TX Power */}
      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="txPower">
          TX Power
        </label>
        <input
          id="txPower"
          className={styles.slider}
          type="range"
          min={2}
          max={20}
          step={1}
          value={form.txPowerDbm}
          onChange={(e) =>
            setForm((f) => ({ ...f, txPowerDbm: Number(e.target.value) }))
          }
          aria-label="TX Power in dBm"
        />
        <span className={styles.sliderValue}>{form.txPowerDbm} dBm</span>
      </div>

      {/* Spreading Factor */}
      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="sf">
          Spreading Factor
        </label>
        <select
          id="sf"
          className={styles.select}
          value={form.sf}
          onChange={(e) =>
            setForm((f) => ({
              ...f,
              sf: Number(e.target.value) as RadioConfig['sf'],
            }))
          }
        >
          {([7, 8, 9, 10, 11, 12] as const).map((n) => (
            <option key={n} value={n}>
              SF{n}
            </option>
          ))}
        </select>
      </div>

      {/* Bandwidth */}
      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="bw">
          Bandwidth
        </label>
        <select
          id="bw"
          className={styles.select}
          value={form.bwKhz}
          onChange={(e) =>
            setForm((f) => ({
              ...f,
              bwKhz: Number(e.target.value) as RadioConfig['bwKhz'],
            }))
          }
        >
          {([125, 250, 500] as const).map((n) => (
            <option key={n} value={n}>
              {n} kHz
            </option>
          ))}
        </select>
      </div>

      {/* Coding Rate */}
      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="cr">
          Coding Rate
        </label>
        <select
          id="cr"
          className={styles.select}
          value={form.cr}
          onChange={(e) =>
            setForm((f) => ({
              ...f,
              cr: Number(e.target.value) as RadioConfig['cr'],
            }))
          }
        >
          {([5, 6, 7, 8] as const).map((n) => (
            <option key={n} value={n}>
              4/{n}
            </option>
          ))}
        </select>
      </div>

      {/* Frequency */}
      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="freq">
          Frequency
        </label>
        <input
          id="freq"
          className={styles.input}
          type="number"
          step="0.1"
          min={137}
          max={1020}
          value={form.freqMhz}
          onChange={(e) =>
            setForm((f) => ({ ...f, freqMhz: Number(e.target.value) }))
          }
          aria-label="Frequency in MHz"
        />
        <span style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>MHz</span>
      </div>

      {/* Actions */}
      <div className={styles.actions}>
        <button className={styles.saveBtn} type="submit" disabled={saving}>
          {saving ? 'Saving…' : 'Save Radio Settings'}
        </button>
        {saved && <span className={styles.savedMsg}>✓ Saved</span>}
        {error && <span className={styles.error}>{error}</span>}
      </div>
    </form>
  );
}
