import { NavLink, Navigate, Route, Routes } from "react-router-dom";
import { About } from "./pages/About";
import { Desk } from "./pages/Desk";
import { Pricer } from "./pages/Pricer";

const DEMO = import.meta.env.MODE === "demo";
// The demo lands on About (docs/UI.md, Flows); the console lands on the Desk.
const DESK_PATH = DEMO ? "/desk" : "/";

// Pages not built yet arrive in later phases (docs/UI.md); they're listed disabled
// so the layout is final from the start. The demo build lists only its own pages.
const PAGES: { to: string; label: string; ready: boolean; demo: boolean; console: boolean }[] = [
  { to: "/", label: "About", ready: true, demo: true, console: false },
  { to: DESK_PATH, label: "Desk", ready: true, demo: true, console: true },
  { to: "/pricer", label: "Pricer", ready: true, demo: true, console: true },
  { to: "/research", label: "Research", ready: false, demo: true, console: true },
  { to: "/mm", label: "Market maker", ready: false, demo: false, console: true },
  { to: "/orders", label: "Orders", ready: false, demo: false, console: true },
  { to: "/configs", label: "Configs", ready: false, demo: false, console: true },
  { to: "/history", label: "History", ready: false, demo: false, console: true },
  { to: "/system", label: "System", ready: false, demo: false, console: true },
];

export function App() {
  return (
    <div className="app">
      <nav className="sidebar">
        <div className="brand">
          options<b>desk</b>
        </div>
        {PAGES.filter((p) => (DEMO ? p.demo : p.console)).map((p) =>
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
          {DEMO && <Route path="/" element={<About />} />}
          <Route path={DESK_PATH} element={<Desk />} />
          <Route path="/pricer" element={<Pricer />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </main>
    </div>
  );
}
