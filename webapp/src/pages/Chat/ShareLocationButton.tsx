import { IconLocation } from '../../components/Icons';
import styles from './ShareLocationButton.module.css';

export type LocationAttach = 'off' | 'zone' | 'exact';

interface ShareLocationToggleProps {
  value: LocationAttach;
  onChange: (v: LocationAttach) => void;
}

const OPTIONS: { value: LocationAttach; label: string; title: string }[] = [
  { value: 'off',   label: 'Off',   title: 'Off: no location shared' },
  { value: 'zone',  label: 'Zone',  title: 'Coarse: grid square (~1km zone)' },
  { value: 'exact', label: 'Exact', title: 'Exact: precise GPS coordinates' },
];

export function ShareLocationToggle({ value, onChange }: ShareLocationToggleProps) {
  return (
    <div className={styles.toggle} role="group" aria-label="Attach location">
      <IconLocation size={14} />
      {OPTIONS.map(opt => (
        <button
          key={opt.value}
          className={`${styles.option} ${value === opt.value ? styles.active : ''} ${opt.value === 'exact' && value === 'exact' ? styles.exactActive : ''}`}
          onClick={() => onChange(opt.value)}
          title={opt.title}
          aria-pressed={value === opt.value}
        >
          {opt.label}
        </button>
      ))}
    </div>
  );
}
