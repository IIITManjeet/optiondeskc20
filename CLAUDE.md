# optionsdesk

C++20 options desk for Deribit, covering:
- live feed and shared-memory bus;
- SVI surface;
- risk;
- a paper/testnet market maker;
- record and replay;
- a web UI.

Start with `README.md`. `docs/ROADMAP.md` has the milestones, and `docs/UI.md`
has the web UI plan. The UI rules are in `ui/CLAUDE.md`.

## Build and run
- Build in WSL. Builds outside the source tree, in `~/build/optionsdesk`:
  `cmake --build ~/build/optionsdesk -j8`.
  - The dependencies are already downloaded. If reconfiguring fails while
    fetching them, run `cmake -DFETCHCONTENT_FULLY_DISCONNECTED=ON .` in the
    build dir.
- Tests: `~/build/optionsdesk/od_tests`.
- From Git Bash, prefix `wsl.exe` calls with `MSYS_NO_PATHCONV=1`, or use
  `sh -c '...'`.

## Rules
- Write recordings (`*.odj`) to `D:\optionsdesk-rec` (`/mnt/d/optionsdesk-rec`),
  never to `~` (the WSL disk lives on C:).
- Order-sending code may only reach Deribit testnet. Never add a path to the
  live exchange.
- Commit messages: a one-line summary in the imperative.
- Keep `README.md` and the docs in step with behaviour changes.
