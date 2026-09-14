import fsSync from "node:fs";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { promisify } from "node:util";
import { requireAuth } from "@/lib/auth";

export const runtime = "nodejs";
const repository = "PolderLabsVOF/TimeTone";

function statusPath() {
  // Docker updates are run by a short-lived sidecar because recreating the
  // dashboard container would otherwise terminate its own updater. Store
  // progress in the checkout mounted at /host, where both generations of the
  // dashboard and the sidecar can read it.
  if (process.env.TIMETONE_INSTALL_MODE === "docker" && fsSync.existsSync("/host/web")) {
    return "/host/web/.timetone-update-status.json";
  }
  const databasePath = process.env.DATABASE_PATH || path.join(process.cwd(), "data", "timekeep.db");
  return path.join(path.dirname(databasePath), "update-status.json");
}

function installRoot(docker: boolean) {
  if (process.env.TIMETONE_UPDATE_ROOT) return process.env.TIMETONE_UPDATE_ROOT;
  const candidates = docker
    ? ["/host", path.resolve(process.cwd(), "../.."), path.resolve(process.cwd(), "..")]
    : [path.resolve(process.cwd(), "../.."), path.resolve(process.cwd(), "../../.."), path.resolve(process.cwd(), "..")];
  return candidates.find((candidate) => {
    try { return fsSync.existsSync(path.join(candidate, "scripts", docker ? "install-release-docker.sh" : "install-release.sh")); } catch { return false; }
  }) || candidates[0];
}

async function writeStatus(status: string, message: string, version?: string) {
  await fs.mkdir(path.dirname(statusPath()), { recursive: true });
  await fs.writeFile(statusPath(), JSON.stringify({ status, message, version: version || null, updatedAt: new Date().toISOString() }));
}

async function dockerRuntime(root: string) {
  const { execFile } = await import("node:child_process");
  const output = await promisify(execFile)("docker", ["compose", "ps", "-q", "timekeep"], {
    cwd: path.join(root, "web"),
  });
  const container = output.stdout.trim().split("\n")[0];
  if (!container) throw new Error("Could not identify the running Docker container.");
  const inspection = await promisify(execFile)("docker", ["inspect", container]);
  const details = JSON.parse(inspection.stdout) as Array<{
    Config?: { Image?: string };
    Mounts?: Array<{ Type?: string; Source?: string; Destination?: string }>;
  }>;
  const current = details[0];
  const hostRoot = current?.Mounts?.find((mount) => mount.Type === "bind" && mount.Destination === "/host")?.Source;
  if (!hostRoot) throw new Error("The running Docker container has no host checkout mounted at /host.");
  const image = current?.Config?.Image;
  if (!image) throw new Error("Could not identify the running Docker image.");
  return { hostRoot, image };
}

export async function POST(request: Request) {
  try {
    await requireAuth();
    const body = await request.json().catch(() => ({})) as { tag?: string };
    const tag = String(body.tag || "").match(/^v?\d+\.\d+\.\d+$/)?.[0];
    if (!tag) return Response.json({ error: "Choose a valid release version first." }, { status: 400 });
    const releaseTag = tag.startsWith("v") ? tag : `v${tag}`;
    const response = await fetch(`https://api.github.com/repos/${repository}/releases/tags/${releaseTag}`, { headers: { Accept: "application/vnd.github+json", "User-Agent": "TimeTone-updater" }, signal: AbortSignal.timeout(10000), cache: "no-store" });
    if (!response.ok) return Response.json({ error: `GitHub release lookup failed (${response.status}).` }, { status: 502 });
    const release = await response.json() as {
      tarball_url?: string;
      draft?: boolean;
      prerelease?: boolean;
      assets?: Array<{ name?: string; browser_download_url?: string }>;
    };
    if (release.draft || release.prerelease) return Response.json({ error: "Only published stable releases can be installed." }, { status: 400 });
    const docker = process.env.TIMETONE_INSTALL_MODE === "docker";
    // Native installations never build on the user's server. A stable release
    // must carry the production bundle assembled by the release workflow.
    const nativeAsset = release.assets?.find((asset) => asset.name === "timetone-web.tar.gz" && asset.browser_download_url);
    if (!docker && !nativeAsset?.browser_download_url) {
      return Response.json({ error: "This release has no prebuilt web bundle. Choose a release with timetone-web.tar.gz." }, { status: 400 });
    }
    const archiveUrl = docker ? release.tarball_url : nativeAsset?.browser_download_url;
    if (!archiveUrl) return Response.json({ error: "Release archive is unavailable." }, { status: 502 });
    const archiveResponse = await fetch(archiveUrl, { headers: { Accept: "application/octet-stream", "User-Agent": "TimeTone-updater" }, signal: AbortSignal.timeout(30000), cache: "no-store" });
    if (!archiveResponse.ok) return Response.json({ error: `Could not download the release archive (${archiveResponse.status}).` }, { status: 502 });
    const root = installRoot(docker);
    const script = path.join(root, "scripts", docker ? "install-release-docker.sh" : "install-release.sh");
    if (!fsSync.existsSync(script)) return Response.json({ error: `Update helper not found at ${script}.` }, { status: 500 });
    const runtime = docker ? await dockerRuntime(root) : null;
    // A Docker updater must survive the dashboard container it replaces, so
    // stage its archive in the host checkout instead of the container /tmp.
    const stage = await fs.mkdtemp(path.join(docker ? root : os.tmpdir(), ".timetone-update-"));
    const archive = path.join(stage, docker ? "release.tar.gz" : "timetone-web.tar.gz");
    await writeStatus("downloading", `Downloading TimeTone ${tag.replace(/^v/, "")}…`, tag.replace(/^v/, ""));
    await fs.writeFile(archive, Buffer.from(await archiveResponse.arrayBuffer()));
    const { spawn } = await import("node:child_process");
    // The helper stops the current service as part of an in-place update. Let
    // this response leave Next.js and pass through the reverse proxy first;
    // otherwise Caddy/Cloudflare sees the origin disappear and reports a 502
    // even though the update has already started.
    await writeStatus("queued", `TimeTone ${tag.replace(/^v/, "")} is queued; the server will restart shortly.`, tag.replace(/^v/, ""));
    const args = docker
      ? [
          "run", "--rm", "--name", `timetone-updater-${Date.now()}`,
          "-v", "/var/run/docker.sock:/var/run/docker.sock",
          "-v", `${runtime!.hostRoot}:/host`,
          "-e", `TIMETONE_HOST_ROOT=${runtime!.hostRoot}`,
          runtime!.image,
          "sh", "/host/scripts/install-release-docker.sh", "/host",
          path.posix.join("/host", path.relative(root, stage)), releaseTag,
        ]
      : ["-c", "sleep 3; exec \"$@\"", "timetone-update", script, root, stage, releaseTag];
    const child = spawn(docker ? "docker" : "sh", args, {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, TIMETONE_UPDATE_ROOT: root, TIMETONE_UPDATE_STATUS: statusPath() },
    });
    child.once("error", () => { void writeStatus("error", "Unable to start the update helper.", tag.replace(/^v/, "")); });
    child.unref();
    return Response.json({ ok: true, version: tag.replace(/^v/, ""), message: docker ? "Update downloaded. Docker is installing the release now." : "Update downloaded. The prebuilt server will restart automatically." });
  } catch (error) {
    const message = error instanceof Error ? error.message : "Unexpected update error";
    return Response.json({ error: `Update could not start: ${message}` }, { status: 500 });
  }
}
