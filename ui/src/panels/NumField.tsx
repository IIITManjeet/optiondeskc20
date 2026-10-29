import { useEffect, useState } from "react";

/**
 * A number input that keeps what's typed (so "0." or "-" survive mid-edit) and
 * reports only parseable values. `scale` converts for display: 100 shows a decimal
 * vol as %. Follows `value` when it changes from outside (loading from the Desk).
 */
export function NumField({
  label,
  value,
  onChange,
  step,
  scale = 1,
  digits = 6,
  unit,
}: {
  label: string;
  value: number;
  onChange: (v: number) => void;
  step: number;
  scale?: number;
  digits?: number;
  unit?: string;
}) {
  const shown = (v: number) => String(+(v * scale).toFixed(digits));
  const [text, setText] = useState(() => shown(value));
  useEffect(() => {
    if (parseFloat(text) / scale !== value) setText(shown(value));
  }, [value]); // only an outside change resyncs the text
  return (
    <label className="field">
      <span>{label}</span>
      <input
        className="num"
        type="number"
        step={step}
        value={text}
        onChange={(e) => {
          setText(e.target.value);
          const v = parseFloat(e.target.value);
          if (Number.isFinite(v)) onChange(v / scale);
        }}
      />
      {unit && <em>{unit}</em>}
    </label>
  );
}
