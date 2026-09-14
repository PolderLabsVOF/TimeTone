# Getting started with TimeTone

## 1. Install the dashboard

For a supported self-hosted installation, run the latest interactive installer
directly from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/PolderLabsVOF/TimeTone/main/install.sh | sh
```

It downloads the repository into `./TimeTone`, asks whether to use Docker or a
native Node.js install, then asks for an admin password, timezone and port. Open
the printed LAN URL and sign in. To use an existing checkout, run
`./install.sh`; explicit modes are `./install.sh --docker` and
`./install.sh --native`.

For local development instead, the web application requires Node.js 24 and uses Deno for the checked-in lock
file in this development environment.

```bash
cd web
cp .env.example .env
```

Set secure values in `.env`. `ADMIN_SECRET` should contain at least 32 random
bytes. Then run:

```bash
deno install --allow-scripts=npm:better-sqlite3
npm run dev
```

Open `http://localhost:3000`. Add an employee and choose a colour sequence in
**Employees**, then approve the terminal in **Devices**.

## 2. Build and flash the terminal

```bash
cd firmware
source /home/drb0rk/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Find a different serial port with `python -m serial.tools.list_ports` when
necessary. On Linux, the account may need membership of the `uucp` or `dialout`
group depending on the distribution.

## 3. Provision it

After first boot the display shows the setup SSID. Join
`TimeTone-XXXX` using password `timekeep`, then browse to
`http://192.168.4.1`. Enter:

- the office 2.4 GHz Wi-Fi network;
- the externally reachable server URL, without a trailing slash;

The board generates and retains its own device credential; no token needs to be
entered by an administrator. It restarts, synchronises time and employees, and enables the keypad.
If the saved Wi-Fi is unavailable for 20 seconds, the setup network returns.

## 4. Clock time

Choose exactly four colors; the terminal submits automatically after the fourth tap. The same action toggles between clock-in
and clock-out. The event is stored before sync, so a short server or Wi-Fi
outage does not lose it. The dashboard calculates rounded time per completed
session while retaining exact timestamps.

## 5. Update over USB

For an existing terminal, open **Devices** in Chrome or Edge on desktop using
an HTTPS dashboard URL (or `localhost`). In **USB firmware update**, select the
`timetone.bin` built by ESP-IDF or supplied by a TimeTone release, connect the
terminal by USB, and choose **Flash connected terminal**. The updater writes
only the application partition at `0x20000`, so saved Wi-Fi and server settings
are preserved. A factory-fresh board still needs the full ESP-IDF flash command
from step 2.

## Verification commands

```bash
cd web
npm test
npm run lint
npm run build

cd ../firmware
idf.py build
```
