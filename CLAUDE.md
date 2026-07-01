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
| `op <words...>` | Inject any operator command as an op (no QQ needed) |
| `reload [name\|all]` / `swap <name>` | Plugin `reload()` hook / hot-swap a rebuilt `.so` |
| `enable` / `disable` / `modules` | `bot.on` / `bot.off` / `bot.list_module` |

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

### Driving op commands from the shell (no QQ)

The bot dispatches operator commands from any event on its receive port, gated
only by `is_op(user_id)`. `shinx-ctl.sh` injects a synthetic message from an op
(default = first id in `op_list.json`; override `SHINX_OP_QQ`) — same mechanism
as `tools/dev_tools/sender.sh`. Reply routing by injected `message_type`:
`private` (default) → the bot DMs the op the reply in QQ; `internal`
(`SHINX_OP_SILENT=1`) → reply is a no-op at the backend, command still runs.

- `reload <name|all>` = `bot.reload` — calls the plugin's in-place `reload()`
  hook (config refresh). Does **not** reload the `.so` binary.
- `swap <name> [function|event]` = `bot.unload` + `bot.load` — dlclose/dlopen,
  the way to pick up a **freshly rebuilt** plugin `.so` without a full restart.
- Command results only return via chat (no stdout), so use `private` when you
  want to see the reply, or check `logs`/`pane` for side effects.

## On-demand monitoring (this is the "Claude-managed" loop)

When asked to check on the bot:
1. `tools/ops/shinx-ctl.sh health` → if healthy, report and stop.
2. If unhealthy, diagnose (don't blindly restart — the fork-loop already handles
   crashes): read `logs`/`pane`, `backend-status`, check API reachability.
3. Report root cause + a recommended fix. Only stop/restart/deploy or edit code
   with explicit approval.

## CPU cost (was ~315% sustained)

Two causes, root-caused via gdb thread stacks:
1. **AddressSanitizer leak (FIXED).** `build.sh` only ever *added* `-fsanitize=address`
   to the CMake cache and never cleared it, so once anyone ran a `sanitize` build,
   every later build (framework AND all plugins) stayed ASan-instrumented (~2-3x
   overhead). Fixed in `build.sh`; framework + all 41 plugins rebuilt clean. This
   removed ~90% CPU. **If you rebuild plugins, wipe their stale `build/` caches too.**
2. **biliget HTTPS polling (remaining ~220%, plugin-repo issue).** Hot threads sit in
   `biliget_http::safe_get_json` → `curl_easy_perform` → `OSSL_DECODER_from_bio` —
   i.e. a fresh TLS handshake per request (no curl handle/connection reuse), plus
   crypto churn. Fix lives in `shinxbot2-plugins/functions/biliget` (reuse a curl
   handle / share connections; consider fewer tracked UIDs or a longer
   `poll_interval_sec`). Not a framework bug.

## Build deps

openssl-dev, libjsoncpp, libzip, ImageMagick++ 7+, libmagick++-dev, libfmt-dev 8+
(see `Dockerfile`).
