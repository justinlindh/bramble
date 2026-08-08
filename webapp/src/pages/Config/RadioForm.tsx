import { useState, useEffect, useMemo } from "react";
import type { RadioConfig } from "../../types/bramble";
import { saveRadio } from "../../store/actions";
import { useTimedFlag } from "../../hooks/useTimedFlag";
import { friendlyErrorFrom } from "../../lib/errors";
import styles from "./RadioForm.module.css";

interface RadioFormProps {
  radio: RadioConfig;
}

type RegionalBand = {
  name: string;
  minMhz: number;
  maxMhz: number;
};

const REGIONAL_BANDS: RegionalBand[] = [
  { name: "US915", minMhz: 902, maxMhz: 928 },
  { name: "EU868", minMhz: 863, maxMhz: 870 },
  { name: "AU915", minMhz: 915, maxMhz: 928 },
  { name: "AS923", minMhz: 923, maxMhz: 925 },
  { name: "KR920", minMhz: 920, maxMhz: 923 },
  { name: "IN865", minMhz: 865, maxMhz: 867 },
  { name: "RU864", minMhz: 864, maxMhz: 870 },
];

function getRegionalBand(freqMhz: number): RegionalBand | null {
  return REGIONAL_BANDS.find((band) => freqMhz >= band.minMhz && freqMhz <= band.maxMhz) ?? null;
}

function isRadioDirty(form: RadioConfig, persisted: RadioConfig): boolean {
  return (
    form.txPowerDbm !== persisted.txPowerDbm ||
    form.sf !== persisted.sf ||
    form.bwKhz !== persisted.bwKhz ||
    form.cr !== persisted.cr ||
    form.freqMhz !== persisted.freqMhz
  );
}

export function RadioForm({ radio }: RadioFormProps) {
  const [persisted, setPersisted] = useState<RadioConfig>(radio);
  const [form, setForm] = useState<RadioConfig>(radio);
  const [saving, setSaving] = useState(false);
  const [saved, flashSaved, resetSaved] = useTimedFlag(2000);
  const [error, setError] = useState("");

  useEffect(() => {
    setPersisted(radio);
    setForm(radio);
  }, [radio]);

  const dirty = useMemo(() => isRadioDirty(form, persisted), [form, persisted]);
  const regionalBand = getRegionalBand(form.freqMhz);
  const isOutOfRegionalBand = !regionalBand;
  const regionalBandText = regionalBand
    ? `${regionalBand.name} ${regionalBand.minMhz}–${regionalBand.maxMhz} MHz`
    : "Out of known regional ISM ranges";

  const handleSave = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!dirty) return;

    setSaving(true);
    setError("");
    resetSaved();
    try {
      await saveRadio(form);
      setPersisted(form);
      flashSaved();
    } catch (err) {
      setError(friendlyErrorFrom(err));
    } finally {
      setSaving(false);
    }
  };

  const handleRevert = () => {
    setForm(persisted);
    setError("");
    resetSaved();
  };

  return (
    <form className={styles.form} onSubmit={handleSave}>
      <div className={styles.formHeader}>
        <span className={styles.formTitle}>Radio Settings</span>
        {dirty && <span className={styles.dirtyBadge}>Unsaved changes</span>}
      </div>

      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="txPower">
          TX Power
        </label>
        <input
          id="txPower"
          className={styles.slider}
          type="range"
          min={2}
          max={22}
          step={1}
          value={form.txPowerDbm}
          onChange={(e) => setForm((f) => ({ ...f, txPowerDbm: Number(e.target.value) }))}
          aria-label="TX Power in dBm"
        />
        <span className={styles.sliderValue}>{form.txPowerDbm} dBm</span>
      </div>

      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="sf">
          Spreading Factor
          {form.sf !== persisted.sf && <span className={styles.changedPill}>changed</span>}
        </label>
        <select
          id="sf"
          className={styles.select}
          value={form.sf}
          onChange={(e) => setForm((f) => ({ ...f, sf: Number(e.target.value) as RadioConfig["sf"] }))}
        >
          {([7, 8, 9, 10, 11, 12] as const).map((n) => (
            <option key={n} value={n}>
              SF{n}
            </option>
          ))}
        </select>
      </div>

      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="bw">
          Bandwidth
          {form.bwKhz !== persisted.bwKhz && <span className={styles.changedPill}>changed</span>}
        </label>
        <select
          id="bw"
          className={styles.select}
          value={form.bwKhz}
          onChange={(e) => setForm((f) => ({ ...f, bwKhz: Number(e.target.value) as RadioConfig["bwKhz"] }))}
        >
          {([125, 250, 500] as const).map((n) => (
            <option key={n} value={n}>
              {n} kHz
            </option>
          ))}
        </select>
      </div>

      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="cr">
          Coding Rate
        </label>
        <select
          id="cr"
          className={styles.select}
          value={form.cr}
          onChange={(e) => setForm((f) => ({ ...f, cr: Number(e.target.value) as RadioConfig["cr"] }))}
        >
          {([5, 6, 7, 8] as const).map((n) => (
            <option key={n} value={n}>
              4/{n}
            </option>
          ))}
        </select>
      </div>

      <div className={styles.field}>
        <label className={styles.fieldLabel} htmlFor="freq">
          Frequency
          {form.freqMhz !== persisted.freqMhz && <span className={styles.changedPill}>changed</span>}
        </label>
        <input
          id="freq"
          className={`${styles.input} ${isOutOfRegionalBand ? styles.inputWarning : ""}`}
          type="number"
          step="0.1"
          min={137}
          max={1020}
          value={form.freqMhz}
          onChange={(e) => setForm((f) => ({ ...f, freqMhz: Number(e.target.value) }))}
          aria-label="Frequency in MHz"
          aria-invalid={isOutOfRegionalBand || undefined}
        />
        <span style={{ color: "var(--text-muted)", fontSize: "0.85rem" }}>MHz</span>
      </div>
      <div className={styles.fieldHintWrap}>
        <span className={styles.hintText}>Regional band: {regionalBandText}</span>
        {isOutOfRegionalBand && (
          <span className={styles.warningText}>
            This frequency is outside common regional ISM bands. Advanced/custom use may still be intentional.
          </span>
        )}
      </div>

      <div className={styles.actions}>
        <button className={styles.saveBtn} type="submit" disabled={saving || !dirty}>
          {saving ? "Saving…" : "Save Radio Settings"}
        </button>
        <button className={styles.revertBtn} type="button" onClick={handleRevert} disabled={saving || !dirty}>
          Revert
        </button>
        {saved && <span className={styles.savedMsg}>✓ Saved</span>}
        {error && <span className={styles.error}>{error}</span>}
      </div>
    </form>
  );
}
