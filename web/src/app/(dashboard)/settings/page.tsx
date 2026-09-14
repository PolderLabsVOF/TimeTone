import { ArchiveRestore, Clock3, Download, KeyRound, Rocket, Save, Settings2, SlidersHorizontal } from "lucide-react";
import { changePassword, saveSettings } from "@/app/actions";
import { PageHeading } from "@/components/page-heading";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { getSettings } from "@/lib/db";
import { ReleaseUpdater } from "@/components/release-updater";

export default async function SettingsPage(
  { searchParams }: { searchParams: Promise<{ password?: string; migration?: string }> },
) {
  const settings = getSettings();
  const { password, migration } = await searchParams;
  return (
    <>
      <PageHeading eyebrow="Workspace controls" title="Settings" description="Keep attendance rules transparent, terminals responsive, and reporting consistent." />
      <div className="mb-6 grid gap-3 sm:grid-cols-3">
        <SummaryCard label="Workspace" value={settings.company_name} detail={settings.timezone} />
        <SummaryCard label="Rounding" value={`${settings.rounding_minutes} min`} detail={settings.rounding_mode === "nearest" ? "Nearest interval" : settings.rounding_mode === "up" ? "Always up" : "Always down"} />
        <SummaryCard label="Open-shift safety" value={settings.auto_close_enabled === "true" ? "Automatic close" : "Manual review"} detail={`${settings.max_shift_hours} hour maximum`} />
      </div>

      <div className="grid gap-8 xl:grid-cols-[minmax(0,1fr)_260px]">
        <main className="min-w-0">
          <form action={saveSettings} className="space-y-5">
            <div className="flex flex-col justify-between gap-4 rounded-2xl bg-[#17211b] p-5 text-white shadow-sm shadow-black/10 sm:flex-row sm:items-center">
              <div>
                <p className="text-xs font-semibold uppercase tracking-[.16em] text-[#d8ff62]">Workspace defaults</p>
                <p className="mt-1 text-sm leading-5 text-white/60">Save attendance, automation, and reporting rules together.</p>
              </div>
              <Button type="submit" size="lg" className="shrink-0 bg-[#d8ff62] text-[#17211b] hover:bg-[#c9ef58]"><Save className="size-4" />Save settings</Button>
            </div>

            <div className="grid gap-5 lg:grid-cols-2">
              <SettingsSection id="workspace" icon={Settings2} title="Workspace" description="How the workspace appears across reports and terminals.">
                <div className="grid gap-5 sm:grid-cols-2">
                  <Field label="Company name" name="company_name" defaultValue={settings.company_name} />
                  <Field label="IANA timezone" name="timezone" defaultValue={settings.timezone} description="For example Europe/Amsterdam." />
                </div>
              </SettingsSection>

              <SettingsSection id="time-rules" icon={Clock3} title="Time rules" description="Raw timestamps are preserved. These rules affect calculated, auditable work sessions.">
                <div className="grid gap-5 sm:grid-cols-2">
                  <SelectField label="Rounding interval" name="rounding_minutes" value={settings.rounding_minutes} options={[["1", "No practical rounding"], ["5", "5 minutes"], ["10", "10 minutes"], ["15", "15 minutes"], ["30", "30 minutes"]]} />
                  <SelectField label="Rounding direction" name="rounding_mode" value={settings.rounding_mode} options={[["nearest", "Nearest interval"], ["up", "Always up"], ["down", "Always down"]]} />
                </div>
              </SettingsSection>
            </div>

            <SettingsSection id="automation" icon={SlidersHorizontal} title="Automatic time management" description="Repair common swipe mistakes automatically; every intervention is recorded in the entry history.">
              <div className="grid gap-5 sm:grid-cols-2">
                <SelectField label="Short interruption handling" name="auto_merge_enabled" value={settings.auto_merge_enabled} options={[["true", "Merge into one shift"], ["false", "Keep sessions separate"]]} />
                <NumberField label="Merge gap window" name="auto_merge_minutes" defaultValue={settings.auto_merge_minutes} suffix="minutes" min={1} max={120} description="A clock-in in this window after clock-out reopens the prior shift." />
                <SelectField label="Open-shift safety close" name="auto_close_enabled" value={settings.auto_close_enabled} options={[["true", "Automatically close"], ["false", "Leave open for review"]]} />
                <NumberField label="Maximum shift length" name="max_shift_hours" defaultValue={settings.max_shift_hours} suffix="hours" min={1} max={24} description="Open shifts exceeding this limit are closed and logged." />
                <NumberField label="Duplicate scan protection" name="duplicate_window_seconds" defaultValue={settings.duplicate_window_seconds} suffix="seconds" min={0} max={120} description="Rapid repeat scans from the same terminal are ignored." />
              </div>
              <div className="rounded-xl border border-amber-200 bg-amber-50 p-4 text-xs leading-5 text-amber-900">Automatic changes appear with their reason in the Time entries audit trail. You can always edit a result manually.</div>
            </SettingsSection>

            <SettingsSection id="reports" icon={SlidersHorizontal} title="Reports" description="Set the default time window for reporting. Terminal-specific options live on each device card.">
              <div className="grid gap-5 sm:grid-cols-3">
                <SelectField label="Default report window" name="default_report_window" value={settings.default_report_window} options={[["7", "7 days"], ["14", "14 days"], ["30", "30 days"], ["60", "2 months"], ["90", "90 days"], ["365", "12 months"]]} />
              </div>
            </SettingsSection>
          </form>

          <div className="mt-8 grid gap-5 lg:grid-cols-2">
            <SettingsSection id="security" icon={KeyRound} title="Admin password" description="Change the password used to sign in to this dashboard.">
              {password === "changed" && <Notice tone="success">Password updated.</Notice>}
              {password === "incorrect" && <Notice tone="error">Current password is not correct.</Notice>}
              {password === "invalid" && <Notice tone="error">Use a new password of 8–128 characters and enter it twice.</Notice>}
              <form action={changePassword} className="grid gap-4">
                <Field label="Current password" name="current_password" type="password" autoComplete="current-password" />
                <Field label="New password" name="new_password" type="password" autoComplete="new-password" />
                <Field label="Confirm password" name="confirm_password" type="password" autoComplete="new-password" />
                <Button type="submit" variant="outline" className="w-fit"><KeyRound className="size-4" />Change password</Button>
              </form>
            </SettingsSection>

            <SettingsSection id="updates" icon={Rocket} title="Software updates" description="Keep TimeTone current with stable releases from GitHub.">
              <ReleaseUpdater />
            </SettingsSection>
          </div>

          <SettingsSection id="migration" icon={ArchiveRestore} title="Migration" description="Move employees, terminals, settings, time entries, and audit history to another TimeTone server." className="mt-5">
            {migration === "imported" && <Notice tone="success">Migration imported successfully. All workspace data and history have been restored.</Notice>}
            {migration && migration !== "imported" && <Notice tone="error">Migration could not be imported. Check the file and try again.</Notice>}
            <div className="grid gap-4 md:grid-cols-2">
              <div className="rounded-xl border border-black/8 bg-[#f5f6f2] p-4 dark:border-white/10 dark:bg-[#243127]">
                <p className="font-medium">Download a complete backup</p>
                <p className="mt-1 text-sm leading-5 text-black/50 dark:text-white/60">Creates one portable JSON file containing all workspace data and history.</p>
                <a href="/api/migration/export" className="mt-4 inline-flex h-9 items-center gap-2 rounded-lg bg-[#17211b] px-3 text-sm font-medium text-white transition hover:-translate-y-0.5 hover:bg-[#26352c]"><Download className="size-4" />Download migration file</a>
              </div>
              <form action="/api/migration/import" method="post" encType="multipart/form-data" className="rounded-xl border border-black/8 bg-[#f5f6f2] p-4 dark:border-white/10 dark:bg-[#243127]">
                <p className="font-medium">Restore on this server</p>
                <p className="mt-1 text-sm leading-5 text-black/50 dark:text-white/60">This replaces the current workspace data. Export a backup first.</p>
                <input name="file" type="file" accept="application/json,.json" required className="mt-4 block w-full text-sm file:mr-3 file:rounded-md file:border-0 file:bg-white file:px-3 file:py-2 file:text-sm file:font-medium dark:file:bg-[#1b261f]" />
                <button type="submit" className="mt-3 inline-flex h-9 items-center gap-2 rounded-lg border border-black/12 bg-white px-3 text-sm font-medium transition hover:-translate-y-0.5 dark:border-white/15 dark:bg-[#1b261f]"><ArchiveRestore className="size-4" />Import migration file</button>
              </form>
            </div>
          </SettingsSection>
        </main>

        <aside className="grid h-fit gap-5 xl:sticky xl:top-6">
          <div className="rounded-2xl bg-[#17211b] p-5 text-white shadow-sm shadow-black/10">
            <p className="text-[11px] font-semibold uppercase tracking-[.16em] text-[#d8ff62]">Settings map</p>
            <p className="mt-2 text-sm leading-5 text-white/55">Jump directly to the part of the workspace you want to change.</p>
            <nav aria-label="Settings sections" className="mt-5 space-y-1">
              {[["workspace", "Workspace"], ["time-rules", "Time rules"], ["automation", "Automatic time management"], ["reports", "Reports"], ["security", "Admin password"], ["migration", "Migration"], ["updates", "Software updates"]].map(([id, label]) => (
                <a key={id} href={`#${id}`} className="block rounded-lg px-3 py-2 text-sm text-white/70 transition hover:bg-white/10 hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#d8ff62]">{label}</a>
              ))}
            </nav>
          </div>
          <div className="rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02]">
            <p className="text-[11px] font-semibold uppercase tracking-[.16em] text-black/35">Where things live</p>
            <p className="mt-2 text-sm leading-5 text-black/55">Workspace rules apply here. Terminal sleep, sync, theme, and firmware controls live on the Devices page.</p>
            <a href="/devices" className="mt-4 inline-flex text-sm font-medium text-black/70 underline-offset-4 hover:underline">Open Devices</a>
          </div>
        </aside>
      </div>
    </>
  );
}

function SummaryCard({ label, value, detail }: { label: string; value: string; detail: string }) {
  return <div className="rounded-2xl border border-black/6 bg-white px-4 py-3 shadow-sm shadow-black/[.02]"><p className="text-xs font-medium text-black/45">{label}</p><p className="mt-1 truncate text-lg font-semibold tracking-tight" title={value}>{value}</p><p className="mt-0.5 text-xs text-black/40">{detail}</p></div>;
}

function SettingsSection({ id, icon: Icon, title, description, className, children }: { id?: string; icon: typeof Settings2; title: string; description: string; className?: string; children: React.ReactNode }) {
  return <section id={id} className={`scroll-mt-6 rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02] md:p-6 ${className || ""}`}><div className="mb-6 flex items-start gap-3"><span className="grid size-10 shrink-0 place-items-center rounded-xl bg-[#eef4e4] text-[#526b38]"><Icon className="size-5" /></span><div><h2 className="font-semibold">{title}</h2><p className="mt-1 max-w-2xl text-sm leading-5 text-black/50">{description}</p></div></div><div className="space-y-5">{children}</div></section>;
}

function Notice({ tone, children }: { tone: "success" | "error"; children: React.ReactNode }) {
  return <p className={`mb-4 rounded-lg px-3 py-2 text-sm ${tone === "success" ? "bg-emerald-50 text-emerald-800" : "bg-red-50 text-red-700"}`}>{children}</p>;
}

function Field({ label, description, ...props }: React.ComponentProps<typeof Input> & { label: string; description?: string }) {
  return <div className="space-y-2"><Label htmlFor={props.name}>{label}</Label><Input id={props.name} className="h-10 bg-white" required {...props} />{description && <p className="text-xs text-black/40">{description}</p>}</div>;
}

function NumberField({ label, suffix, description, ...props }: React.ComponentProps<typeof Input> & { label: string; suffix: string; description?: string }) {
  return <div className="space-y-2"><Label htmlFor={props.name}>{label}</Label><div className="relative"><Input id={props.name} type="number" className="h-10 bg-white pr-20" required {...props} /><span className="pointer-events-none absolute inset-y-0 right-3 grid place-items-center text-xs text-black/40">{suffix}</span></div>{description && <p className="text-xs text-black/40">{description}</p>}</div>;
}

function SelectField({ label, name, value, options }: { label: string; name: string; value: string; options: string[][] }) {
  return <div className="space-y-2"><Label htmlFor={name}>{label}</Label><select id={name} name={name} defaultValue={value} className="h-10 w-full rounded-lg border border-input bg-white px-3 text-sm">{options.map(([key, text]) => <option key={key} value={key}>{text}</option>)}</select></div>;
}
