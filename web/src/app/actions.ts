"use server";

import crypto from "node:crypto";
import { cookies } from "next/headers";
import { redirect } from "next/navigation";
import { revalidatePath } from "next/cache";
import { z } from "zod";
import { createSession, requireAuth, SESSION_COOKIE, verifyAdminPassword } from "@/lib/auth";
import { db, logTimeEntryChange, runTimeMaintenance, sha256 } from "@/lib/db";

const employeeSchema = z.object({
  name: z.string().trim().min(2).max(80),
  email: z.string().trim().email().or(z.literal("")),
  role: z.string().trim().max(80),
  code: z.string().regex(/^[ABCD]{4}$/),
  color: z.string().regex(/^#[0-9A-Fa-f]{6}$/),
});
const employeeUpdateSchema = employeeSchema.extend({
  id: z.string().min(1),
  code: z.string().regex(/^[ABCD]{4}$/).or(z.literal("")),
});

export async function login(formData: FormData) {
  const password = String(formData.get("password") || "");
  if (!verifyAdminPassword(password)) {
    redirect("/login?error=1");
  }
  const store = await cookies();
  store.set(SESSION_COOKIE, await createSession(), {
    httpOnly: true,
    sameSite: "lax",
    // Plain HTTP is useful for a trusted LAN install. Enable explicitly when
    // the service is served through HTTPS (see COOKIE_SECURE in .env.example).
    secure: process.env.COOKIE_SECURE === "true",
    path: "/",
    maxAge: 43200,
  });
  redirect("/");
}

export async function logout() {
  const store = await cookies();
  store.delete(SESSION_COOKIE);
  redirect("/login");
}

export async function createEmployee(formData: FormData) {
  await requireAuth();
  const data = employeeSchema.parse(Object.fromEntries(formData));
  const now = new Date().toISOString();
  db.prepare("INSERT INTO employees VALUES (?, ?, ?, ?, ?, 1, ?, ?, ?)").run(
    crypto.randomUUID(),
    data.name,
    data.email || null,
    data.role || null,
    sha256(data.code),
    data.color,
    now,
    now,
  );
  revalidatePath("/employees");
  revalidatePath("/");
}

export async function toggleEmployee(formData: FormData) {
  await requireAuth();
  const id = String(formData.get("id"));
  db.prepare(
    "UPDATE employees SET active = CASE active WHEN 1 THEN 0 ELSE 1 END, updated_at = ? WHERE id = ?",
  ).run(new Date().toISOString(), id);
  revalidatePath("/employees");
}

export async function updateEmployee(formData: FormData) {
  await requireAuth();
  const data = employeeUpdateSchema.parse(Object.fromEntries(formData));
  const now = new Date().toISOString();
  if (data.code) {
    db.prepare("UPDATE employees SET name = ?, email = ?, role = ?, code_digest = ?, color = ?, updated_at = ? WHERE id = ?")
      .run(data.name, data.email || null, data.role || null, sha256(data.code), data.color, now, data.id);
  } else {
    db.prepare("UPDATE employees SET name = ?, email = ?, role = ?, color = ?, updated_at = ? WHERE id = ?")
      .run(data.name, data.email || null, data.role || null, data.color, now, data.id);
  }
  revalidatePath("/employees");
  revalidatePath("/");
}

export async function approveDevice(formData: FormData) {
  await requireAuth();
  const id = z.string().min(1).parse(formData.get("id"));
  db.prepare("UPDATE devices SET approved = 1 WHERE id = ?").run(id);
  revalidatePath("/devices");
  revalidatePath("/");
}

export async function rejectDevice(formData: FormData) {
  await requireAuth();
  const id = z.string().min(1).parse(formData.get("id"));
  db.prepare("DELETE FROM devices WHERE id = ? AND approved = 0").run(id);
  revalidatePath("/devices");
}

export async function deleteDevice(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  db.transaction(() => {
    if (!db.prepare("SELECT 1 FROM devices WHERE id = ?").get(id)) throw new Error("Device not found");
    db.prepare("DELETE FROM device_events WHERE device_id = ?").run(id);
    db.prepare("DELETE FROM devices WHERE id = ?").run(id);
  })();
  revalidatePath("/devices");
  revalidatePath("/events");
  revalidatePath("/");
}

export async function renameDevice(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  const name = z.string().trim().min(2).max(80).parse(formData.get("name"));
  db.prepare("UPDATE devices SET name = ? WHERE id = ?").run(name, id);
  revalidatePath("/devices");
  revalidatePath("/");
}

export async function saveDeviceSettings(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  const screenOffTimeout = z.coerce.number().int().min(0).max(3600).parse(formData.get("screen_off_timeout_seconds"));
  const lowPowerTimeout = z.coerce.number().int().min(0).max(3600).parse(formData.get("low_power_timeout_seconds"));
  if (lowPowerTimeout && screenOffTimeout && lowPowerTimeout < screenOffTimeout) throw new Error("Low-power timeout must be longer than screen-off timeout");
  const syncInterval = z.coerce.number().int().min(2).max(60).parse(formData.get("sync_interval_seconds"));
  const fullSyncInterval = z.coerce.number().int().min(30).max(3600).parse(formData.get("full_sync_interval_seconds"));
  const terminalTheme = z.enum(["light", "dark"]).parse(formData.get("terminal_theme"));
  db.prepare("UPDATE devices SET screen_off_timeout_seconds = ?, low_power_timeout_seconds = ?, sync_interval_seconds = ?, full_sync_interval_seconds = ?, terminal_theme = ?, sync_requested_at = ? WHERE id = ?")
    .run(screenOffTimeout, lowPowerTimeout, syncInterval, fullSyncInterval, terminalTheme, new Date().toISOString(), id);
  revalidatePath("/devices");
}

export async function requestDeviceSync(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  db.prepare("UPDATE devices SET sync_requested_at = ? WHERE id = ? AND approved = 1")
    .run(new Date().toISOString(), id);
  revalidatePath("/devices");
}

export async function requestFirmwareUpdate(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  const response = await fetch("https://api.github.com/repos/PolderLabsVOF/TimeTone/releases/latest", { headers: { Accept: "application/vnd.github+json", "User-Agent": "TimeTone-dashboard" }, signal: AbortSignal.timeout(10000), cache: "no-store" });
  if (!response.ok) throw new Error("Unable to check GitHub releases");
  const release = await response.json() as { tag_name?: string; assets?: Array<{ name?: string; browser_download_url?: string }> };
  const version = String(release.tag_name || "").replace(/^v/, "");
  const asset = release.assets?.find((candidate) => candidate.name?.toLowerCase().endsWith(".bin") && candidate.browser_download_url);
  if (!/^\d+\.\d+\.\d+$/.test(version) || !asset?.browser_download_url) throw new Error("The latest release has no compatible firmware image");
  db.prepare("UPDATE devices SET ota_version = ?, ota_url = ?, ota_requested_at = ? WHERE id = ? AND approved = 1")
    .run(version, asset.browser_download_url, new Date().toISOString(), id);
  revalidatePath("/devices");
}

export async function saveSettings(formData: FormData) {
  await requireAuth();
  const values = {
    company_name: z.string().trim().min(2).max(80).parse(
      formData.get("company_name"),
    ),
    timezone: z.string().trim().min(3).max(80).parse(formData.get("timezone")),
    rounding_minutes: z.enum(["1", "5", "10", "15", "30"]).parse(
      formData.get("rounding_minutes"),
    ),
    rounding_mode: z.enum(["nearest", "up", "down"]).parse(
      formData.get("rounding_mode"),
    ),
    auto_merge_enabled: z.enum(["true", "false"]).parse(formData.get("auto_merge_enabled")),
    auto_merge_minutes: z.coerce.number().int().min(1).max(120).parse(formData.get("auto_merge_minutes")).toString(),
    auto_close_enabled: z.enum(["true", "false"]).parse(formData.get("auto_close_enabled")),
    max_shift_hours: z.coerce.number().min(1).max(24).parse(formData.get("max_shift_hours")).toString(),
    duplicate_window_seconds: z.coerce.number().int().min(0).max(120).parse(formData.get("duplicate_window_seconds")).toString(),
    default_report_window: z.enum(["7", "14", "30", "60", "90", "365"]).parse(formData.get("default_report_window")),
  };
  const statement = db.prepare(
    "INSERT INTO settings(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
  );
  db.transaction(() =>
    Object.entries(values).forEach(([key, value]) => statement.run(key, value))
  )();
  runTimeMaintenance();
  revalidatePath("/");
  revalidatePath("/settings");
  revalidatePath("/reports");
}

export async function changePassword(formData: FormData) {
  await requireAuth();
  const current = String(formData.get("current_password") || "");
  const next = String(formData.get("new_password") || "");
  const confirmation = String(formData.get("confirm_password") || "");
  if (!verifyAdminPassword(current)) redirect("/settings?password=incorrect");
  if (next.length < 8 || next.length > 128 || next !== confirmation) {
    redirect("/settings?password=invalid");
  }
  db.prepare("INSERT INTO settings(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value")
    .run("admin_password_digest", sha256(next));
  redirect("/settings?password=changed");
}

export async function addManualEntry(formData: FormData) {
  await requireAuth();
  try {
    const employeeIds = [...new Set(z.array(z.string().min(1)).min(1).parse(formData.getAll("employee_ids")))];
    const clockIn = parseEntryDate(formData.get("clock_in"), "Clock-in");
    const clockOutValue = String(formData.get("clock_out") || "");
    const clockOut = clockOutValue ? parseEntryDate(clockOutValue, "Clock-out") : null;
    const note = z.string().max(200).parse(String(formData.get("note") || ""));
    if (clockOut && new Date(clockOut) <= new Date(clockIn)) throw new Error("Clock-out must be after clock-in");
    const now = new Date().toISOString();
    db.transaction(() => {
      for (const employeeId of employeeIds) {
        if (!db.prepare("SELECT 1 FROM employees WHERE id = ?").get(employeeId)) throw new Error("Employee not found");
        if (!clockOut && db.prepare("SELECT 1 FROM time_entries WHERE employee_id = ? AND clock_out IS NULL").get(employeeId)) {
          throw new Error("One or more employees already have an open time entry");
        }
      }
      for (const employeeId of employeeIds) {
        const id = crypto.randomUUID();
        db.prepare("INSERT INTO time_entries VALUES (?, ?, ?, ?, 'manual', ?, ?, ?)")
          .run(id, employeeId, clockIn, clockOut, note || null, now, now);
        logTimeEntryChange(id, "manual_create", null, { employeeId, clockIn, clockOut, note }, "Manual entry created");
      }
    })();
    revalidateTimeEntryViews();
  } catch (error) {
    redirectEntryError(error);
  }
}

export async function updateTimeEntry(formData: FormData) {
  await requireAuth();
  try {
    const id = z.string().min(1).parse(formData.get("id"));
    const employeeId = z.string().min(1).parse(formData.get("employee_id"));
    const clockIn = parseEntryDate(formData.get("clock_in"), "Clock-in");
    const clockOutValue = String(formData.get("clock_out") || "");
    const clockOut = clockOutValue ? parseEntryDate(clockOutValue, "Clock-out") : null;
    const note = z.string().max(200).parse(String(formData.get("note") || ""));
    if (clockOut && new Date(clockOut) <= new Date(clockIn)) throw new Error("Clock-out must be after clock-in");

    db.transaction(() => {
      const before = db.prepare("SELECT * FROM time_entries WHERE id = ?").get(id);
      if (!before) throw new Error("Time entry not found");
      if (!db.prepare("SELECT 1 FROM employees WHERE id = ?").get(employeeId)) throw new Error("Employee not found");
      const otherOpen = db.prepare(
        "SELECT id FROM time_entries WHERE employee_id = ? AND clock_out IS NULL AND id != ?",
      ).get(employeeId, id);
      if (!clockOut && otherOpen) throw new Error("This employee already has an open time entry");
      db.prepare(
        "UPDATE time_entries SET employee_id = ?, clock_in = ?, clock_out = ?, note = ?, updated_at = ? WHERE id = ?",
      ).run(employeeId, clockIn, clockOut, note || null, new Date().toISOString(), id);
      logTimeEntryChange(
        id,
        "manual_edit",
        before,
        { ...(before as Record<string, unknown>), employee_id: employeeId, clock_in: clockIn, clock_out: clockOut, note: note || null },
        clockOut ? "Manual correction from dashboard" : "Manual correction; entry remains open",
      );
    })();
    // A direct correction is authoritative. Do not run automatic merge/close
    // rules here: they can change an open entry immediately after it is saved.
    revalidateTimeEntryViews();
  } catch (error) {
    redirectEntryError(error);
  }
}

function revalidateTimeEntryViews() {
  revalidatePath("/entries");
  revalidatePath("/");
  revalidatePath("/reports");
}

function redirectEntryError(error: unknown): never {
  // Next redirects are implemented as a thrown sentinel and must pass through.
  if (typeof error === "object" && error !== null && "digest" in error && String(error.digest).startsWith("NEXT_REDIRECT")) throw error;
  const message = error instanceof Error && error.message ? error.message : "Unable to save this time entry";
  redirect(`/entries?entryError=${encodeURIComponent(message)}`);
}

export async function deleteTimeEntry(formData: FormData) {
  await requireAuth();
  const id = z.string().uuid().parse(formData.get("id"));
  const entry = db.prepare("SELECT * FROM time_entries WHERE id = ?").get(id);
  if (!entry) throw new Error("Time entry not found");
  db.prepare("DELETE FROM time_entries WHERE id = ?").run(id);
  logTimeEntryChange(id, "manual_delete", entry, null, "Deleted from dashboard");
  revalidatePath("/entries");
  revalidatePath("/");
  revalidatePath("/reports");
}

function parseEntryDate(value: FormDataEntryValue | null, label: string) {
  const raw = z.string().min(1).parse(value);
  const date = new Date(raw);
  if (Number.isNaN(date.getTime())) throw new Error(`${label} is not valid`);
  return date.toISOString();
}
