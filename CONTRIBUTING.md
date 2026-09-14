# Contributing to TimeTone

TimeTone is a coordinated release of two artifacts that ship together: the
**firmware** for the ESP32-CYD office terminal, and the **web dashboard** that
stores and reports the time entries. A change that crosses that boundary — a new
device setting, a sync or OTA protocol change — has to land in both or it ships
broken.

## Repository layout

| Path | What it is |
| --- | --- |
| `firmware/` | ESP-IDF project for the CYD terminal. Version in `firmware/main/timekeep.h`. |
| `web/` | Next.js dashboard (App Router, TypeScript, SQLite). |
| `install.sh` | Public installer and updater. Consumes published release assets. |
| `scripts/Dockerfile.release` | Release image; targets `build`, `runtime`, `artifact`. |
| `.github/workflows/ci.yml` | Build and test on every push and PR. |
| `.github/workflows/release.yml` | Tag-driven asset build and publication. |
| `VERSION` | Version source of truth for the whole release. |
| `docs/` | Architecture, API, deployment, hardware, and versioning notes. |

## Local setup

### Dashboard

The dashboard is a Next.js 16 app on Node 24. Its npm dependencies are resolved
through **Deno**, not npm — there is a `web/deno.lock` and deliberately no
`package-lock.json`.

```sh
deno install --allow-scripts=npm:better-sqlite3
```

That install runs once from the `web/` directory. After it:

```sh
npm --prefix web run dev     # dev server
npm --prefix web test        # vitest, run once
npm --prefix web run lint    # eslint
npm --prefix web run build   # production build
```

Copy `web/.env.example` to `web/.env` and set real values before running. Note
that `ADMIN_SECRET` must be at least 32 random bytes, and that
`TIMETONE_INSTALL_MODE` is normally written by `install.sh` rather than by hand.

### Firmware

The firmware builds with ESP-IDF v6.0.1 — the same container CI uses:

```sh
docker run --rm -v "$PWD:/project" -w /project/firmware \
  espressif/idf:v6.0.1 idf.py build
```

The artifact lands at `firmware/build/timetone.bin`.

`firmware/build/` is generated and is not tracked. Because CMake caches
absolute paths, a build directory created by a *different* mount point or
container layout makes later builds fail with a cache error that does not
mention the real cause. Remove the directory rather than debugging the cache:

```sh
docker run --rm -v "$PWD:/project" -w /project/firmware espressif/idf:v6.0.1 \
  sh -c 'rm -rf /project/firmware/build'
```

The same applies if an earlier container run left root-owned files behind —
delete the directory from inside the container so the ownership is not an
obstacle.

## Branch model

Three long-lived branches. Promotion only ever moves **forward**:

```
feature/* ──PR──> dev ──PR──> beta ──PR──> main
```

| Branch | Role |
| --- | --- |
| `dev` | Integration branch. **The repository default.** Feature and fix branches target this. |
| `beta` | Release-candidate validation. Receives promotion PRs from `dev`. |
| `main` | Stable. Receives promotion PRs from `beta`. Every commit is released or release-ready. |

Branch off `dev` for feature work:

```sh
git switch dev && git pull
git switch -c feature/device-removal
```

**Never push directly to `main` or `beta`.** Changes reach them only through a
pull request.

### Hotfixes

Branch from `main` so the fix is based on what users actually run, then
back-merge so the lanes do not drift apart:

```sh
git switch main && git pull
git switch -c hotfix/ota-retry
# fix, PR into main
```

After the hotfix merges, merge `main` into `beta`, then `beta` into `dev`.

## Commits

[Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): description
```

Types in use: `feat`, `fix`, `chore`, `docs`, `style`, `refactor`, `perf`,
`test`, `ci`. Scope is the area — `installer`, `web`, `firmware`, `release`,
`api`. Examples from this repository:

```
fix(installer): show branded header before update handoff
chore(release): bump version to 0.3.2
ci: support the dev/beta/main lanes and pre-release tags
```

Keep each commit one logical change. A commit that both refactors and fixes is
two commits that were accidentally merged.

## Pull requests

- Target `dev` for feature and fix work. Target `main` or `beta` only for
  promotions and hotfixes.
- CI must be green. The `web` and `firmware` jobs are both required.
- One approval from a maintainer before merge.
- Do **not** force-push or rebase a branch that other people have based work on.
- Never commit `web/.env`, credentials, tokens, or generated build output.

## Definition of done

- [ ] Behavior implemented, with a regression test that fails without the change.
- [ ] `npm --prefix web test`, `npm --prefix web run lint`, and
      `npm --prefix web run build` all pass.
- [ ] The firmware builds when `firmware/` changed.
- [ ] Docs updated when you changed a documented surface.
- [ ] `CHANGELOG.md` updated for anything user-visible, under the version being
      released.

## Versioning

`VERSION` at the repository root is the single source of truth. It must match
`web/package.json` and `firmware/main/timekeep.h` (`TK_FIRMWARE_VERSION`) — a
mismatch fails the release workflow before anything is built. The full policy,
including per-lane pre-release suffixes, is in
[docs/VERSIONING.md](docs/VERSIONING.md).

## A note on branch protection

The rules above — pull requests required for `main` and `beta`, CI required,
force-pushes and deletions disallowed — are the **intended policy**, and they
are enforced socially, not by the platform. Repository branch protection is not
currently configured. A maintainer with admin rights must enable it in the
repository's *Settings → Branches* before those rules are mechanically
guaranteed.
