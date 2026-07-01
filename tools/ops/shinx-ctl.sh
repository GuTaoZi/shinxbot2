#!/bin/bash
# shinx-ctl.sh — Claude-managed lifecycle/ops control for shinxbot2.
#
# Wraps the existing tmux setup (sessions `bot` and `llbot`) with deterministic
# verbs so both humans and Claude sessions can build, inspect, and health-check
# the bot the same way. It does NOT change how the bot runs — the bot binary's
# own fork-loop (main.cpp bot_run) still auto-restarts the child on a crash.
#
# Usage: tools/ops/shinx-ctl.sh <command> [args]
#
#   build [main|simple] [sanitize]   Build the framework (-> ./build/shinxbot)
#   start                            Launch bot in tmux session `bot`
#   stop                             Stop bot (SIGINT to the fork-loop parent)
#   restart                          stop + start (needed to pick up a NEW build)
#   deploy [main|simple]             build + restart in one shot
#   status                           Sessions, PIDs, uptime, ports
#   logs [N]                         Tail today's info.log/erro.log (default 40)
#   pane [N]                         Tail the live bot tmux pane (default 40)
#   health                           Full health check; exit 0 = healthy
#   qr                               Render the backend login QR to the terminal
#   login                            Show QR (if logged out) + poll until logged in
#   backend-status                   llbot backend session/process check
#   backend-logs [N]                 Tail the llbot tmux pane (default 40)
#   backend-restart                  Restart llbot (needed to refresh an expired QR)
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BOT_SESSION="bot"
BACKEND_SESSION="llbot"
BACKEND_DIR="$(cd "$ROOT/.." && pwd)/llbot"
BACKEND_START="./start.sh"
BOT_BIN="./build/shinxbot"
PORT_FILE="$ROOT/config/port.txt"
QR_FILE="$BACKEND_DIR/qrcode.png"
QR_RENDER="$ROOT/tools/ops/show-qr.py"

C_R='\033[0;31m'; C_G='\033[0;32m'; C_Y='\033[1;33m'; C_B='\033[0;36m'; C_N='\033[0m'
ok()   { echo -e "${C_G}[ ok ]${C_N} $*"; }
bad()  { echo -e "${C_R}[FAIL]${C_N} $*"; }
warn() { echo -e "${C_Y}[warn]${C_N} $*"; }
info() { echo -e "${C_B}[info]${C_N} $*"; }

# --- config parsing --------------------------------------------------------
# port.txt layout (see src/main.cpp): "<API/send_port> <event/recv_port> <token>"
read_ports() {
    API_PORT="" EVENT_PORT="" TOKEN=""
    if [ -f "$PORT_FILE" ]; then
        read -r API_PORT EVENT_PORT TOKEN < "$PORT_FILE"
    fi
    : "${API_PORT:=3000}" "${EVENT_PORT:=3001}" "${TOKEN:=}"
}

# --- process/session helpers ----------------------------------------------
session_exists() { tmux has-session -t "$1" 2>/dev/null; }
bot_pids()       { pgrep -f "$BOT_BIN" 2>/dev/null; }
bot_running()    { [ -n "$(bot_pids)" ]; }

# most-recent per-day log dir for the live bot qq (from the running API)
today_log_dir() {
    local qq day
    qq="$(api_login_qq)"
    [ -z "$qq" ] && return 1
    day="$(date +%Y_%m_%d)"
    local d="$ROOT/log/$qq/$day"
    [ -d "$d" ] && { echo "$d"; return 0; }
    # fall back to newest day dir under that qq
    ls -1dt "$ROOT/log/$qq"/*/ 2>/dev/null | head -1
}

# query the OneBot API for the logged-in qq; empty on failure
api_login_qq() {
    read_ports
    curl -s -m 5 -H "Authorization: Bearer $TOKEN" \
        "http://127.0.0.1:$API_PORT/get_login_info" 2>/dev/null \
        | sed -n 's/.*"user_id":\([0-9]\+\).*/\1/p'
}

# --- commands --------------------------------------------------------------
cmd_build() {
    info "building ($1 ${2:-})"
    ( cd "$ROOT" && ./build.sh "$@" )
}

cmd_start() {
    if bot_running; then warn "bot already running (pids: $(bot_pids | tr '\n' ' '))"; return 0; fi
    if ! session_exists "$BOT_SESSION"; then
        info "creating tmux session '$BOT_SESSION'"
        tmux new-session -d -s "$BOT_SESSION" -c "$ROOT"
    fi
    info "launching $BOT_BIN in session '$BOT_SESSION'"
    tmux send-keys -t "$BOT_SESSION" "cd $ROOT && $BOT_BIN" C-m
    sleep 3
    if bot_running; then ok "bot started (pids: $(bot_pids | tr '\n' ' ')"; else bad "bot did not come up — check: $0 pane"; return 1; fi
}

cmd_stop() {
    if ! bot_running; then warn "bot not running"; return 0; fi
    info "stopping bot (SIGINT to fork-loop parent group)"
    # C-c on the pane signals the whole foreground process group -> kills the
    # fork-loop parent AND its child, so it will NOT respawn.
    tmux send-keys -t "$BOT_SESSION" C-c 2>/dev/null
    for _ in 1 2 3 4 5; do bot_running || break; sleep 1; done
    if bot_running; then
        warn "still up; sending SIGTERM to $(bot_pids | tr '\n' ' ')"
        pkill -TERM -f "$BOT_BIN"; sleep 2
    fi
    if bot_running; then bad "could not stop bot"; return 1; else ok "bot stopped"; fi
}

cmd_restart() { cmd_stop; cmd_start; }

cmd_deploy() { cmd_build "${1:-main}" && cmd_restart; }

cmd_status() {
    read_ports
    echo "root:        $ROOT"
    echo "ports:       API/send=$API_PORT  event/recv=$EVENT_PORT"
    echo -n "bot session: "; session_exists "$BOT_SESSION" && echo "up" || echo "MISSING"
    if bot_running; then
        ps -o pid,ppid,etime,pcpu,pmem,cmd -p "$(bot_pids | tr '\n' ',' | sed 's/,$//')" | sed 's/^/  /'
    else
        echo "  (no shinxbot process)"
    fi
    echo -n "llbot session: "; session_exists "$BACKEND_SESSION" && echo "up" || echo "MISSING"
    local qq; qq="$(api_login_qq)"
    [ -n "$qq" ] && echo "API login qq: $qq (reachable)" || echo "API: unreachable on :$API_PORT"
}

cmd_logs() {
    local n="${1:-40}" d; d="$(today_log_dir)" || { warn "no log dir (API unreachable?) — falling back to pane"; cmd_pane "$n"; return; }
    info "log dir: $d"
    for f in erro.log warn.log info.log; do
        [ -f "$d/$f" ] || continue
        echo "----- $f (last $n) -----"
        tail -n "$n" "$d/$f"
    done
}

cmd_pane()         { tmux capture-pane -pt "$BOT_SESSION" -S "-${1:-40}" 2>/dev/null || bad "session '$BOT_SESSION' not found"; }
cmd_backend_logs() { tmux capture-pane -pt "$BACKEND_SESSION" -S "-${1:-40}" 2>/dev/null || bad "session '$BACKEND_SESSION' not found"; }

cmd_backend_status() {
    echo -n "llbot session: "; session_exists "$BACKEND_SESSION" && echo "up" || { echo "MISSING"; return 1; }
    pgrep -af "llbot" | grep -v shinx | sed 's/^/  /' || warn "no llbot process found"
}

cmd_health() {
    local rc=0
    read_ports
    # 1. bot tmux session
    if session_exists "$BOT_SESSION"; then ok "tmux session '$BOT_SESSION' present"; else bad "tmux session '$BOT_SESSION' MISSING"; rc=1; fi
    # 2. bot process
    if bot_running; then ok "shinxbot process up (pids: $(bot_pids | tr '\n' ' '))"; else bad "shinxbot process DOWN"; rc=1; fi
    # 3. OneBot API reachable (proves bot<->backend link)
    local qq; qq="$(api_login_qq)"
    if [ -n "$qq" ]; then ok "OneBot API :$API_PORT reachable (login qq=$qq)"; else bad "OneBot API :$API_PORT not returning a login (backend down or logged out)"; rc=1; fi
    # 4. backend session
    if session_exists "$BACKEND_SESSION"; then
        ok "backend session '$BACKEND_SESSION' present"
        [ -z "$qq" ] && warn "backend up but no login — likely needs QR scan: run '$0 login'"
    else
        bad "backend session '$BACKEND_SESSION' MISSING"; rc=1
    fi
    # 5. today's error log
    local d; d="$(today_log_dir 2>/dev/null)"
    if [ -n "$d" ] && [ -s "$d/erro.log" ]; then
        warn "erro.log has content ($(wc -l <"$d/erro.log") lines) — tail:"
        tail -n 10 "$d/erro.log" | sed 's/^/    /'
    elif [ -n "$d" ]; then
        ok "erro.log clean for today"
    fi
    # 6. CPU sanity (fork-loop busy-spin has bitten this bot before)
    if bot_running; then
        local cpu; cpu="$(ps -o %cpu= -p "$(bot_pids | tr '\n' ',' | sed 's/,$//')" | awk '{s+=$1} END{printf "%.0f", s}')"
        [ "${cpu:-0}" -ge 150 ] && warn "high CPU: ${cpu}% (possible busy-loop)" || info "cpu: ${cpu:-?}%"
    fi
    [ "$rc" -eq 0 ] && echo -e "\n${C_G}==> HEALTHY${C_N}" || echo -e "\n${C_R}==> UNHEALTHY${C_N}"
    return "$rc"
}

# llbot prints "二维码网址: <url>" to its pane at login — grab the newest one.
qr_url_from_pane() {
    tmux capture-pane -pt "$BACKEND_SESSION" -S -2000 2>/dev/null \
        | grep -aoE '二维码网址: *https?://[^ ]+' | tail -1 | sed 's/^二维码网址: *//'
}

cmd_qr() {
    local url; url="$(qr_url_from_pane)"
    [ -n "$url" ] && info "QR payload URL (open on another device if needed):" && echo "    $url"
    if [ ! -f "$QR_FILE" ]; then
        [ -n "$url" ] && { warn "no $QR_FILE, but llbot also renders its own QR in the pane: $0 backend-logs 40"; return 0; }
        bad "no QR file at $QR_FILE (backend may already be logged in, or hasn't produced one)"; return 1
    fi
    local age; age=$(( $(date +%s) - $(stat -c %Y "$QR_FILE") ))
    info "QR image: $QR_FILE (updated ${age}s ago)"
    [ "$age" -gt 300 ] && warn "QR is stale (>5min) — it may be expired. Use '$0 backend-restart' to refresh it."
    python3 "$QR_RENDER" "$QR_FILE" || return 1
}

cmd_login() {
    local qq; qq="$(api_login_qq)"
    if [ -n "$qq" ]; then ok "already logged in (qq=$qq) — nothing to do"; return 0; fi
    warn "not logged in — bringing up the QR"
    if ! session_exists "$BACKEND_SESSION"; then
        bad "backend session '$BACKEND_SESSION' is down; start it first (cd $BACKEND_DIR && $BACKEND_START), then re-run '$0 login'"
        return 1
    fi
    cmd_qr || warn "could not render QR file; check the backend pane: $0 backend-logs"
    echo; info "Scan the QR above with the target QQ's mobile app. Waiting for login (up to 180s)…"
    for i in $(seq 1 60); do
        sleep 3
        qq="$(api_login_qq)"
        if [ -n "$qq" ]; then echo; ok "logged in as qq=$qq"; return 0; fi
        printf "\r  waiting… %ds" "$(( i * 3 ))"
    done
    echo; bad "still not logged in after 180s — QR may have expired; try '$0 backend-restart' then '$0 login' again"
    return 1
}

# Direct relaunch (no sudo). start.sh is only needed for first-time dependency
# install; on an already-provisioned host we launch the llbot CLI directly, which
# matches the running process (`xvfb-run -a ./llbot`) and avoids start.sh's sudo -v.
cmd_backend_restart() {
    if ! session_exists "$BACKEND_SESSION"; then
        info "creating tmux session '$BACKEND_SESSION'"
        tmux new-session -d -s "$BACKEND_SESSION" -c "$BACKEND_DIR"
    else
        warn "restarting backend (this drops the current QQ connection; re-login needs a QR scan)"
        tmux send-keys -t "$BACKEND_SESSION" C-c 2>/dev/null
        for _ in $(seq 1 20); do pgrep -f "bin/pmhq/pmhq" >/dev/null || break; sleep 1; done
        pgrep -f "$BACKEND_DIR/bin" >/dev/null && { pkill -f "$BACKEND_DIR/bin"; sleep 2; }
    fi
    tmux send-keys -t "$BACKEND_SESSION" "cd $BACKEND_DIR && xvfb-run -a ./llbot" C-m
    info "backend (re)start issued; a fresh QR appears in a few seconds — run '$0 login' to scan it"
}

# --- dispatch --------------------------------------------------------------
cmd="${1:-}"; shift 2>/dev/null || true
case "$cmd" in
    build)          cmd_build "${1:-main}" "${2:-}" ;;
    start)          cmd_start ;;
    stop)           cmd_stop ;;
    restart)        cmd_restart ;;
    deploy)         cmd_deploy "${1:-main}" ;;
    status)         cmd_status ;;
    logs)           cmd_logs "${1:-40}" ;;
    pane)           cmd_pane "${1:-40}" ;;
    health)         cmd_health ;;
    qr)             cmd_qr ;;
    login)          cmd_login ;;
    backend-status) cmd_backend_status ;;
    backend-logs)   cmd_backend_logs "${1:-40}" ;;
    backend-restart) cmd_backend_restart ;;
    *) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
