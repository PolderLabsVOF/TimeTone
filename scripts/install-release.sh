#!/usr/bin/env sh
set -eu

CURRENT_ROOT=${1:?current install root required}
STAGE=${2:?staging directory required}
TAG=${3:?release tag required}
ARCHIVE="$STAGE/timetone-web.tar.gz"
EXTRACTED="$STAGE/source"
PRESERVED="$STAGE/preserved"
BACKUP_ROOT="${CURRENT_ROOT}.previous-${TAG}"
FAILED_ROOT="$STAGE/failed-install"
STATUS_FILE=${TIMETONE_UPDATE_STATUS:-$CURRENT_ROOT/web/data/update-status.json}
SWAPPED=false
SERVICE_STOPPED=false
NEW_PID=""
NATIVE_SERVICE_NAME=timetone.service

native_service_available() {
  command -v systemctl >/dev/null 2>&1 || return 1
  if [ "$(id -u)" -eq 0 ]; then
    SERVICE_FILE="/etc/systemd/system/$NATIVE_SERVICE_NAME"
  else
    SERVICE_FILE="${HOME:-$(getent passwd "$(id -un)" | cut -d: -f6)}/.config/systemd/user/$NATIVE_SERVICE_NAME"
  fi
  [ -f "$SERVICE_FILE" ] || return 1
  if [ "$(id -u)" -eq 0 ]; then systemctl cat "$NATIVE_SERVICE_NAME" >/dev/null 2>&1; else systemctl --user cat "$NATIVE_SERVICE_NAME" >/dev/null 2>&1; fi
}

native_service_ctl() {
  if [ "$(id -u)" -eq 0 ]; then systemctl "$@"; else systemctl --user "$@"; fi
}

write_status() {
  mkdir -p "$(dirname "$STATUS_FILE")"
  printf '{"status":"%s","message":"%s","version":"%s","updatedAt":"%s"}\n' "$1" "$2" "$TAG" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STATUS_FILE"
}

valid_pid() {
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  return 0
}

stop_pid() {
  STOP_PID=$1
  if native_service_available; then
    native_service_ctl stop "$NATIVE_SERVICE_NAME"
    return
  fi
  valid_pid "$STOP_PID" || return 0
  kill -0 "$STOP_PID" 2>/dev/null || return 0
  kill "$STOP_PID" 2>/dev/null || true
  STOP_ATTEMPT=1
  while [ "$STOP_ATTEMPT" -le 15 ] && kill -0 "$STOP_PID" 2>/dev/null; do
    sleep 1
    STOP_ATTEMPT=$((STOP_ATTEMPT + 1))
  done
  kill -0 "$STOP_PID" 2>/dev/null && kill -9 "$STOP_PID" 2>/dev/null || true
}

launch_server() {
  LAUNCH_ROOT=$1
  if native_service_available; then
    native_service_ctl enable --now "$NATIVE_SERVICE_NAME"
    NEW_PID=$(native_service_ctl show "$NATIVE_SERVICE_NAME" --property=MainPID --value 2>/dev/null || true)
    return
  fi
  LAUNCH_PORT=$(sed -n 's/^TIMETONE_PORT=//p' "$LAUNCH_ROOT/web/.env" | head -n 1)
  case "$LAUNCH_PORT" in ''|*[!0-9]*) LAUNCH_PORT=3000 ;; esac
  (
    cd "$LAUNCH_ROOT/web/.next/standalone"
    set -a
    . "$LAUNCH_ROOT/web/.env"
    set +a
    DATABASE_PATH="$LAUNCH_ROOT/web/data/timekeep.db"
    PORT="$LAUNCH_PORT"
    HOSTNAME=0.0.0.0
    export DATABASE_PATH PORT HOSTNAME
    nohup node server.js > "$LAUNCH_ROOT/web/timetone.log" 2>&1 &
    printf '%s\n' "$!" > "$LAUNCH_ROOT/web/timetone.pid"
  )
  NEW_PID=$(sed -n '1p' "$LAUNCH_ROOT/web/timetone.pid" 2>/dev/null || true)
}

wait_for_health() {
  HEALTH_ROOT=$1
  EXPECTED_VERSION=${2:-}
  HEALTH_PORT=$(sed -n 's/^TIMETONE_PORT=//p' "$HEALTH_ROOT/web/.env" | head -n 1)
  case "$HEALTH_PORT" in ''|*[!0-9]*) HEALTH_PORT=3000 ;; esac
  HEALTH_ATTEMPT=1
  while [ "$HEALTH_ATTEMPT" -le 30 ]; do
    if HEALTH_BODY=$(curl -fsS --max-time 2 "http://127.0.0.1:$HEALTH_PORT/api/health" 2>/dev/null); then
      if [ -z "$EXPECTED_VERSION" ]; then
        return 0
      fi
      HEALTH_VERSION=$(printf '%s' "$HEALTH_BODY" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p')
      [ "$HEALTH_VERSION" = "$EXPECTED_VERSION" ] && return 0
    fi
    sleep 1
    HEALTH_ATTEMPT=$((HEALTH_ATTEMPT + 1))
  done
  return 1
}

rollback() {
  RESULT=$1
  trap - EXIT HUP INT TERM
  if [ "$SWAPPED" = true ]; then
    stop_pid "$NEW_PID"
    if [ -d "$BACKUP_ROOT" ]; then
      if [ -d "$CURRENT_ROOT" ]; then mv "$CURRENT_ROOT" "$FAILED_ROOT"; fi
      mv "$BACKUP_ROOT" "$CURRENT_ROOT"
      STATUS_FILE="$CURRENT_ROOT/web/data/update-status.json"
      write_status rolling_back "TimeTone $TAG failed its health check; restoring the previous version"
      if launch_server "$CURRENT_ROOT" && wait_for_health "$CURRENT_ROOT" ""; then
        write_status rolled_back "TimeTone $TAG failed; the previous version is running"
      else
        write_status error "TimeTone $TAG failed and the previous version could not be restarted"
      fi
    else
      write_status error "TimeTone $TAG failed before the previous version could be restored"
    fi
  elif [ "$SERVICE_STOPPED" = true ]; then
    STATUS_FILE="$CURRENT_ROOT/web/data/update-status.json"
    if launch_server "$CURRENT_ROOT" && wait_for_health "$CURRENT_ROOT" ""; then
      write_status error "Update failed before replacing TimeTone; the previous version is running"
    else
      write_status error "Update failed before replacing TimeTone and the previous version could not be restarted"
    fi
  else
    write_status error "Update failed before replacing TimeTone"
  fi
  exit "$RESULT"
}

trap 'rollback $?' EXIT HUP INT TERM

[ -d "$CURRENT_ROOT/web" ] || { echo "Current TimeTone install is incomplete" >&2; exit 1; }
[ -f "$CURRENT_ROOT/web/.env" ] || { echo "Current TimeTone configuration is missing" >&2; exit 1; }
[ -f "$CURRENT_ROOT/web/.next/standalone/server.js" ] || { echo "Current TimeTone install has no standalone runtime for rollback" >&2; exit 1; }
[ -f "$ARCHIVE" ] || { echo "Prebuilt TimeTone web archive is missing" >&2; exit 1; }
[ ! -e "$BACKUP_ROOT" ] || { echo "Previous backup already exists at $BACKUP_ROOT" >&2; exit 1; }

write_status preparing "Preparing TimeTone $TAG"
mkdir -p "$EXTRACTED" "$PRESERVED"
tar -xzf "$ARCHIVE" -C "$EXTRACTED"
if [ -d "$EXTRACTED/web" ]; then
  SOURCE=$EXTRACTED
else
  SOURCE=$(find "$EXTRACTED" -mindepth 1 -maxdepth 1 -type d | head -n 1)
fi
[ -n "${SOURCE:-}" ] && [ -d "$SOURCE/web" ] || { echo "Release archive contained no TimeTone install root" >&2; exit 1; }
[ -f "$SOURCE/web/.next/standalone/server.js" ] || { echo "Release archive lacks web/.next/standalone/server.js" >&2; exit 1; }
[ -d "$SOURCE/web/.next/standalone/.next/static" ] || { echo "Release archive lacks standalone static assets" >&2; exit 1; }
[ -d "$SOURCE/web/public" ] || { echo "Release archive lacks web/public assets" >&2; exit 1; }

cp "$CURRENT_ROOT/web/.env" "$PRESERVED/.env"
# Release archives must never supply a runtime database or local configuration.
if [ -d "$SOURCE/web/data" ]; then mv "$SOURCE/web/data" "$PRESERVED/release-data"; fi
mkdir -p "$SOURCE/web/data"
cp "$PRESERVED/.env" "$SOURCE/web/.env"

write_status restarting "Restarting TimeTone $TAG"
OLD_PID=$(sed -n '1p' "$CURRENT_ROOT/web/timetone.pid" 2>/dev/null || true)
stop_pid "$OLD_PID"
SERVICE_STOPPED=true
if [ -d "$CURRENT_ROOT/web/data" ]; then cp -a "$CURRENT_ROOT/web/data" "$PRESERVED/data"; fi
if [ -d "$PRESERVED/data" ]; then cp -a "$PRESERVED/data/." "$SOURCE/web/data/"; fi
mv "$CURRENT_ROOT" "$BACKUP_ROOT"
SWAPPED=true
mv "$SOURCE" "$CURRENT_ROOT"
STATUS_FILE="$CURRENT_ROOT/web/data/update-status.json"
launch_server "$CURRENT_ROOT"
if ! wait_for_health "$CURRENT_ROOT" "${TAG#v}"; then
  echo "Updated TimeTone service did not pass its health check" >&2
  exit 1
fi

trap - EXIT HUP INT TERM
write_status complete "TimeTone $TAG is ready"
rm -rf "$STAGE"
