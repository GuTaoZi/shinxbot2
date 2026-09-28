#!/bin/bash
# shinx-ha.sh — active/standby failover for shinxbot2 across two hosts.
#
# Only ONE host may hold the bot's QQ login at a time (a second Linux QQ login
# kicks the first), so this is active/standby, not active/active:
#   * the ACTIVE host runs NapCat + bot and pushes bot state (config/, resource/)
#     to the standby every few minutes;
#   * the BACKUP host's watchdog takes over if the primary stays unreachable /
#     logged out for SHINX_HA_THRESHOLD consecutive minutes;
#   * the PRIMARY host takes the bot back at boot (tools/ops/shinxbot.service).
#
# Host-local settings live in ~/.config/shinxbot/host.env (see host.env.example):
#   SHINX_HA_ROLE=primary|backup   SHINX_HA_PEER=<ssh alias>   SHINX_HA_PEER_ROOT=<path>
#
# Usage: tools/ops/shinx-ha.sh <command>
#   status          Where is the bot? (this host / peer: online|active|standby)
#   takeover        Move the bot HERE: release peer, pull its state, start locally
#   release         Stop bot + NapCat here (frees the QQ login)
#   sync            Push state to the peer (only when active here and peer is not)
#   code-push       Push source to the peer, rebuild there (restart if it's active)
#   boot            Machine-boot entry: primary -> takeover; backup -> nothing
#   tick            Cron entry (every minute): periodic sync / watchdog / split-brain fix
#   pause | resume  Disable / re-enable the automatic tick on this host
#   log [N]         Tail the HA log
#
# Transfers show an rsync progress line on a terminal; set SHINX_HA_PROGRESS=1
# to get it in non-interactive runs too (e.g. `... code-push > f; tail -c 200 f`).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CTL="$ROOT/tools/ops/shinx-ctl.sh"
HOST_ENV="${SHINX_HOST_ENV:-$HOME/.config/shinxbot/host.env}"
# shellcheck disable=SC1090
[ -f "$HOST_ENV" ] && . "$HOST_ENV"
ROLE="${SHINX_HA_ROLE:-primary}"
PEER="${SHINX_HA_PEER:-}"
PEER_ROOT="${SHINX_HA_PEER_ROOT:-}"
THRESHOLD="${SHINX_HA_THRESHOLD:-5}"          # consecutive bad minutes before backup takes over
SYNC_EVERY="${SHINX_HA_SYNC_EVERY:-300}"      # seconds between state pushes
# 0 = failover only: no periodic push, no state transfer on takeover/hand-back
# (each host keeps its own config/+resource/; `sync` still works by hand).
STATE_SYNC="${SHINX_HA_STATE_SYNC:-1}"
STATE_DIR="$HOME/.local/state/shinxbot"
LOG="$STATE_DIR/ha.log"
mkdir -p "$STATE_DIR"

C_R='\033[0;31m'; C_G='\033[0;32m'; C_Y='\033[1;33m'; C_B='\033[0;36m'; C_N='\033[0m'
ok()   { echo -e "${C_G}[ ok ]${C_N} $*"; }
bad()  { echo -e "${C_R}[FAIL]${C_N} $*"; }
warn() { echo -e "${C_Y}[warn]${C_N} $*"; }
info() { echo -e "${C_B}[info]${C_N} $*"; }
hlog() { echo "$(date '+%F %T') [$ROLE] $*" >> "$LOG"; }

[ -n "$PEER" ] && [ -n "$PEER_ROOT" ] || { bad "SHINX_HA_PEER / SHINX_HA_PEER_ROOT not set in $HOST_ENV"; exit 1; }

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
RSYNC=(rsync -a --partial --timeout=120 -e "ssh ${SSH_OPTS[*]}")
# Whole-transfer progress (bytes, %, rate, ETA) on a terminal, or in logs with
# SHINX_HA_PROGRESS=1 — the China<->US link is slow enough that you want to see it.
if [ -t 1 ] || [ "${SHINX_HA_PROGRESS:-0}" = 1 ]; then RSYNC+=(--info=progress2 --no-inc-recursive); fi
peer()     { timeout "${PEER_TIMEOUT:-60}" ssh "${SSH_OPTS[@]}" "$PEER" "$@"; }
peer_ha()  { peer "$PEER_ROOT/tools/ops/shinx-ha.sh" "$@"; }

# Serialize every mutating verb on this host (cron tick vs. a manual takeover).
with_lock() { exec 9>"$STATE_DIR/ha.lock"; flock -n 9 || { warn "another shinx-ha run holds the lock"; exit 3; }; }

# --- state probes ------------------------------------------------------------
# online  = backend logged in (OneBot get_login_info returns a qq)
# active  = bot or QQ process running (holds / is acquiring the login)
local_state() {
    if [ -n "$("$CTL" _login_qq 2>/dev/null)" ]; then echo online
    elif "$CTL" active >/dev/null 2>&1; then echo active
    else echo standby; fi
}
# Prints online|active|standby, or "unreachable" if SSH fails.
peer_state() {
    local s; s="$(PEER_TIMEOUT=40 peer_ha _state 2>/dev/null | tail -1)"
    case "$s" in online|active|standby) echo "$s" ;; *) echo unreachable ;; esac
}

# --- state transfer ----------------------------------------------------------
# config/ is the bot's mutable state (feature json: rua/bottle/bili/poke ...).
# resource/ is mostly static media plus a few generated files; no --delete there.
push_state() { # [--update]  (only overwrite files older on the peer)
    "${RSYNC[@]}" "$@" --delete "$ROOT/config/" "$PEER:$PEER_ROOT/config/" &&
    "${RSYNC[@]}" "$@" "$ROOT/resource/" "$PEER:$PEER_ROOT/resource/"
}
pull_state() {
    "${RSYNC[@]}" --delete "$PEER:$PEER_ROOT/config/" "$ROOT/config/" &&
    "${RSYNC[@]}" "$PEER:$PEER_ROOT/resource/" "$ROOT/resource/"
}

# --- verbs ---------------------------------------------------------------------
cmd_status() {
    echo "this host ($(hostname), role=$ROLE): $(local_state)"
    echo "peer ($PEER): $(peer_state)"
    [ -f "$STATE_DIR/paused" ] && warn "automatic tick is PAUSED on this host"
    [ "$STATE_SYNC" = 1 ] || info "state sync OFF (failover only; run 'sync' by hand to copy state)"
    [ -f "$STATE_DIR/last_sync" ] && info "last state push: $(date -d "@$(cat "$STATE_DIR/last_sync")" '+%F %T')"
    return 0
}

cmd_release() {
    "$CTL" stop; "$CTL" backend-stop
    hlog "released (bot + NapCat stopped)"
}

cmd_sync() {
    [ "$(local_state)" = standby ] && { warn "not active here — nothing to push"; return 1; }
    local ps; ps="$(peer_state)"
    case "$ps" in
        unreachable) warn "peer unreachable — skipping sync"; return 1 ;;
        online|active) bad "peer is also $ps (split brain) — refusing to push"; hlog "sync refused: peer $ps"; return 2 ;;
    esac
    if push_state; then date +%s > "$STATE_DIR/last_sync"; ok "state pushed to $PEER"; else bad "state push failed"; hlog "sync failed"; return 1; fi
}

# takeover [--no-wait]: --no-wait starts NapCat+bot in the background (watchdog use).
cmd_takeover() {
    local ps; ps="$(peer_state)"
    # At boot the network may lag: don't mistake "no route yet" for "peer dead".
    if [ "$ps" = unreachable ] && [ "${BOOTING:-0}" = 1 ]; then
        for _ in $(seq 1 8); do sleep 15; ps="$(peer_state)"; [ "$ps" != unreachable ] && break; done
    fi
    case "$ps" in
        online|active)
            info "peer is $ps — releasing it"
            peer_ha release >/dev/null 2>&1 || { bad "peer release failed — aborting to avoid split brain"; hlog "takeover aborted: peer release failed"; return 1; }
            if [ "$STATE_SYNC" = 1 ]; then
                info "pulling latest state from $PEER"
                pull_state || warn "state pull failed — continuing with local state"
            fi
            ;;
        standby) info "peer is standby — keeping local state" ;;
        unreachable) warn "peer unreachable — using local state (as of last sync)" ;;
    esac
    hlog "takeover (peer was $ps)"
    if [ "${1:-}" = "--no-wait" ]; then
        setsid nohup "$CTL" boot >> "$LOG" 2>&1 < /dev/null &
        info "boot launched in background (log: $LOG)"
    else
        "$CTL" boot
    fi
}

cmd_boot() {
    if [ "$ROLE" = primary ]; then BOOTING=1 cmd_takeover; else info "backup host: boot does nothing (watchdog decides)"; fi
}

cmd_code_push() {
    info "pushing source tree to $PEER:$PEER_ROOT"
    "${RSYNC[@]}" --delete \
        --exclude='/config/' --exclude='/resource/' --exclude='/log/' --exclude='/backup/' \
        --exclude='/build/' --exclude='/plugins/functions/*/build/' --exclude='/plugins/events/*/build/' \
        --exclude='/plugins/lib/functions/*.so' --exclude='/plugins/lib/events/*.so' --exclude='/lib/libutils.so' \
        --exclude='/plugins/lib/functions/.hot/' --exclude='/.claude/' --exclude='__pycache__/' \
        --exclude='*.jpeg' --exclude='*.tmp' --exclude='/qq.location' \
        "$ROOT/" "$PEER:$PEER_ROOT/" || { bad "rsync failed"; return 1; }
    ok "source pushed; building on $PEER (framework + plugins)"
    PEER_TIMEOUT=1800 peer "cd $PEER_ROOT && ./build.sh main >/tmp/shx-build.log 2>&1 && \
        (cd plugins/functions && bash make_all.sh >/tmp/shx-plug-f.log 2>&1) && \
        (cd plugins/events && bash make_all.sh >/tmp/shx-plug-e.log 2>&1) && \
        ! grep -l 'error:' /tmp/shx-build.log /tmp/shx-plug-f.log /tmp/shx-plug-e.log" \
        || { bad "build failed on $PEER — see /tmp/shx-build.log, /tmp/shx-plug-*.log there"; return 1; }
    ok "build ok on $PEER"
    if [ "$(peer_state)" != standby ]; then info "peer is active — restarting its bot"; peer "$PEER_ROOT/tools/ops/shinx-ctl.sh restart"; fi
    hlog "code-push to $PEER ok"
}

# Cron entry. Rules (this host's point of view):
#  * active/online here:
#      - backup AND peer online  -> primary is back: push newer state, release here
#      - primary, logged out, peer online for a while -> we got kicked: release here
#      - otherwise push state every SYNC_EVERY seconds
#  * standby here AND backup: watchdog — peer unreachable or not online for
#    THRESHOLD consecutive ticks -> take over.
cmd_tick() {
    [ -f "$STATE_DIR/paused" ] && exit 0
    local ls ps fails; ls="$(local_state)"; ps="$(peer_state)"
    if [ "$ls" != standby ]; then
        echo 0 > "$STATE_DIR/fails"
        if [ "$ROLE" = backup ] && [ "$ps" = online ]; then
            hlog "primary is online again — handing back (release)"
            [ "$STATE_SYNC" = 1 ] && { push_state --update || hlog "hand-back state push failed"; }
            cmd_release; return
        fi
        if [ "$ROLE" = primary ] && [ "$ls" = active ] && [ "$ps" = online ]; then
            fails=$(( $(cat "$STATE_DIR/kicked" 2>/dev/null || echo 0) + 1 )); echo "$fails" > "$STATE_DIR/kicked"
            if [ "$fails" -ge "$THRESHOLD" ]; then hlog "logged out here while peer online for ${fails}m — releasing"; rm -f "$STATE_DIR/kicked"; cmd_release; fi
            return
        fi
        rm -f "$STATE_DIR/kicked"
        [ "$STATE_SYNC" = 1 ] || return 0
        local last; last="$(cat "$STATE_DIR/last_sync" 2>/dev/null || echo 0)"
        if [ $(( $(date +%s) - last )) -ge "$SYNC_EVERY" ] && [ "$ps" = standby ]; then
            push_state && date +%s > "$STATE_DIR/last_sync" || hlog "periodic sync failed"
        fi
        return
    fi
    [ "$ROLE" = backup ] || return 0
    if [ "$ps" = online ]; then echo 0 > "$STATE_DIR/fails"; return; fi
    fails=$(( $(cat "$STATE_DIR/fails" 2>/dev/null || echo 0) + 1 )); echo "$fails" > "$STATE_DIR/fails"
    hlog "watchdog: peer $ps ($fails/$THRESHOLD)"
    if [ "$fails" -ge "$THRESHOLD" ]; then
        echo 0 > "$STATE_DIR/fails"
        hlog "watchdog: taking over"
        cmd_takeover --no-wait >> "$LOG" 2>&1
    fi
}

cmd="${1:-status}"; shift || true
case "$cmd" in
    status)    cmd_status ;;
    takeover)  with_lock; cmd_takeover "$@" ;;
    release)   with_lock; cmd_release ;;
    sync)      with_lock; cmd_sync ;;
    code-push) with_lock; cmd_code_push ;;
    boot)      with_lock; cmd_boot ;;
    tick)      with_lock; cmd_tick ;;
    pause)     touch "$STATE_DIR/paused"; ok "automatic tick paused on this host" ;;
    resume)    rm -f "$STATE_DIR/paused"; ok "automatic tick resumed" ;;
    log)       tail -n "${1:-40}" "$LOG" ;;
    _state)    local_state ;;
    *) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
