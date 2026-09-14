import fs from "node:fs";
import path from "node:path";
import { requireAuth } from "@/lib/auth";

export const runtime = "nodejs";

const repository = "PolderLabsVOF/TimeTone";
function currentVersion() {
  for (const candidate of [path.join(process.cwd(), "..", "VERSION"), path.join(process.cwd(), "VERSION"), path.join(process.cwd(), "package.json")]) {
    try {
      const raw = fs.readFileSync(candidate, "utf8");
      if (candidate.endsWith("package.json")) return (JSON.parse(raw) as { version?: string }).version || "0.1.0";
      return raw.trim().replace(/^v/, "");
    } catch { /* try the next deployment layout */ }
  }
  return "0.1.0";
}
function versionParts(version: string) { return version.replace(/^v/, "").split(/[+-]/)[0].split(".").map((part) => Number(part) || 0); }
function isNewer(latest: string, current: string) {
  const a = versionParts(latest), b = versionParts(current);
  return a.some((value, index) => value !== b[index] && value > b[index]) || (a[0] === b[0] && a[1] === b[1] && a[2] > b[2]);
}

export async function GET() {
  await requireAuth();
  const current = currentVersion();
  try {
    const response = await fetch(`https://api.github.com/repos/${repository}/releases/latest`, { headers: { Accept: "application/vnd.github+json", "User-Agent": "TimeTone-updater" }, signal: AbortSignal.timeout(8000), cache: "no-store" });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    const release = await response.json() as { tag_name?: string; name?: string; html_url?: string; body?: string; published_at?: string; draft?: boolean; prerelease?: boolean };
    const latest = String(release.tag_name || "").replace(/^v/, "");
    if (!latest) throw new Error("GitHub release has no version tag");
    return Response.json({ current, latest, updateAvailable: isNewer(latest, current), name: release.name || `TimeTone ${latest}`, url: release.html_url, notes: release.body || "", publishedAt: release.published_at || null });
  } catch (error) {
    return Response.json({ current, error: error instanceof Error ? error.message : "Unable to reach GitHub." }, { status: 502 });
  }
}
