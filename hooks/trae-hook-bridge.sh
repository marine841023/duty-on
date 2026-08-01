#!/usr/bin/env bash
# Trae IDE Hook Bridge (macOS / Linux) — mirrors trae-hook-bridge.ps1.
#
# Called by Trae IDE's Hook system for each AI lifecycle event. Reads the hook
# event JSON from stdin, enriches it with project information, and POSTs it to
# the DutyOn (开工啦) local HTTP server (http://127.0.0.1:17521/hook).
#
# Non-blocking by design: if the pet is not running or any step fails, exit 0
# silently so the AI's workflow is never affected.
#
# Diagnostic log: ~/.dutyon/hooks/bridge.log

# Never let a failure here break the AI workflow.
set +e

LOG_DIR="$HOME/.dutyon/hooks"
LOG_PATH="$LOG_DIR/bridge.log"
PET_URL="http://127.0.0.1:17521/hook"

write_log() {
  mkdir -p "$LOG_DIR" 2>/dev/null
  printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >>"$LOG_PATH" 2>/dev/null
}

write_log "INVOKED pid=$$"

# Read JSON from stdin.
stdin_text=$(cat)
write_log "stdin: $stdin_text"

if [ -z "$stdin_text" ]; then
  write_log "empty stdin, exit"
  exit 0
fi

# Resolve project dir: env override first, else .cwd from the event JSON.
# QODER_PROJECT_DIR is set when invoked by Qoder's hook runner; TRAE_PROJECT_DIR
# / CLAUDE_PROJECT_DIR when invoked by Trae IDE.
project_dir="$TRAE_PROJECT_DIR"
if [ -z "$project_dir" ]; then
  project_dir="$QODER_PROJECT_DIR"
fi
if [ -z "$project_dir" ]; then
  project_dir="$CLAUDE_PROJECT_DIR"
fi
if [ -z "$project_dir" ]; then
  if command -v jq >/dev/null 2>&1; then
    project_dir=$(printf '%s' "$stdin_text" | jq -r '.cwd // empty' 2>/dev/null)
  elif command -v python3 >/dev/null 2>&1; then
    project_dir=$(printf '%s' "$stdin_text" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("cwd") or "")' 2>/dev/null)
  fi
fi

project_name=""
if [ -n "$project_dir" ]; then
  project_name=$(basename "$project_dir")
fi

# Millisecond epoch (portable: BSD date lacks %N; second precision is fine since
# the server stamps timing from its own clock).
timestamp=$(($(date '+%s') * 1000))

# Enrich the event JSON with project_path / project_name / timestamp.
# Prefer jq, then python3; if neither is available, send the raw stdin — the
# server derives project info from .cwd as a fallback.
body=""
if command -v jq >/dev/null 2>&1; then
  body=$(printf '%s' "$stdin_text" | jq -c \
    --arg pp "$project_dir" --arg pn "$project_name" --argjson ts "$timestamp" \
    '. + {project_path:$pp, project_name:$pn, timestamp:$ts}' 2>/dev/null)
elif command -v python3 >/dev/null 2>&1; then
  body=$(printf '%s' "$stdin_text" | PYTHONIOENCODING=utf-8 python3 -c '
import sys, json
e = json.load(sys.stdin)
e["project_path"] = sys.argv[1]
e["project_name"] = sys.argv[2]
e["timestamp"] = int(sys.argv[3])
print(json.dumps(e, separators=(",", ":")))
' "$project_dir" "$project_name" "$timestamp" 2>/dev/null)
fi

if [ -z "$body" ]; then
  write_log "enrich skipped (no jq/python3); sending raw stdin"
  body="$stdin_text"
fi

# POST to the pet's HTTP server with a 2s timeout. Never block the AI.
if command -v curl >/dev/null 2>&1; then
  if curl -s -m 2 -X POST -H "Content-Type: application/json" -d "$body" "$PET_URL" >/dev/null 2>&1; then
    write_log "POST ok"
  else
    write_log "POST failed (curl exit $?)"
  fi
else
  write_log "POST skipped: curl not found"
fi

# Exit successfully with no stdout (don't interfere with the AI).
exit 0
