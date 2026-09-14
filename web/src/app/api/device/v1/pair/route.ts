import crypto from "node:crypto";
import { z } from "zod";
import { db, sha256 } from "@/lib/db";
import { publishLiveUpdate } from "@/lib/live-updates";

const schema = z.object({
  deviceName: z.string().trim().min(2).max(80),
  token: z.string().trim().min(12).max(128),
  firmwareVersion: z.string().max(40).optional(),
  ipAddress: z.string().max(64).optional(),
});

export async function POST(request: Request) {
  const parsed = schema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return Response.json({ error: "Invalid pairing payload" }, { status: 400 });
  const { deviceName, token, firmwareVersion, ipAddress } = parsed.data;
  const digest = sha256(token);
  const existing = db.prepare("SELECT * FROM devices WHERE token_digest = ?").get(digest) as {
    id: string;
    approved: number;
  } | undefined;
  if (existing) {
    db.prepare("UPDATE devices SET name = ?, firmware_version = ?, ip_address = ? WHERE id = ?")
      .run(deviceName, firmwareVersion || null, ipAddress || null, existing.id);
    publishLiveUpdate("device");
    return Response.json(
      { ok: true, approved: existing.approved === 1, deviceId: existing.id },
      { status: existing.approved === 1 ? 200 : 202 },
    );
  }
  const id = crypto.randomUUID();
  db.prepare("INSERT INTO devices (id, name, token_digest, last_seen_at, firmware_version, ip_address, pending_events, created_at, approved) VALUES (?, ?, ?, ?, ?, ?, 0, ?, 0)")
    .run(id, deviceName, digest, new Date().toISOString(), firmwareVersion || null, ipAddress || null, new Date().toISOString());
  publishLiveUpdate("device");
  return Response.json({ ok: true, approved: false, deviceId: id }, { status: 202 });
}
