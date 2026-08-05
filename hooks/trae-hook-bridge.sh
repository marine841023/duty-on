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
  # --- Log rotation: if the file exceeds ~500 lines, trim it down to the
  # last 250 lines so the diagnostic log can't grow unbounded across many
  # hook invocations. `tr -d` strips whitespace because BSD `wc` may pad the
  # count with leading spaces; everything is best-effort (set +e + 2>/dev/null)
  # so a rotation failure can never break the AI workflow.
  if [ -f "$LOG_PATH" ]; then
    lines=$(wc -l < "$LOG_PATH" 2>/dev/null | tr -d '[:space:]')
    if [ -n "$lines" ] && [ "$lines" -gt 500 ] 2>/dev/null; then
      tail -n 250 "$LOG_PATH" > "$LOG_PATH.tmp" 2>/dev/null && mv "$LOG_PATH.tmp" "$LOG_PATH" 2>/dev/null
    fi
  fi
  printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >>"$LOG_PATH" 2>/dev/null
}

write_log "INVOKED pid=$$"

# IDE kind ("trae" / "qoder" / "cursor") passed as $1 by the installed hook
# command. Cursor's stdin payload carries no event name, so its installed
# commands bake it into $2 (normalized below).
ide="${1:-}"
hook_event="${2:-}"

# Read JSON from stdin.
stdin_text=$(cat)
# Cursor may prefix its JSON with a UTF-8 BOM; strip it so jq/python3 can parse.
stdin_text=${stdin_text#"$(printf '\357\273\277')"}
write_log "stdin: $stdin_text"

if [ -z "$stdin_text" ]; then
  write_log "empty stdin, exit"
  exit 0
fi

# Cursor payloads carry hook_event_name in camelCase (postToolUse, stop, ...)
# which the state machine does not match, and use conversation_id /
# workspace_roots where the state machine expects session_id / cwd. Patch
# them in before enrichment. Best-effort: needs jq or python3; without either
# the raw stdin goes through (the server will drop it — logged for triage).
if [ "$ide" = "cursor" ] && [ -n "$hook_event" ]; then
  case "$hook_event" in
    sessionStart) hook_event="SessionStart" ;;
    beforeSubmitPrompt) hook_event="UserPromptSubmit" ;;
    preToolUse) hook_event="PreToolUse" ;;
    postToolUse) hook_event="PostToolUse" ;;
    stop) hook_event="Stop" ;;
  esac
  patched=""
  if command -v jq >/dev/null 2>&1; then
    patched=$(printf '%s' "$stdin_text" | jq -c --arg ev "$hook_event" '
      .hook_event_name = $ev
      | .session_id = ((.session_id // .conversation_id // "cursor-session"))
      | .cwd = ((.cwd // (.workspace_roots[0] // "")))
      | .tool_name = ((.tool_name // .toolInfo.name // .tool.name))
    ' 2>/dev/null)
  elif command -v python3 >/dev/null 2>&1; then
    patched=$(printf '%s' "$stdin_text" | EV="$hook_event" python3 -c '
import json, os, sys
e = json.load(sys.stdin)
e["hook_event_name"] = os.environ["EV"]
e.setdefault("session_id", e.get("conversation_id") or "cursor-session")
e.setdefault("cwd", (e.get("workspace_roots") or [""])[0])
if "tool_name" not in e:
    ti = e.get("toolInfo") or e.get("tool") or {}
    if isinstance(ti, dict) and ti.get("name"):
        e["tool_name"] = ti["name"]
print(json.dumps(e, separators=(",", ":")))
' 2>/dev/null)
  fi
  if [ -n "$patched" ]; then
    stdin_text="$patched"
    write_log "cursor patched: $stdin_text"
  else
    write_log "cursor payload patch skipped (no jq/python3)"
  fi
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
  project_dir="$CURSOR_PROJECT_DIR"
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

# IDE kind: the explicit argument wins; fall back to the runner's env marker
# (covers legacy hook commands installed without the argument).
if [ -z "$ide" ]; then
  if [ -n "$QODER_PROJECT_DIR" ]; then
    ide="qoder"
  elif [ -n "$TRAE_PROJECT_DIR" ] || [ -n "$CLAUDE_PROJECT_DIR" ]; then
    ide="trae"
  fi
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
    --arg pp "$project_dir" --arg pn "$project_name" --arg ide "$ide" --argjson ts "$timestamp" \
    '. + {project_path:$pp, project_name:$pn, timestamp:$ts} + (if $ide == "" then {} else {ide:$ide} end)' 2>/dev/null)
elif command -v python3 >/dev/null 2>&1; then
  body=$(printf '%s' "$stdin_text" | PYTHONIOENCODING=utf-8 python3 -c '
import sys, json
e = json.load(sys.stdin)
e["project_path"] = sys.argv[1]
e["project_name"] = sys.argv[2]
e["timestamp"] = int(sys.argv[3])
if sys.argv[4]:
    e["ide"] = sys.argv[4]
print(json.dumps(e, separators=(",", ":")))
' "$project_dir" "$project_name" "$timestamp" "$ide" 2>/dev/null)
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
