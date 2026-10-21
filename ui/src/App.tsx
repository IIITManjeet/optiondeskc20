import { NavLink, Navigate, Route, Routes } from "react-router-dom";
import { Desk } from "./pages/Desk";

// Pages beyond the Desk arrive in later phases (docs/UI.md); they're listed
// disabled so the layout is final from the start.
const PAGES: { to: string; label: string; ready: boolean }[] = [
  { to: "/", label: "Desk", ready: true },
  { to: "/pricer", label: "Pricer", ready: false },
  { to: "/research", label: "Research", ready: false },
  { to: "/mm", label: "Market maker", ready: false },
  { to: "/orders", label: "Orders", ready: false },
  { to: "/configs", label: "Configs", ready: false },
  { to: "/history", label: "History", ready: false },
  { to: "/system", label: "System", ready: false },
];

export function App() {
  return (
    <div className="app">
      <nav className="sidebar">
        <div className="brand">
          options<b>desk</b>
        </div>
        {PAGES.map((p) =>
          p.ready ? (
            <NavLink key={p.to} to={p.to} end className={({ isActive }) => (isActive ? "nav on" : "nav")}>
              {p.label}
            </NavLink>
          ) : (
            <span key={p.to} className="nav off" title="coming in a later phase">
              {p.label}
            </span>
          ),
        )}
      </nav>
      <main className="content">
        <Routes>
          <Route path="/" element={<Desk />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </main>
    </div>
  );
}
