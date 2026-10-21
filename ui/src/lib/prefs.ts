// UI preferences in localStorage. Storage can be missing or throw (private mode,
// blocked site data), so every access is guarded and falls back to the default.

import { useCallback, useState } from "react";

export function usePref<T extends string>(key: string, fallback: T): [T, (v: T) => void];
export function usePref(key: string, fallback: string | null): [string | null, (v: string) => void];
export function usePref(key: string, fallback: string | null) {
  const full = `od.${key}`;
  const [value, setValue] = useState<string | null>(() => {
    try {
      return localStorage.getItem(full) ?? fallback;
    } catch {
      return fallback;
    }
  });
  const set = useCallback(
    (v: string) => {
      setValue(v);
      try {
        localStorage.setItem(full, v);
      } catch {
        // not persisted; fine
      }
    },
    [full],
  );
  return [value, set] as const;
}
