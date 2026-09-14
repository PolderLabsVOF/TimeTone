# Versioning and releases

TimeTone uses Semantic Versioning (`MAJOR.MINOR.PATCH`) for coordinated
dashboard and terminal releases. The repository version is recorded in the
root `VERSION` file; the dashboard package and terminal firmware report the
same version.

- **MAJOR**: incompatible API, data, or device changes requiring migration.
- **MINOR**: backwards-compatible features.
- **PATCH**: backwards-compatible fixes, security updates, and documentation.

## One version, three files

`VERSION` is the source of truth. These must agree with it:

| File | Field |
| --- | --- |
| `VERSION` | the bare version string |
| `web/package.json` | `version` |
| `firmware/main/timekeep.h` | `TK_FIRMWARE_VERSION` |

A mismatch fails the release workflow at its first step, before any asset is
built. If you see that failure, the message prints both the tag and the
`VERSION` value it was compared against.

## Version per branch lane

Branches promote forward — `dev` → `beta` → `main` — and each lane carries the
version appropriate to its stage:

| Branch | `VERSION` holds | Tag | Release channel |
| --- | --- | --- | --- |
| `dev` | `X.Y.Z-dev` | none | not published |
| `beta` | `X.Y.Z-rc.N` | `vX.Y.Z-rc.N` | GitHub **pre-release** |
| `main` | `X.Y.Z` | `vX.Y.Z` | GitHub **release** |

`VERSION` is re-edited in each promotion PR — the `dev`→`beta` PR sets the
`-rc.N` suffix, and the `beta`→`main` PR strips it. This is intentional and
expected, not a merge conflict to engineer away.

### Why pre-releases are safe to publish

GitHub's `releases/latest` API endpoint **excludes pre-releases**. The installer
resolves the version it installs from that endpoint, so a published `-rc` or
`-dev` release is invisible to it and users on stable keep receiving stable
versions. That property is load-bearing: anything that changes how the
installer picks a version has to preserve it.

### Tags are bound to their lane

The release workflow verifies that the tagged commit is reachable from the lane
the channel implies — a clean tag must be contained in `origin/main`, a suffixed
tag in `origin/beta`. Tagging a commit that has not been promoted to the
matching branch fails the release.

## Release checklist

Every release must:

1. Update `VERSION`, `web/package.json`, `firmware/main/timekeep.h`, and
   `CHANGELOG.md`.
2. Run the web tests, lint, production build, and firmware build. Type checking
   runs as part of the production build; there is no separate script for it.
3. Commit the changes, create an annotated `vMAJOR.MINOR.PATCH` tag, and push
   the branch and tag. Suffix the tag for pre-releases (`v0.4.0-rc.1`).
4. Publish a GitHub release using the matching changelog section with
   `timetone-web.tar.gz`, `timetone-docker.tar.gz`,
   `timetone-VERSION.bin`, and `SHA256SUMS`. The web archive includes
   the tracked installation files and `web/.next/standalone`, with static
   assets and public files inside that runtime directory.

The tag-triggered release workflow builds all assets before publication.
It does not overwrite an existing release — if a release already exists for the
tag, the built assets are uploaded to it and it is published rather than
recreated. Native and Docker installs consume these assets without compiling
locally. Web binaries target Linux x86-64; the native runtime requires Node.js
24 and glibc 2.36 or newer.

Pre-release identifiers such as `-rc.1` are allowed for testing and must not
be used as the production version reported by a terminal.

## Changelog format

Sections are newest-first and dated:

```markdown
## [0.4.0] - 2026-09-14

- Describe the user-visible change, not the implementation.
```

Write entries for behavior a user or operator would notice. A refactor with no
behavior change does not need one.
