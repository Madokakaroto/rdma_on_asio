#!/usr/bin/env bash
# Stage 8 comparison driver: per scenario case, run the RDMA-on-Asio lib bench
# (two-process) and linux-rdma/perftest side by side, then diff them.
#
# Pure orchestration -- it reuses the built C++ tools and adds no C++:
#   asio_perftest                    : the lib bench (server/client, reads --scenario)
#   rdma_benchmark_perftest_commands : emits perftest server/client command strings
#   rdma_benchmark_parse_perftest    : perftest stdout -> result JSON
#   rdma_benchmark_compare_results   : N result JSONs -> markdown table
#
# The lib server prints "RDMA_BENCH_READY role=server" so the client is gated on
# that marker (no blind sleep); perftest has no clean marker -> short fixed sleep.
#
# Single-host two-process only (loopback: --peer-addr defaults to --local-addr).
# Real two-box runs are Stage 14 (pass a distinct --peer-addr there).
#
# Phase 3 (managed ExternalProject) is intentionally not used: perftest is taken
# from --perftest-bin-dir or PATH.

set -u

# ---- defaults -------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$(realpath "$0")")" && pwd)"
BIN_DIR="$SCRIPT_DIR"
LOCAL_ADDR=""
PEER_ADDR=""
PERFTEST_BIN_DIR=""
OUTPUT_DIR="comparison-run"
BASE_PORT=18000
SLEEP_SERVER=1.0
DRY_RUN=0
declare -a SCENARIOS=()

die() { echo "fatal: $*" >&2; exit 1; }
log() { echo "[run_comparison] $*" >&2; }

usage() {
  cat >&2 <<'EOF'
Usage: run_comparison.sh --local-addr IP (--scenario FILE | --cases-dir DIR) [options]

  --scenario FILE        scenario JSON (repeatable)
  --cases-dir DIR        run every *.json in DIR
  --local-addr IP        RDMA local IP for lib + perftest servers (required)
  --peer-addr IP         address clients dial (default: --local-addr, loopback)
  --perftest-bin-dir DIR dir holding ib_* (default: PATH)
  --output-dir DIR       artifact root (default: comparison-run)
  --base-port N          first port; +10 per case (default: 18000)
  --bin-dir DIR          dir holding asio_perftest + rdma_benchmark_* (default: script dir)
  --sleep-server SEC     perftest server warmup sleep (default: 1.0)
  --dry-run              print constructed commands + resolved perftest, do not launch
  -h, --help             this help
EOF
}

# ---- arg parse ------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --scenario) SCENARIOS+=("$2"); shift 2;;
    --cases-dir)
      for f in "$2"/*.json; do [ -e "$f" ] && SCENARIOS+=("$f"); done; shift 2;;
    --local-addr) LOCAL_ADDR="$2"; shift 2;;
    --peer-addr) PEER_ADDR="$2"; shift 2;;
    --perftest-bin-dir) PERFTEST_BIN_DIR="$2"; shift 2;;
    --output-dir) OUTPUT_DIR="$2"; shift 2;;
    --base-port) BASE_PORT="$2"; shift 2;;
    --bin-dir) BIN_DIR="$2"; shift 2;;
    --sleep-server) SLEEP_SERVER="$2"; shift 2;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage; exit 0;;
    *) die "unknown argument: $1 (see --help)";;
  esac
done

[ -n "$LOCAL_ADDR" ] || { usage; die "--local-addr is required"; }
[ ${#SCENARIOS[@]} -gt 0 ] || { usage; die "no scenarios (--scenario / --cases-dir)"; }
[ -n "$PEER_ADDR" ] || PEER_ADDR="$LOCAL_ADDR"
command -v python3 >/dev/null 2>&1 || die "python3 is required (JSON extraction)"

ASIO="$BIN_DIR/asio_perftest"
PERFTEST_COMMANDS="$BIN_DIR/rdma_benchmark_perftest_commands"
PARSE_PERFTEST="$BIN_DIR/rdma_benchmark_parse_perftest"
COMPARE_RESULTS="$BIN_DIR/rdma_benchmark_compare_results"
for t in "$ASIO" "$PERFTEST_COMMANDS" "$PARSE_PERFTEST" "$COMPARE_RESULTS"; do
  [ -x "$t" ] || die "tool not found/executable: $t (build first, or pass --bin-dir)"
done

# stdbuf -oL forces the lib server's stdout line-buffered so its RDMA_BENCH_READY
# marker lands in the redirected log immediately; otherwise full-buffered stdout
# hides it and the ready-gate grep never sees it. Without stdbuf we fall back to a
# fixed warmup sleep (see run_case).
STDBUF=""
command -v stdbuf >/dev/null 2>&1 && STDBUF="stdbuf -oL"

# ---- helpers --------------------------------------------------------------
# json_get FILE KEY  -> value (empty if missing); strings unquoted.
json_get() {
  python3 -c '
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    v=d.get(sys.argv[2],"")
    print("" if v is None else v)
except Exception:
    print("")
' "$1" "$2"
}

# perftest tool name from operation + metric.
perftest_tool() { # op metric
  local base suf
  case "$1" in
    send_recv) base="ib_send";; write) base="ib_write";; read) base="ib_read";;
    *) base="";;
  esac
  case "$2" in bandwidth) suf="_bw";; latency) suf="_lat";; *) suf="";; esac
  echo "${base}${suf}"
}

# resolve_perftest TOOL -> prints absolute path on stdout, returns 1 if missing.
resolve_perftest() {
  local tool="$1" cand
  if [ -n "$PERFTEST_BIN_DIR" ]; then
    cand="$PERFTEST_BIN_DIR/$tool"
    [ -x "$cand" ] && { echo "$cand"; return 0; }
    return 1
  fi
  cand="$(command -v "$tool" 2>/dev/null)" && { echo "$cand"; return 0; }
  return 1
}

# emit_skip_json OUTFILE SCENARIO REASON  -- minimal perftest-side skip result.
emit_skip_json() {
  local out="$1" scen="$2" reason="$3"
  local op metric mode topo size qd
  op="$(json_get "$scen" operation)"; metric="$(json_get "$scen" metric)"
  mode="$(json_get "$scen" completion_mode)"; topo="$(json_get "$scen" topology)"
  size="$(json_get "$scen" message_size)"; qd="$(json_get "$scen" queue_depth)"
  cat > "$out" <<EOF
{
  "schema_version": "1",
  "baseline": "perftest",
  "backend": "perftest",
  "topology": "$topo",
  "operation": "$op",
  "metric": "$metric",
  "completion_mode": "$mode",
  "message_size_bytes": ${size:-0},
  "queue_depth": ${qd:-0},
  "throughput_gbit_s": 0,
  "latency_p50_us": null,
  "latency_p99_us": null,
  "skip_reason": "$reason",
  "first_error": ""
}
EOF
}

# wait_for_ready LOGFILE PID  -- returns 0 once the server marker appears.
wait_for_ready() {
  local logf="$1" pid="$2" start=$SECONDS
  while ! grep -q "RDMA_BENCH_READY role=server" "$logf" 2>/dev/null; do
    kill -0 "$pid" 2>/dev/null || return 1          # server exited early
    [ $((SECONDS - start)) -ge 15 ] && return 1      # deadline
    sleep 0.2
  done
  return 0
}

kill_wait() { # pid -- TERM, then KILL after ~3s, never block forever.
  local pid="$1" i
  kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; return 0; }
  kill "$pid" 2>/dev/null
  for i in $(seq 1 15); do
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; return 0; }
    sleep 0.2
  done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

# ---- version capture ------------------------------------------------------
mkdir -p "$OUTPUT_DIR"
VERSION_FILE="$OUTPUT_DIR/perftest_version.txt"
{
  probe="$(resolve_perftest ib_send_bw || true)"
  if [ -n "$probe" ]; then
    echo "ib_send_bw: $probe"
    "$probe" --version 2>&1 | head -3
  else
    echo "perftest: not found in ${PERFTEST_BIN_DIR:-PATH}"
  fi
} > "$VERSION_FILE" 2>&1
log "perftest version -> $VERSION_FILE"

SUMMARY="$OUTPUT_DIR/summary.md"
: > "$SUMMARY"

# ---- per-case runner ------------------------------------------------------
run_case() {
  local scen="$1" port="$2"
  [ -f "$scen" ] || { log "SKIP missing scenario: $scen"; return; }
  local name op metric mode
  name="$(json_get "$scen" name)"; [ -n "$name" ] || name="$(basename "$scen" .json)"
  op="$(json_get "$scen" operation)"
  metric="$(json_get "$scen" metric)"
  mode="$(json_get "$scen" completion_mode)"

  local case_dir="$OUTPUT_DIR/$name"
  mkdir -p "$case_dir"
  log "=== case $name (op=$op metric=$metric mode=$mode port=$port) ==="

  local lib_json="$case_dir/lib_${name}.json"
  local srv_log="$case_dir/lib.server.stdout.log"
  local srv_err="$case_dir/lib.server.stderr.log"
  local cli_log="$case_dir/lib.client.stdout.log"
  local cli_err="$case_dir/lib.client.stderr.log"

  # lib bench command lines (scenario supplies op/metric/mode/token/topology/size/iters/qd).
  local lib_server="$ASIO --server --local-addr $LOCAL_ADDR --scenario $scen --port $port"
  local lib_client="$ASIO --client $PEER_ADDR --scenario $scen --port $port --json-out $lib_json"

  # perftest resolve + commands.
  local tool tool_path
  tool="$(perftest_tool "$op" "$metric")"
  local cmds_json="$case_dir/perftest_commands.json"
  local pf_json="$case_dir/perftest_${name}.json"
  local pf_srv_log="$case_dir/perftest.server.stdout.log"
  local pf_cli_log="$case_dir/perftest.client.stdout.log"

  if [ "$DRY_RUN" = "1" ]; then
    echo "--- $name ---"
    echo "  lib server : ${STDBUF:+$STDBUF }$lib_server"
    echo "  lib client : $lib_client"
    if [ "$op" = "write" ] && [ "$mode" = "event" ]; then
      echo "  perftest   : SKIP (ib_write_* has no --events)"
    elif tool_path="$(resolve_perftest "$tool")"; then
      echo "  perftest   : $tool -> $tool_path (commands via $PERFTEST_COMMANDS --scenario $scen --port $port --peer-addr $PEER_ADDR)"
    else
      echo "  perftest   : SKIP ($tool not found in ${PERFTEST_BIN_DIR:-PATH})"
    fi
    return
  fi

  # ---- (A) lib bench, two-process ----
  local ready=0 srv_pid
  if [ -n "$STDBUF" ]; then
    ( $STDBUF $lib_server >"$srv_log" 2>"$srv_err" ) &
    srv_pid=$!
    wait_for_ready "$srv_log" "$srv_pid" && ready=1
  else
    ( $lib_server >"$srv_log" 2>"$srv_err" ) &
    srv_pid=$!
    sleep 2                                    # no stdbuf: fixed warmup gate
    kill -0 "$srv_pid" 2>/dev/null && ready=1
  fi
  if [ "$ready" = "1" ]; then
    $lib_client >"$cli_log" 2>"$cli_err" \
      || log "lib client exited nonzero for $name (see $cli_err)"
  else
    log "lib server never became ready for $name (see $srv_err)"
  fi
  kill_wait "$srv_pid"

  # ---- (B) perftest side ----
  if [ "$op" = "write" ] && [ "$mode" = "event" ]; then
    emit_skip_json "$pf_json" "$scen" "perftest $tool has no --events (event mode)"
    log "perftest SKIP: $tool has no --events"
  elif tool_path="$(resolve_perftest "$tool")"; then
    "$PERFTEST_COMMANDS" --scenario "$scen" --port "$port" --peer-addr "$PEER_ADDR" \
      ${PERFTEST_BIN_DIR:+--perftest-bin-dir "$PERFTEST_BIN_DIR"} \
      --json-out "$cmds_json" >/dev/null 2>&1
    local server_cmd client_cmd
    server_cmd="$(json_get "$cmds_json" server_command)"
    client_cmd="$(json_get "$cmds_json" client_command)"
    if [ -z "$server_cmd" ] || [ -z "$client_cmd" ]; then
      emit_skip_json "$pf_json" "$scen" "perftest_commands produced no command"
      log "perftest SKIP: empty command for $name"
    else
      ( eval "$server_cmd" >"$pf_srv_log" 2>&1 ) &
      local pf_pid=$!
      sleep "$SLEEP_SERVER"
      eval "$client_cmd" >"$pf_cli_log" 2>&1
      kill_wait "$pf_pid"
      "$PARSE_PERFTEST" --scenario "$scen" --raw-stdout "$pf_cli_log" \
        --json-out "$pf_json" >/dev/null 2>&1 \
        || emit_skip_json "$pf_json" "$scen" "parse_perftest failed (see $pf_cli_log)"
    fi
  else
    emit_skip_json "$pf_json" "$scen" "perftest $tool not found in ${PERFTEST_BIN_DIR:-PATH}"
    log "perftest SKIP: $tool not found"
  fi

  # ---- (C) compare ----
  local -a cmp_inputs=()
  [ -f "$lib_json" ] && cmp_inputs+=("$lib_json")
  [ -f "$pf_json" ] && cmp_inputs+=("$pf_json")
  if [ ${#cmp_inputs[@]} -gt 0 ]; then
    "$COMPARE_RESULTS" "${cmp_inputs[@]}" | tee "$case_dir/compare.md"
    {
      echo "### $name"
      cat "$case_dir/compare.md"
      echo
    } >> "$SUMMARY"
  else
    log "no result JSON for $name; nothing to compare"
  fi
}

# ---- main loop ------------------------------------------------------------
port=$BASE_PORT
for scen in "${SCENARIOS[@]}"; do
  run_case "$scen" "$port"
  port=$((port + 10))
done

if [ "$DRY_RUN" != "1" ]; then
  log "done -> $OUTPUT_DIR (summary: $SUMMARY)"
fi
