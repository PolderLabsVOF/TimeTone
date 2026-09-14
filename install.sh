#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Resolve source and prebuilt assets from the same published release.
# Private forks may explicitly override the source reference.
SOURCE_REF=${TIMETONE_SOURCE_REF:-}
RELEASE_TAG=${TIMETONE_RELEASE_TAG:-}

resolve_release() {
  if [ -z "$RELEASE_TAG" ]; then
    RELEASE_TAG=$(curl -fsSL https://api.github.com/repos/DrB0rk/TimeTone/releases/latest |
      sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1)
  fi
  case "$RELEASE_TAG" in v[0-9]*.[0-9]*.[0-9]*) ;; *) printf '%s\n' "Could not resolve a stable release." >&2; exit 1 ;; esac
  [ -n "$SOURCE_REF" ] || SOURCE_REF=$RELEASE_TAG
  export TIMETONE_RELEASE_TAG="$RELEASE_TAG"
}

show_release_preview() {
  PREVIEW_ACTION=$1
  printf '\nTimeTone %s\n' "$PREVIEW_ACTION" >&2
  printf '  Target version: %s\n' "$RELEASE_TAG" >&2
  printf '  Release source: GitHub stable release\n\n' >&2
}

require_release_platform() {
  if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    printf '%s\n' "Prebuilt web releases currently support Linux x86-64 only." >&2
    exit 1
  fi
}

stop_port_processes() {
  STOP_PORT=$1
  if command -v ss >/dev/null 2>&1; then
    ss -ltnp 2>/dev/null | awk -v p=":$STOP_PORT" '$4 ~ p {print}' | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | while IFS= read -r STOP_PID; do
      [ -n "$STOP_PID" ] || continue
      kill "$STOP_PID" 2>/dev/null || true
    done
  fi
  if command -v fuser >/dev/null 2>&1; then fuser -k "$STOP_PORT/tcp" >/dev/null 2>&1 || true; fi
  stop_wait=1
  while [ "$stop_wait" -le 10 ] && (command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | awk -v p=":$STOP_PORT" '$4 ~ p {found=1} END {exit found ? 0 : 1}'); do
    sleep 1
    stop_wait=$((stop_wait + 1))
  done
  # A stale supervisor can leave the old Next.js listener alive after SIGTERM.
  # Force-kill only processes currently identified as owning this port.
  if command -v ss >/dev/null 2>&1; then
    ss -ltnp 2>/dev/null | awk -v p=":$STOP_PORT" '$4 ~ p {print}' | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | while IFS= read -r STOP_PID; do
      [ -n "$STOP_PID" ] || continue
      kill -9 "$STOP_PID" 2>/dev/null || true
    done
  fi
  if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | awk -v p=":$STOP_PORT" '$4 ~ p {found=1} END {exit found ? 0 : 1}'; then
    printf '%s\n' "Unable to free port $STOP_PORT; an existing process is still listening." >&2
    return 1
  fi
}

stop_before_update() {
  UPDATE_MODE=$(sed -n 's/^TIMETONE_INSTALL_MODE=//p' "$SCRIPT_DIR/web/.env" | head -n 1)
  if [ "$UPDATE_MODE" = docker ]; then
    printf '%s\n' "Keeping Docker running until the replacement image is ready…" >&2
    return
  fi
  UPDATE_PID=$(sed -n '1p' "$SCRIPT_DIR/web/timetone.pid" 2>/dev/null || true)
  case "$UPDATE_PID" in *[!0-9]*|'') UPDATE_PID="" ;; esac
  if [ -n "$UPDATE_PID" ] && kill -0 "$UPDATE_PID" 2>/dev/null; then
    printf '%s\n' "Stopping the existing native service before updating…" >&2
    kill "$UPDATE_PID" 2>/dev/null || true
    stop_attempt=1
    while [ "$stop_attempt" -le 15 ] && kill -0 "$UPDATE_PID" 2>/dev/null; do sleep 1; stop_attempt=$((stop_attempt + 1)); done
    if kill -0 "$UPDATE_PID" 2>/dev/null; then kill -9 "$UPDATE_PID" 2>/dev/null || true; fi
  fi
  stop_port_processes "$(sed -n 's/^TIMETONE_PORT=//p' "$SCRIPT_DIR/web/.env" | head -n 1)"
}

# Palette and branded header are defined ahead of the update and first-install
# branches below. Both of those branches `exec` a replacement process, so a
# header defined or printed after them could never render before a download
# starts. TIMETONE_HEADER_SHOWN is exported so the re-executed installer does
# not print the header a second time.
if [ -t 2 ] && [ "${TERM:-dumb}" != dumb ]; then
  C_RESET=$(printf '\033[0m'); C_BOLD=$(printf '\033[1m'); C_DIM=$(printf '\033[2m'); C_GREEN=$(printf '\033[32m'); C_CYAN=$(printf '\033[36m'); C_YELLOW=$(printf '\033[33m'); C_RED=$(printf '\033[31m'); C_BLUE=$(printf '\033[34m'); C_MAGENTA=$(printf '\033[35m')
else
  C_RESET=""; C_BOLD=""; C_DIM=""; C_GREEN=""; C_CYAN=""; C_YELLOW=""; C_RED=""; C_BLUE=""; C_MAGENTA=""
fi

is_interactive_terminal() {
  [ -t 2 ] && [ "${TERM:-dumb}" != dumb ]
}

show_installer_header() {
  printf '\n%s%s    _____ _          _____%s\n' "$C_BOLD" "$C_CYAN" "$C_RESET" >&2
  printf '%s%s   |_   _(_)_ __ ___|_   _|__  _ __   ___%s\n' "$C_BOLD" "$C_CYAN" "$C_RESET" >&2
  printf '%s%s     | | | | '\''_ ` _ \ | |/ _ \| '\''_ \ / _ \%s\n' "$C_BOLD" "$C_BLUE" "$C_RESET" >&2
  printf '%s%s     | | | | | | | | || | (_) | | | |  __/%s\n' "$C_BOLD" "$C_BLUE" "$C_RESET" >&2
  printf '%s%s     |_| |_|_| |_| |_||_|\___/|_| |_|\___|%s\n' "$C_BOLD" "$C_CYAN" "$C_RESET" >&2
  printf '%s%s                   T I M E T O N E%s\n' "$C_BOLD" "$C_GREEN" "$C_RESET" >&2
  printf '%sOffice time, beautifully tracked.%s\n\n' "$C_DIM" "$C_RESET" >&2
}

if [ "${TIMETONE_HEADER_SHOWN:-}" != 1 ]; then
  export TIMETONE_HEADER_SHOWN=1
  show_installer_header
fi

# Re-running the same entrypoint against an existing installation is an
# update, not a fresh install. Fetch the current source first, while keeping
# the user's .env and persistent data in place.
if [ -f "$SCRIPT_DIR/web/.env" ] && [ "${TIMETONE_UPDATE_IN_PROGRESS:-}" != 1 ]; then
  case " $* " in
    *" --help "*) ;;
    *" --reset-password "*) ;;
    *)
      command -v curl >/dev/null 2>&1 || { printf '%s\n' "curl is required to update TimeTone." >&2; exit 1; }
      resolve_release
      TMP_UPDATE=$(mktemp -d)
      trap 'rm -rf "$TMP_UPDATE"' EXIT HUP INT TERM
      show_release_preview "update"
      printf '%s\n' "  [1/4] Downloading application source..." >&2
      curl -fsSL "https://codeload.github.com/DrB0rk/TimeTone/tar.gz/$SOURCE_REF?cachebust=$(date +%s%N)" -o "$TMP_UPDATE/timetone.tar.gz"
      mkdir -p "$TMP_UPDATE/source"
      printf '%s\n' "  [2/4] Checking the downloaded source..." >&2
      tar -xzf "$TMP_UPDATE/timetone.tar.gz" -C "$TMP_UPDATE/source"
      NEW_SOURCE=$(find "$TMP_UPDATE/source" -mindepth 1 -maxdepth 1 -type d | head -n 1)
      [ -n "$NEW_SOURCE" ] || { printf '%s\n' "The update archive was empty." >&2; exit 1; }
      cp "$SCRIPT_DIR/web/.env" "$TMP_UPDATE/.env"
      INSTALLED_MODE=$(sed -n 's/^TIMETONE_INSTALL_MODE=//p' "$TMP_UPDATE/.env" | head -n 1)
      require_release_platform
      if [ "$INSTALLED_MODE" = native ]; then
        printf '%s\n' "  [3/4] Downloading the native web runtime..." >&2
        curl -fsSL "https://github.com/DrB0rk/TimeTone/releases/download/$RELEASE_TAG/timetone-web.tar.gz" -o "$TMP_UPDATE/timetone-web.tar.gz"
        tar -tzf "$TMP_UPDATE/timetone-web.tar.gz" | grep -q 'web/.next/standalone/server.js' || {
          printf '%s\n' "Release is missing the prebuilt web runtime; current installation was not stopped." >&2
          exit 1
        }
      fi
      # All downloads and validation happen while the current service is still
      # available. Stop it only once the replacement is ready to be applied.
      printf '%s\n' "  [4/4] Applying the verified update..." >&2
      stop_before_update
      cp -a "$NEW_SOURCE"/. "$SCRIPT_DIR"/
      cp "$TMP_UPDATE/.env" "$SCRIPT_DIR/web/.env"
      # Native deployments keep the compiled Next.js runtime outside git.
      # Replace it from the release asset during updates; copying source alone
      # would otherwise leave the previous UI bundle serving stale pages.
      INSTALLED_MODE=$(sed -n 's/^TIMETONE_INSTALL_MODE=//p' "$TMP_UPDATE/.env" | head -n 1)
      if [ "$INSTALLED_MODE" = native ]; then
        if [ -f "$TMP_UPDATE/timetone-web.tar.gz" ]; then
          if [ -d "$SCRIPT_DIR/web/.next/standalone" ]; then mv "$SCRIPT_DIR/web/.next/standalone" "$TMP_UPDATE/old-standalone"; fi
          tar -xzf "$TMP_UPDATE/timetone-web.tar.gz" -C "$SCRIPT_DIR"
        else
          printf '%s\n' "Prebuilt web bundle unavailable; update stopped." >&2
          exit 1
        fi
      fi
      trap - EXIT HUP INT TERM
      export TIMETONE_UPDATE_IN_PROGRESS=1
      exec sh "$SCRIPT_DIR/install.sh" --update "$@"
      ;;
  esac
fi

if [ ! -f "$SCRIPT_DIR/web/package.json" ]; then
  command -v curl >/dev/null 2>&1 || { printf '%s\n' "curl is required for one-command installation." >&2; exit 1; }
  resolve_release
  show_release_preview "installation"
  INSTALL_DIR=${TIMETONE_INSTALL_DIR:-"$(pwd)/TimeTone"}
  [ -d "$INSTALL_DIR/web" ] || {
    TMP_DIR=$(mktemp -d)
    trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
    mkdir -p "$INSTALL_DIR"
    REQUEST_NATIVE=false
    for arg in "$@"; do [ "$arg" = "--native" ] && REQUEST_NATIVE=true; done
    if [ "$REQUEST_NATIVE" = true ]; then
      printf '%s\n' "  [1/2] Downloading the prebuilt native web runtime..."
      if curl -fsSL "https://github.com/DrB0rk/TimeTone/releases/download/$RELEASE_TAG/timetone-web.tar.gz" -o "$TMP_DIR/timetone-web.tar.gz"; then
        tar -xzf "$TMP_DIR/timetone-web.tar.gz" -C "$INSTALL_DIR"
      else
        printf '%s\n' "Prebuilt release unavailable. No local build was attempted; retry when the release is available." >&2
        exit 1
      fi
    else
      printf '%s\n' "  [1/2] Downloading the application source..."
      curl -fsSL "https://codeload.github.com/DrB0rk/TimeTone/tar.gz/$SOURCE_REF?cachebust=$(date +%s)" -o "$TMP_DIR/timetone.tar.gz"
      mkdir -p "$TMP_DIR/source"
      tar -xzf "$TMP_DIR/timetone.tar.gz" -C "$TMP_DIR/source"
      SOURCE_DIR=$(find "$TMP_DIR/source" -mindepth 1 -maxdepth 1 -type d | head -n 1)
      cp -R "$SOURCE_DIR"/. "$INSTALL_DIR"/
    fi
  }
  # Always refresh the entrypoint, including when a previous failed install
  # already created INSTALL_DIR. This avoids rerunning a stale cached script.
  printf '%s\n' "  [2/2] Starting the TimeTone installer..."
  curl -fsSL "https://raw.githubusercontent.com/DrB0rk/TimeTone/main/install.sh?cachebust=$(date +%s%N)" -o "$INSTALL_DIR/install.sh"
  exec sh "$INSTALL_DIR/install.sh" "$@"
fi
ROOT_DIR=$SCRIPT_DIR
WEB_DIR="$ROOT_DIR/web"
NON_INTERACTIVE=false
FORCE=false
UPDATE=false
RESET_PASSWORD=false
MODE=""
phase() {
  case "$1" in
    1/4) PHASE_BAR='[##------]'; PHASE_COLOR=$C_YELLOW ;;
    2/4) PHASE_BAR='[####----]'; PHASE_COLOR=$C_CYAN ;;
    3/4) PHASE_BAR='[######--]'; PHASE_COLOR=$C_BLUE ;;
    4/4) PHASE_BAR='[########]'; PHASE_COLOR=$C_GREEN ;;
    plan) PHASE_BAR='[ ready ]'; PHASE_COLOR=$C_MAGENTA ;;
    start) PHASE_BAR='[  go!  ]'; PHASE_COLOR=$C_GREEN ;;
    *) PHASE_BAR="[ $1 ]"; PHASE_COLOR=$C_CYAN ;;
  esac
  printf '%s%s%s %s\n' "$PHASE_COLOR" "$PHASE_BAR" "$C_RESET" "$2" >&2
}

run_with_spinner() {
  TASK_LABEL=$1
  shift
  if ! is_interactive_terminal; then
    "$@"
    return
  fi
  TASK_LOG=$(mktemp "${TMPDIR:-/tmp}/timetone-task.XXXXXX")
  "$@" >"$TASK_LOG" 2>&1 &
  TASK_PID=$!
  TASK_FRAME=0
  while kill -0 "$TASK_PID" 2>/dev/null; do
    case "$TASK_FRAME" in
      0) TASK_GLYPH='|' ;;
      1) TASK_GLYPH='/' ;;
      2) TASK_GLYPH='-' ;;
      *) TASK_GLYPH='\' ;;
    esac
    printf '\r%s[%s] %s%s' "$C_CYAN" "$TASK_GLYPH" "$TASK_LABEL" "$C_RESET" >&2
    TASK_FRAME=$(((TASK_FRAME + 1) % 4))
    sleep 0.12
  done
  if wait "$TASK_PID"; then
    printf '\r%s[ok]%s %s\n' "$C_GREEN" "$C_RESET" "$TASK_LABEL" >&2
    rm -f "$TASK_LOG"
    return
  else
    TASK_STATUS=$?
  fi
  printf '\r%s[failed]%s %s\n' "$C_RED" "$C_RESET" "$TASK_LABEL" >&2
  cat "$TASK_LOG" >&2
  rm -f "$TASK_LOG"
  return "$TASK_STATUS"
}

show_install_plan() {
  PLAN_ACTION=$1
  PLAN_CURRENT=$2
  printf '%sTarget version%s  %s%s%s\n' "$C_GREEN" "$C_RESET" "$C_BOLD" "$RELEASE_TAG" "$C_RESET" >&2
  [ -z "$PLAN_CURRENT" ] || printf '%sCurrent version%s %s\n' "$C_YELLOW" "$C_RESET" "$PLAN_CURRENT" >&2
  printf '%sMode%s            %s\n\n' "$C_CYAN" "$C_RESET" "$MODE" >&2
  phase "plan" "$PLAN_ACTION starts after the checks below."
}

for arg in "$@"; do
  case "$arg" in
    --non-interactive) NON_INTERACTIVE=true ;;
    --update) UPDATE=true ;;
    --reset-password) RESET_PASSWORD=true ;;
    --native) MODE=native ;;
    --docker) MODE=docker ;;
    --force) FORCE=true ;;
    -h|--help) printf '%s\n' "Usage: ./install.sh [--docker|--native] [--update] [--reset-password] [--non-interactive] [--force]"; exit 0 ;;
    *) printf '%s\n' "Unknown option: $arg" >&2; exit 1 ;;
  esac
done
[ -f "$WEB_DIR/package.json" ] && [ -f "$WEB_DIR/compose.yaml" ] || { printf '%s\n' "Run this script from a complete TimeTone checkout." >&2; exit 1; }

ask() {
  prompt=$1 default=$2
  if [ "$NON_INTERACTIVE" = true ]; then printf '%s' "$default"; return; fi
  printf '%s [%s]: ' "$prompt" "$default" >&2
  if [ -r /dev/tty ]; then IFS= read -r answer </dev/tty || true; else answer=""; fi
  printf '%s' "${answer:-$default}"
}

[ -n "$MODE" ] || MODE=${TIMETONE_MODE:-}
if [ "$UPDATE" = true ] && [ -f "$WEB_DIR/.env" ]; then
  # The installed mode is authoritative during an update. This prevents a
  # copied command such as --native from accidentally converting a Docker
  # deployment (or vice versa) and leaving its data service behind.
  INSTALLED_MODE=$(sed -n 's/^TIMETONE_INSTALL_MODE=//p' "$WEB_DIR/.env" | head -n 1)
  MODE=$INSTALLED_MODE
  [ "$MODE" = native ] || MODE=docker
fi
DEFAULT_MODE=docker
if [ -z "$MODE" ]; then
  if [ "$NON_INTERACTIVE" = true ]; then MODE=docker
  else
    command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1 || DEFAULT_MODE=native
    printf '\n%s%sChoose an installation mode%s\n' "$C_BOLD" "$C_BLUE" "$C_RESET" >&2
    printf '  %s[1]%s Docker   %s(recommended for servers)%s\n' "$C_CYAN" "$C_RESET" "$C_DIM" "$C_RESET" >&2
    printf '  %s[2]%s Native   %s(Node.js process, no Docker)%s\n' "$C_CYAN" "$C_RESET" "$C_DIM" "$C_RESET" >&2
    if [ "$DEFAULT_MODE" = docker ]; then DEFAULT_CHOICE=1; else DEFAULT_CHOICE=2; fi
    printf '%sSelection [%s]: %s' "$C_BOLD" "$DEFAULT_CHOICE" "$C_RESET" >&2
    CHOICE=""
    if [ -r /dev/tty ]; then IFS= read -r CHOICE </dev/tty || true; else CHOICE=""; fi
    case "$CHOICE" in
      1|docker|Docker|DOCKER) MODE=docker ;;
      2|native|Native|NATIVE) MODE=native ;;
      "") MODE=$DEFAULT_MODE ;;
      *) printf '%sInvalid selection; using option %s.%s\n' "$C_YELLOW" "$DEFAULT_CHOICE" "$C_RESET" >&2; MODE=$DEFAULT_MODE ;;
    esac
  fi
fi
case "$MODE" in docker|native) ;; *) printf '%s\n' "Unrecognized install mode; using $DEFAULT_MODE." >&2; MODE=$DEFAULT_MODE ;; esac

run_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"; elif command -v sudo >/dev/null 2>&1; then sudo "$@"; else printf '%s\n' "This step needs root privileges. Install sudo or rerun as root: $*" >&2; exit 1; fi
}
ensure_base_dependencies() {
  command -v tar >/dev/null 2>&1 && command -v sed >/dev/null 2>&1 && command -v find >/dev/null 2>&1 && command -v curl >/dev/null 2>&1 && return
  if command -v apt-get >/dev/null 2>&1; then
    phase "1/4" "Installing required system utilities..."
    run_root apt-get update
    run_root apt-get install -y ca-certificates curl tar sed findutils openssl
  else
    printf '%s\n' "Missing required utilities (curl, tar, sed, find). Install them with your OS package manager and rerun." >&2
    exit 1
  fi
}
ensure_base_dependencies
resolve_release
CURRENT_VERSION=""
if [ "$UPDATE" = true ]; then
  CURRENT_VERSION=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$WEB_DIR/package.json" | head -n 1)
fi
if [ "$UPDATE" = true ]; then PLAN_ACTION="Update"; else PLAN_ACTION="Installation"; fi
show_install_plan "$PLAN_ACTION" "$CURRENT_VERSION"

if [ "$MODE" = docker ]; then
  if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
      phase "2/4" "Installing Docker Engine and Compose..."
      run_root apt-get update
      run_root apt-get install -y docker.io docker-compose-v2 || run_root apt-get install -y docker.io docker-compose-plugin
      if command -v systemctl >/dev/null 2>&1; then run_root systemctl enable --now docker || true; fi
    else
      printf '%s\n' "Docker Compose v2 is required. Install Docker Engine and Compose, then rerun." >&2
      exit 1
    fi
  fi
  command -v docker >/dev/null 2>&1 || { printf '%s\n' "Docker installation did not provide the docker command." >&2; exit 1; }
  docker compose version >/dev/null 2>&1 || { printf '%s\n' "Docker Compose v2 is required (docker compose)." >&2; exit 1; }
else
  NODE_OK=false
  if command -v node >/dev/null 2>&1; then node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 20 ? 0 : 1)' >/dev/null 2>&1 && NODE_OK=true; fi
  if [ "$NODE_OK" = false ] || ! command -v npm >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
      phase "2/4" "Installing Node.js 24 and npm..."
      run_root apt-get update
      run_root apt-get install -y ca-certificates curl
      curl -fsSL https://deb.nodesource.com/setup_24.x | run_root sh
      run_root apt-get install -y nodejs
    else
      printf '%s\n' "Node.js 20.9+ and npm are required. Install them with your OS package manager and rerun." >&2
      exit 1
    fi
  fi
  command -v node >/dev/null 2>&1 && command -v npm >/dev/null 2>&1 || { printf '%s\n' "Node.js and npm installation did not complete." >&2; exit 1; }
fi

PORT=${TIMETONE_PORT:-3000}
if [ -f "$WEB_DIR/.env" ] && [ -z "${TIMETONE_PORT:-}" ]; then
  EXISTING_PORT=$(sed -n 's/^TIMETONE_PORT=//p' "$WEB_DIR/.env" | head -n 1)
  [ -n "$EXISTING_PORT" ] && PORT=$EXISTING_PORT
fi
show_status() {
  HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  HEALTH_URL="http://127.0.0.1:$PORT/api/health"
  HEALTH_OK=false
  HEALTH_BODY=""
  HEALTH_CURL_ERROR=""
  APP_VERSION=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$WEB_DIR/package.json" | head -n 1)
  [ -n "$APP_VERSION" ] || APP_VERSION=$(sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$/\1/p' "$ROOT_DIR/VERSION" | head -n 1)
  [ -n "$APP_VERSION" ] || APP_VERSION="unknown"
  RUNTIME_VERSION="unknown"
  attempt=1
  while [ "$attempt" -le 15 ]; do
    if HEALTH_BODY=$(curl -fsS --max-time 5 "$HEALTH_URL" 2>/tmp/timetone-health-error); then
      RUNTIME_VERSION=$(printf '%s' "$HEALTH_BODY" | sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
      [ -n "$RUNTIME_VERSION" ] || RUNTIME_VERSION="unknown"
      if [ "$RUNTIME_VERSION" = "$APP_VERSION" ]; then HEALTH_OK=true; break; fi
    else
      HEALTH_CURL_ERROR=$(sed -n '1p' /tmp/timetone-health-error 2>/dev/null || true)
    fi
    sleep 1
    attempt=$((attempt + 1))
  done
  if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then BUNDLE_STATUS="prebuilt production bundle"; else BUNDLE_STATUS="locally built production bundle"; fi
  if [ "$MODE" = native ]; then
    DATA_PATH=$(sed -n 's/^DATABASE_PATH=//p' "$WEB_DIR/.env" | head -n 1)
    [ -n "$DATA_PATH" ] || DATA_PATH="$WEB_DIR/data/timekeep.db"
  else
    DATA_PATH="Docker volume: timekeep-data"
  fi
  if [ "$UPDATE" = true ]; then OPERATION="updated"; else OPERATION="installed"; fi
  if [ "$HEALTH_OK" = true ] && [ "$RUNTIME_VERSION" = "$APP_VERSION" ]; then
    printf '\n%s%s✓ TimeTone %s successfully%s\n' "$C_BOLD" "$C_GREEN" "$OPERATION" "$C_RESET"
    printf '  Version: %s%s%s\n' "$C_CYAN" "$APP_VERSION" "$C_RESET"
    printf '  Mode: %s%s%s\n' "$C_CYAN" "$MODE" "$C_RESET"
    printf '  Web runtime: %s%s%s\n' "$C_CYAN" "$BUNDLE_STATUS" "$C_RESET"
    printf '  LAN URL: %shttp://%s:%s%s\n' "$C_CYAN" "${HOST_IP:-localhost}" "$PORT" "$C_RESET"
    printf '  Local health: %shealthy%s (%s)\n' "$C_GREEN" "$C_RESET" "$HEALTH_URL"
    printf '  Running version: %s%s%s\n' "$C_CYAN" "$RUNTIME_VERSION" "$C_RESET"
    printf '  Persistent data: %s%s%s\n' "$C_DIM" "$DATA_PATH" "$C_RESET"
    printf '  Admin UI: %sopen the LAN URL above and sign in%s\n' "$C_DIM" "$C_RESET"
  else
    printf '\n%s%s✗ Installation needs attention%s\n' "$C_BOLD" "$C_RED" "$C_RESET"
    printf '  Installed version: %s\n' "$APP_VERSION"
    printf '  Running version: %s\n' "$RUNTIME_VERSION"
    printf '  URL: http://%s:%s\n' "${HOST_IP:-localhost}" "$PORT"
    printf '  Health endpoint: %s\n' "$HEALTH_URL"
    printf '  Health response: %s\n' "${HEALTH_BODY:-<no response>}"
    [ -n "$HEALTH_CURL_ERROR" ] && printf '  Health error: %s\n' "$HEALTH_CURL_ERROR"
    if [ "$MODE" = native ]; then
      if [ -f "$WEB_DIR/timetone.pid" ]; then printf '  PID file: %s (state: %s)\n' "$(sed -n '1p' "$WEB_DIR/timetone.pid")" "$(ps -p "$(sed -n '1p' "$WEB_DIR/timetone.pid")" -o stat= 2>/dev/null || printf 'not running')"; else printf '  PID file: missing\n'; fi
      if command -v ss >/dev/null 2>&1; then printf '  Port check: %s\n' "$(ss -ltnp 2>/dev/null | awk -v p=":$PORT" '$4 ~ p {print; found=1} END {if (!found) print "not listening"}')"; fi
      printf '  Recent log output:\n'; tail -n 30 "$WEB_DIR/timetone.log" 2>/dev/null || printf '    <log unavailable>\n'
    else
      printf '  Container status:\n'; (cd "$WEB_DIR" && docker compose ps) 2>&1 || true
      printf '  Recent container logs:\n'; (cd "$WEB_DIR" && docker compose logs --tail=30) 2>&1 || true
    fi
    return 1
  fi
}

stop_native_server() {
  if [ -f "$WEB_DIR/timetone.pid" ]; then
    OLD_PID=$(sed -n '1p' "$WEB_DIR/timetone.pid")
    case "$OLD_PID" in *[!0-9]*|'') OLD_PID="" ;; esac
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
      kill "$OLD_PID" 2>/dev/null || true
      wait_attempt=1
      while [ "$wait_attempt" -le 10 ] && kill -0 "$OLD_PID" 2>/dev/null; do
        sleep 1
        wait_attempt=$((wait_attempt + 1))
      done
      if kill -0 "$OLD_PID" 2>/dev/null; then kill -9 "$OLD_PID" 2>/dev/null || true; fi
    fi
  fi
  # A manually created systemd/nohup process may not use our PID file. On a
  # native deployment this port belongs to TimeTone, so release it before the
  # replacement runtime starts. Ignore systems without fuser.
  stop_port_processes "$PORT"
}

if [ "$RESET_PASSWORD" = true ]; then
  [ -f "$WEB_DIR/.env" ] || { printf '%s\n' "No existing installation was found to reset." >&2; exit 1; }
  [ "$NON_INTERACTIVE" = true ] && { printf '%s\n' "Password reset requires an interactive terminal." >&2; exit 1; }
  NEW_PASSWORD=""
  CONFIRM_PASSWORD=""
  while [ ${#NEW_PASSWORD} -lt 8 ] || [ "$NEW_PASSWORD" != "$CONFIRM_PASSWORD" ]; do
    printf 'New admin password (at least 8 characters): ' >&2
    if [ -r /dev/tty ]; then stty -echo </dev/tty; IFS= read -r NEW_PASSWORD </dev/tty || true; stty echo </dev/tty; else exit 1; fi
    printf '\nConfirm new admin password: ' >&2
    if [ -r /dev/tty ]; then stty -echo </dev/tty; IFS= read -r CONFIRM_PASSWORD </dev/tty || true; stty echo </dev/tty; else exit 1; fi
    printf '\n' >&2
    [ ${#NEW_PASSWORD} -ge 8 ] || printf '%s\n' "Password must be at least 8 characters." >&2
    [ "$NEW_PASSWORD" = "$CONFIRM_PASSWORD" ] || printf '%s\n' "Passwords do not match." >&2
  done
  RESET_ENV=$(mktemp)
  umask 077
  awk -v password="$NEW_PASSWORD" 'BEGIN { updated=0 } /^ADMIN_PASSWORD=/ { print "ADMIN_PASSWORD=" password; updated=1; next } { print } END { if (!updated) print "ADMIN_PASSWORD=" password }' "$WEB_DIR/.env" > "$RESET_ENV"
  mv "$RESET_ENV" "$WEB_DIR/.env"
  DB_PATH=$(sed -n 's/^DATABASE_PATH=//p' "$WEB_DIR/.env" | head -n 1)
  [ -n "$DB_PATH" ] || DB_PATH="$WEB_DIR/data/timekeep.db"
  SQLITE_MODULE=$(find "$WEB_DIR/.next/standalone/node_modules/.deno" "$WEB_DIR/node_modules/.deno" -type d -path '*/better-sqlite3' 2>/dev/null | head -n 1)
  if [ -n "$SQLITE_MODULE" ]; then
    for RESET_DB in "$DB_PATH" "$WEB_DIR/data/timekeep.db" "$WEB_DIR/.next/standalone/data/timekeep.db"; do
      [ -f "$RESET_DB" ] || continue
      node - "$RESET_DB" "$NEW_PASSWORD" "$SQLITE_MODULE" <<'NODE'
const crypto = require("node:crypto");
const [dbPath, password, modulePath] = process.argv.slice(2);
const Database = require(modulePath);
const digest = crypto.createHash("sha256").update(password).digest("hex");
const db = new Database(dbPath);
db.prepare("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)").run();
db.prepare("INSERT INTO settings (key, value) VALUES ('admin_password_digest', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value").run(digest);
db.close();
NODE
    done
  else
    printf '%s\n' "Warning: database password digest could not be updated; the server will use ADMIN_PASSWORD from .env." >&2
  fi
  MODE=$(sed -n 's/^TIMETONE_INSTALL_MODE=//p' "$WEB_DIR/.env" | head -n 1)
  [ "$MODE" = native ] || MODE=docker
  printf '%s▸%s Restarting TimeTone with the new password…\n' "$C_GREEN" "$C_RESET" >&2
  if [ "$MODE" = docker ]; then
    (cd "$WEB_DIR" && docker compose up -d)
  else
    stop_native_server
    if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then (cd "$WEB_DIR/.next/standalone" && set -a && . "$WEB_DIR/.env" && set +a && PORT="$PORT" nohup node server.js > "$WEB_DIR/timetone.log" 2>&1 & echo $! > "$WEB_DIR/timetone.pid"); else (cd "$WEB_DIR" && set -a && . .env && set +a && PORT="$PORT" nohup npm run start -- --hostname 0.0.0.0 > timetone.log 2>&1 & echo $! > timetone.pid); fi
  fi
  show_status || true
  printf '%s\n' "Password reset complete. Configuration and database preserved." >&2
  exit 0
fi

install_prebuilt_native() {
  require_release_platform
  resolve_release
  [ -f "$WEB_DIR/.next/standalone/server.js" ] && return
  BUNDLE_STAGE=$(mktemp -d)
  phase "3/4" "Downloading the native web runtime..."
  run_with_spinner "Downloading TimeTone $RELEASE_TAG" curl -fsSL "https://github.com/DrB0rk/TimeTone/releases/download/$RELEASE_TAG/timetone-web.tar.gz" -o "$BUNDLE_STAGE/web.tar.gz"
  phase "4/4" "Unpacking the verified runtime..."
  tar -xzf "$BUNDLE_STAGE/web.tar.gz" -C "$ROOT_DIR"
  [ -f "$WEB_DIR/.next/standalone/server.js" ] || {
    printf '%s\n' "The release has no standalone runtime." >&2; exit 1;
  }
}

install_prebuilt_docker() {
  require_release_platform
  resolve_release
  IMAGE_STAGE=$(mktemp -d)
  phase "3/4" "Downloading the Docker image..."
  run_with_spinner "Downloading TimeTone $RELEASE_TAG" curl -fsSL "https://github.com/DrB0rk/TimeTone/releases/download/$RELEASE_TAG/timetone-docker.tar.gz" -o "$IMAGE_STAGE/docker.tar.gz"
  phase "4/4" "Loading and starting the Docker image..."
  run_with_spinner "Loading TimeTone $RELEASE_TAG" docker load -i "$IMAGE_STAGE/docker.tar.gz"
  (cd "$WEB_DIR" && docker compose config -q && docker compose up -d --no-build)
}

if [ "$UPDATE" = true ]; then
  phase "start" "Updating TimeTone $RELEASE_TAG in place."
  if [ "$MODE" = docker ]; then
    install_prebuilt_docker
  else
    if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then
      printf '%s\n' "Using the prebuilt web bundle (no local build required)."
    else
      install_prebuilt_native
    fi
    stop_native_server
    if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then (cd "$WEB_DIR/.next/standalone" && set -a && . "$WEB_DIR/.env" && set +a && PORT="$PORT" nohup node server.js > "$WEB_DIR/timetone.log" 2>&1 & echo $! > "$WEB_DIR/timetone.pid"); else (cd "$WEB_DIR" && set -a && . .env && set +a && PORT="$PORT" nohup npm run start -- --hostname 0.0.0.0 > timetone.log 2>&1 & echo $! > timetone.pid); fi
  fi
  show_status || true
  printf '%s\n' "Configuration and database preserved." >&2
  exit 0
fi

if [ -f "$WEB_DIR/.env" ] && [ "$FORCE" = false ] && [ "$NON_INTERACTIVE" = false ]; then
  printf 'An existing web/.env was found. Replace it? [y/N]: ' >&2
  if [ -r /dev/tty ]; then IFS= read -r replace </dev/tty || true; else replace=""; fi
  case "$replace" in y|Y|yes|YES) ;; *)
    printf '%s\n' "Kept existing configuration. Starting TimeTone…"
    if [ "$MODE" = docker ]; then (cd "$WEB_DIR" && docker compose up -d --build)
    else
      stop_native_server
      if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then (cd "$WEB_DIR/.next/standalone" && set -a && . "$WEB_DIR/.env" && set +a && PORT="$PORT" nohup node server.js > "$WEB_DIR/timetone.log" 2>&1 & echo $! > "$WEB_DIR/timetone.pid"); else (cd "$WEB_DIR" && npm run build >/dev/null && set -a && . .env && set +a && PORT="$PORT" nohup npm run start -- --hostname 0.0.0.0 > timetone.log 2>&1 & echo $! > "$WEB_DIR/timetone.pid"); fi
    fi
    show_status || true
    exit 0 ;;
  esac
fi

ADMIN_PASSWORD=${TIMETONE_ADMIN_PASSWORD:-}
if [ -z "$ADMIN_PASSWORD" ]; then
  [ "$NON_INTERACTIVE" = false ] || { printf '%s\n' "TIMETONE_ADMIN_PASSWORD is required with --non-interactive." >&2; exit 1; }
  while [ ${#ADMIN_PASSWORD} -lt 8 ]; do
    printf 'Create an admin password (at least 8 characters): ' >&2
    if [ -r /dev/tty ]; then stty -echo </dev/tty; IFS= read -r ADMIN_PASSWORD </dev/tty || true; stty echo </dev/tty; else printf '%s\n' "An interactive terminal is required to create a password. Set TIMETONE_ADMIN_PASSWORD for piped installs without a TTY." >&2; exit 1; fi
    printf '\n' >&2
    [ ${#ADMIN_PASSWORD} -ge 8 ] || printf '%s\n' "Please use at least 8 characters." >&2
  done
fi
PORT=${TIMETONE_PORT:-$(ask "LAN port" "3000")}
TIMEZONE=${TIMEKEEP_TIMEZONE:-$(ask "Office timezone (IANA name)" "Europe/Amsterdam")}
case "$PORT" in *[!0-9]*|'') printf '%s\n' "TIMETONE_PORT must be a number." >&2; exit 1;; esac
if command -v openssl >/dev/null 2>&1; then SECRET=$(openssl rand -hex 32); else SECRET=$(date +%s | sha256sum | awk '{print $1}'); fi
umask 077
if [ "$MODE" = native ]; then mkdir -p "$WEB_DIR/data"; DATABASE_LINE="DATABASE_PATH=$WEB_DIR/data/timekeep.db"; else DATABASE_LINE=""; fi
cat > "$WEB_DIR/.env" <<EOF
# Generated by TimeTone install.sh — keep this file private.
ADMIN_PASSWORD=$ADMIN_PASSWORD
ADMIN_SECRET=$SECRET
COOKIE_SECURE=false
TIMEKEEP_TIMEZONE=$TIMEZONE
TIMETONE_PORT=$PORT
$([ "$MODE" = native ] && printf '%s\n' 'TIMETONE_INSTALL_MODE=native' || true)
$DATABASE_LINE
EOF

phase "start" "Installing TimeTone $RELEASE_TAG."
if [ "$MODE" = docker ]; then
  install_prebuilt_docker
else
  if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then
    printf '%s\n' "Using the prebuilt web bundle (no local build required)."
  else
    install_prebuilt_native
  fi
  stop_native_server
  if [ -f "$WEB_DIR/.next/standalone/server.js" ]; then (cd "$WEB_DIR/.next/standalone" && set -a && . "$WEB_DIR/.env" && set +a && PORT="$PORT" nohup node server.js > "$WEB_DIR/timetone.log" 2>&1 & echo $! > "$WEB_DIR/timetone.pid"); else (cd "$WEB_DIR" && set -a && . .env && set +a && PORT="$PORT" nohup npm run start -- --hostname 0.0.0.0 > timetone.log 2>&1 & echo $! > timetone.pid); fi
fi
show_status || true
[ "$MODE" = native ] && printf '%s\n' "Native logs: $WEB_DIR/timetone.log  |  PID file: $WEB_DIR/timetone.pid"
printf '%s\n' "For browser-based USB updates, serve this address through HTTPS (or use localhost)."
