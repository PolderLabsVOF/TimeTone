#!/usr/bin/env sh
set -eu

CURRENT_ROOT=${1:?current install root required}
STAGE=${2:?staging directory required}
TAG=${3:?release tag required}
ARCHIVE="$STAGE/release.tar.gz"
IMAGE_ARCHIVE="$STAGE/timetone-docker.tar.gz"
IMAGE_URL="https://github.com/DrB0rk/TimeTone/releases/download/$TAG/timetone-docker.tar.gz"
EXTRACTED="$STAGE/source"
STATUS_FILE=/host/web/.timetone-update-status.json
write_status() {
  mkdir -p "$(dirname "$STATUS_FILE")"
  printf '{"status":"%s","message":"%s","version":"%s","updatedAt":"%s"}\n' "$1" "$2" "$TAG" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$STATUS_FILE"
}
trap 'write_status error "Docker update failed"' EXIT
write_status preparing "Preparing TimeTone $TAG"
write_status downloading "Downloading the TimeTone Docker image"
node - "$IMAGE_URL" "$IMAGE_ARCHIVE" <<'NODE'
const fs = require("node:fs");
const { Readable } = require("node:stream");
const { pipeline } = require("node:stream/promises");

const [url, output] = process.argv.slice(2);
const partial = `${output}.part`;
const controller = new AbortController();
const timeout = setTimeout(() => controller.abort(), 120000);

(async () => {
  try {
    const response = await fetch(url, { redirect: "follow", signal: controller.signal });
    if (!response.ok || !response.body) throw new Error(`Release image download failed (${response.status})`);
    await pipeline(Readable.fromWeb(response.body), fs.createWriteStream(partial, { flags: "wx" }));
    if (fs.statSync(partial).size === 0) throw new Error("Release image download was empty");
    fs.renameSync(partial, output);
  } catch (error) {
    fs.rmSync(partial, { force: true });
    throw error;
  } finally {
    clearTimeout(timeout);
  }
})().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
NODE
write_status loading "Loading the TimeTone Docker image"
docker load -i "$IMAGE_ARCHIVE"
docker image inspect timetone:release >/dev/null
mkdir -p "$EXTRACTED"
tar -xzf "$ARCHIVE" -C "$EXTRACTED"
SOURCE=$(find "$EXTRACTED" -mindepth 1 -maxdepth 1 -type d | head -n 1)
[ -n "$SOURCE" ] || { echo "Release archive contained no source directory" >&2; exit 1; }
[ -f "$CURRENT_ROOT/web/.env" ] && cp "$CURRENT_ROOT/web/.env" "$SOURCE/web/.env"
rm -rf "$SOURCE/web/data" "$SOURCE/web/node_modules" "$SOURCE/web/.next"
write_status preparing "Preparing TimeTone $TAG"
# Preserve the checked-out install directory and persistent named volume while
# replacing the application source used by the compose project.
cp -a "$SOURCE"/. "$CURRENT_ROOT"/
write_status restarting "Restarting the TimeTone Docker service"
# Let the API response reach the browser before Compose recreates the service.
sleep 3
(cd "$CURRENT_ROOT/web" && docker compose up -d --no-build)
trap - EXIT
write_status complete "TimeTone $TAG is ready"
case "$STAGE" in
  "$CURRENT_ROOT"/.timetone-update-*) rm -rf "$STAGE" ;;
  *) echo "Refusing to remove unexpected staging directory: $STAGE" >&2 ;;
esac
