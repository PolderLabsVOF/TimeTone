import { z } from "zod";
import { authenticateDevice, unauthorized } from "@/lib/device-api";
import { db } from "@/lib/db";
import { publishLiveUpdate } from "@/lib/live-updates";

export const dynamic = "force-dynamic";

const schema = z.object({
  firmwareVersion: z.string().max(40),
  ipAddress: z.string().max(64).optional(),
  pendingEvents: z.number().int().min(0).max(10000),
});

export async function POST(request: Request) {
  const device = authenticateDevice(request);
  if (!device) return unauthorized();
  const parsed = schema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return Response.json({ error: "Invalid payload" }, { status: 400 });
  }
  const syncRequested = !!device.sync_requested_at;
  db.prepare(
    "UPDATE devices SET last_seen_at = ?, firmware_version = ?, ip_address = ?, pending_events = ?, ota_version = CASE WHEN ota_version = ? THEN NULL ELSE ota_version END, ota_url = CASE WHEN ota_version = ? THEN NULL ELSE ota_url END, ota_requested_at = CASE WHEN ota_version = ? THEN NULL ELSE ota_requested_at END WHERE id = ?",
  ).run(
    new Date().toISOString(),
    parsed.data.firmwareVersion,
    parsed.data.ipAddress || null,
    parsed.data.pendingEvents,
    parsed.data.firmwareVersion,
    parsed.data.firmwareVersion,
    parsed.data.firmwareVersion,
    device.id,
  );
  publishLiveUpdate("device");
  return Response.json(
    { ok: true, serverTime: new Date().toISOString(), configRefresh: syncRequested },
    { headers: { "Cache-Control": "no-store, no-cache, must-revalidate" } },
  );
}
