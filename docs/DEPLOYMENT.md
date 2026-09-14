# Deploying and operating TimeTone

## Recommended install

From a trusted release checkout on a Linux host:

```bash
./install.sh
```

The script is interactive by default: choose Docker or native Node.js
installation, then provide the password, timezone, and port. It creates
`web/.env` with a random 256-bit session secret and starts the dashboard. It
never sends configuration or attendance data outside your host. Re-run it to
rebuild after pulling an update; choose to keep the existing configuration when
prompted. Native installs require Node.js 20.9+ and npm and write logs to
`web/timetone.log`. The installer registers a systemd service named
`timetone.service` and starts it at boot. Regular-user installs use a user
service with lingering; root installs use a system service. Inspect its status
with `systemctl status timetone.service` for root installs or
`systemctl --user status timetone.service` for regular-user installs. If the
host does not run systemd, use Docker mode or arrange an equivalent service
manager yourself.

On Debian/Ubuntu, the installer can install missing system dependencies using
`apt-get` (Docker Engine and Compose for Docker mode, Node.js 24 and npm for
native mode). On other distributions, install the selected runtime and `tar`,
`sed`, `find`, `curl`, and `openssl` before running it.

For automation, use `./install.sh --docker --non-interactive` or
`./install.sh --native --non-interactive` with `TIMETONE_ADMIN_PASSWORD` set.

## Docker Compose

```bash
cd web
cp .env.example .env
# Set ADMIN_PASSWORD, ADMIN_SECRET and TIMEKEEP_TIMEZONE.
# Keep COOKIE_SECURE=false for plain HTTP on a trusted LAN; set it true once
# the application is available through HTTPS.
# Then:
docker compose up -d --build
```

The named `timekeep-data` volume contains the SQLite database. Put the service
behind a TLS reverse proxy such as Caddy, nginx, or Traefik before exposing it
beyond a trusted LAN. Web Serial firmware updates require a secure browser
context, which means HTTPS or `localhost`. The ESP firmware uses the standard
certificate bundle and expects a publicly trusted HTTPS certificate in
production.

## Backup

SQLite uses WAL mode. Use SQLite's online backup command instead of copying a
busy database file:

```bash
docker compose exec timekeep sqlite3 /data/timekeep.db \
  ".backup '/data/timekeep-backup.db'"
```

Copy the backup out of the volume and test restoration periodically.

To restore, stop TimeTone, replace `/data/timekeep.db` from a tested backup,
remove any matching `-wal`/`-shm` sidecar files, and start the service again.
Keep an encrypted, off-host backup; the database contains attendance data.

## Security checklist

- Replace the development admin password and session secret. Employee color
  sequences are created in the protected web UI.
- Do not share `web/.env` or the Docker volume. The application creates a
  separate device credential during terminal pairing.
- Terminate TLS before exposing the app or configuring an ESP.
- Restrict dashboard access with a VPN or identity-aware proxy when practical.
- Protect backups: they contain employee names, attendance, and email addresses.
- Review manual corrections and CSV exports as personal data under applicable
  employment and privacy law.

## Updating

From **Settings → Software updates**, use **Check for updates** to query the
latest stable GitHub release. Native installs can install the selected release
directly; the updater preserves `.env` and the SQLite data directory, builds the
new version, and restarts the service. Keep the previous install directory as
your rollback copy until you have verified the update.

For testing unreleased code, run the installer from the `dev` branch with
`--dev --native`. It downloads the current branch (or the SHA in
`TIMETONE_SOURCE_REF`) and builds the standalone runtime locally:

```bash
curl -fsSL https://raw.githubusercontent.com/PolderLabsVOF/TimeTone/dev/install.sh | sh -s -- --dev --native
```

The matching firmware image is retained as a CI artifact on the same dev
commit. Download that artifact and use **Devices → USB firmware update**; this
does not require a GitHub release.

Docker installs show the available release but must be updated from the host:
download the release, replace the checkout, and rerun `./install.sh --docker`
(or run `docker compose up -d --build` in `web/`). Docker preserves the named
database volume.

### Updating firmware

For unattended updates, open **Devices**. Approved terminals with an older
firmware version show the latest stable GitHub release and a **Start terminal
update** button. Starting an update queues the release application image for
that terminal; it downloads the image over HTTPS on the next online config
sync, verifies it with ESP-IDF OTA, and reboots. The device reports its new
version on the next heartbeat and the queue is cleared automatically.

Use **Devices → USB firmware update** in a supported browser as a recovery or
first-flash path. Select the `timetone.bin` image from a compatible release or
build; existing terminal configuration is preserved.
