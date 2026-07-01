# shinxbot2 — operations runbook

C++17 OneBot11 QQ bot **framework**. Plugins (`.so`) live in a separate repo,
symlinked at `plugins/lib/{functions,events}` → `lib/{functions,events}`.

## Runtime topology

Two long-lived tmux sessions (already running; don't recreate blindly):

- **`llbot`** — OneBot11 backend (LLBot-CLI under `xvfb-run`), started by
  `../llbot/start.sh`. Posts events to `127.0.0.1:3001`, serves the API on `3000`.
- **`bot`** — `./build/shinxbot`, started from this repo root.

Ports/token come from `config/port.txt` (`<API/send=3000> <event/recv=3001> <token>`).
The bot is an HTTP **server on 3001** (receives events) and **POSTs API calls to 3000**
(auth header `Authorization: Bearer <token>`).

**Self-healing:** `src/main.cpp` `bot_run()` forks a child that runs the bot and
`waitpid`s it — on a *crash* the parent re-forks the **same binary**. So process
crashes recover on their own; you do **not** restart on a crash. But a **new build
is only picked up after a full stop+start** (the fork-loop keeps the old binary
alive otherwise) — use `restart`/`deploy` for that.

## Control script — `tools/ops/shinx-ctl.sh`

Prefer these verbs over ad-hoc tmux/ps commands:

| Verb | Purpose |
|------|---------|
| `health` | Full check (session→process→API→backend→erro.log→CPU); exit 0 = healthy |
| `status` | Sessions, PIDs, uptime, ports, login qq |
| `logs [N]` | Tail today's `erro.log`/`warn.log`/`info.log` (per-qq daily dir) |
| `pane [N]` | Tail the live `bot` tmux pane (stdout) |
| `build [main\|simple] [sanitize]` | `./build.sh` → `./build/shinxbot` |
| `stop` / `start` / `restart` | Lifecycle (stop = SIGINT to fork-loop group) |
| `deploy [main\|simple]` | `build` + `restart` (to ship a new binary) |
| `qr` / `login` | Render the backend login QR / show it + poll until logged in |
| `backend-status` / `backend-logs [N]` / `backend-restart` | Inspect / restart the `llbot` backend |

`build simple` skips rebuilding `libutils`; plain `build`/`main` builds everything.

## Backend login (QR) — the one manual step

llbot login requires **scanning a QR with the target QQ's phone**; this cannot be
automated. The workflow's job is to detect the logged-out state, surface the QR in
the terminal fast, and confirm re-login:

**Every login requires a fresh QR scan** — this setup has no device-trust persistence
(scanning invalidates the previous session server-side), so a restart never
auto-relogins. Plan for a human scan on every backend restart.

- **Detect:** `get_login_info` returns no `user_id` while the `llbot` session is up
  → logged out. `health` flags this and points at `login`.
- **Show:** `tools/ops/show-qr.py` decodes `~/llbot/qrcode.png` (1-bit grayscale PNG)
  with pure-stdlib zlib — no Pillow — and prints it as ANSI half-blocks that a phone
  can scan straight from the terminal. `qr` also warns if the PNG is >5 min old (stale/expired).
- **Refresh:** if the QR is expired, `backend-restart` bounces llbot so it writes a
  fresh `qrcode.png`. It relaunches directly (`cd ~/llbot && xvfb-run -a ./llbot`) —
  **no sudo**; `start.sh` is only for first-time dependency install. llbot also
  auto-regenerates an expired QR on its own every ~2–3 min while waiting.
- **Full flow:** `tools/ops/shinx-ctl.sh login` → shows the QR, then polls
  `get_login_info` every 3 s (up to 180 s) and reports the qq once login lands.

For a guaranteed-scannable render, run `qr`/`login` in a real terminal (ANSI color),
e.g. via the `!` prefix or directly in your shell. Because the scan is human, this
step stays interactive — Claude drives everything around it (detect → render → confirm).

## Logs

`log/<botqq>/YYYY_MM_DD/{erro,warn,info}.log` (botqq = `3664637421`), rotated daily.
`erro.log` empty = healthy. The live tmux pane also shows INFO stdout.

## Operator commands (in QQ chat, from an op in `config/core/op_list.json`)

`bot.on`/`bot.off`, `bot.reload|load|unload <name|all>`, `bot.backup`,
`bot.list_module`, `bot.block|unblock|white|unwhite`, `bot.module.*`.
Prefer these for plugin reloads — no framework restart needed.

## On-demand monitoring (this is the "Claude-managed" loop)

When asked to check on the bot:
1. `tools/ops/shinx-ctl.sh health` → if healthy, report and stop.
2. If unhealthy, diagnose (don't blindly restart — the fork-loop already handles
   crashes): read `logs`/`pane`, `backend-status`, check API reachability.
3. Report root cause + a recommended fix. Only stop/restart/deploy or edit code
   with explicit approval.

## Known issue

The `bot` child process runs at **~300% CPU sustained** (visible in `status`/`health`).
Likely a busy-loop in the network/event path — investigate separately; not caused
by this ops tooling.

## Build deps

openssl-dev, libjsoncpp, libzip, ImageMagick++ 7+, libmagick++-dev, libfmt-dev 8+
(see `Dockerfile`).
