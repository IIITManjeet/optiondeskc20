import type { ReactNode } from "react";

/** Titled box. `dim` greys the content when what it shows isn't live. */
export function Panel({
  title,
  right,
  dim,
  className,
  children,
}: {
  title: ReactNode;
  right?: ReactNode;
  dim?: boolean;
  className?: string;
  children: ReactNode;
}) {
  return (
    <section className={`panel ${className ?? ""} ${dim ? "dim" : ""}`}>
      <header className="panel-head">
        <h2>{title}</h2>
        {right && <div className="panel-right">{right}</div>}
      </header>
      <div className="panel-body">{children}</div>
    </section>
  );
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="empty">{children}</div>;
}
