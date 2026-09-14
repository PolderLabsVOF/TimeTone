import { ArchiveRestore, Clock3, Download, KeyRound, Rocket, Save, Settings2, SlidersHorizontal } from "lucide-react";
import { changePassword, saveSettings } from "@/app/actions";
import { PageHeading } from "@/components/page-heading";
import { ReleaseUpdater } from "@/components/release-updater";
import { SettingsTabs } from "@/components/settings-tabs";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { getSettings } from "@/lib/db";

export default async function SettingsPage(
  { searchParams }: { searchParams: Promise<{ password?: string; migration?: string }> },
) {
  const settings = getSettings();
  const { password, migration } = await searchParams;
  const defaultTab = password ? "security" : migration ? "data" : "workspace";

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
          <div className="flex flex-col justify-between gap-4 rounded-2xl bg-[#17211b] p-5 text-white shadow-sm shadow-black/10 sm:flex-row sm:items-center">
            <div>
              <p className="text-xs font-semibold uppercase tracking-[.16em] text-[#d8ff62]">Workspace defaults</p>
              <p className="mt-1 text-sm leading-5 text-white/60">Choose a tab below. Workspace changes save together.</p>
            </div>
            <Button type="submit" form="workspace-settings" size="lg" className="shrink-0 bg-[#d8ff62] text-[#17211b] hover:bg-[#c9ef58]"><Save className="size-4" />Save settings</Button>
          </div>

          <form id="workspace-settings" action={saveSettings}></form>
          <SettingsTabs
            defaultValue={defaultTab}
            tabs={[
              {
                value: "workspace",
                label: "Workspace",
                icon: "workspace",
                content: <SettingsSection icon={Settings2} title="Workspace" description="How the workspace appears across reports and terminals.">
                  <div className="grid gap-5 sm:grid-cols-2">
                    <Field formId="workspace-settings" label="Company name" name="company_name" defaultValue={settings.company_name} />
                    <Field formId="workspace-settings" label="IANA timezone" name="timezone" defaultValue={settings.timezone} description="For example Europe/Amsterdam." />
                  </div>
                </SettingsSection>,
              },
              {
                value: "time",
                label: "Time rules",
                icon: "time",
                content: <div className="space-y-5">
                  <SettingsSection icon={Clock3} title="Time rules" description="Raw timestamps are preserved. These rules affect calculated, auditable work sessions.">
                    <div className="grid gap-5 sm:grid-cols-2">
                      <SelectField formId="workspace-settings" label="Rounding interval" name="rounding_minutes" value={settings.rounding_minutes} options={[["1", "No practical rounding"], ["5", "5 minutes"], ["10", "10 minutes"], ["15", "15 minutes"], ["30", "30 minutes"]]} />
                      <SelectField formId="workspace-settings" label="Rounding direction" name="rounding_mode" value={settings.rounding_mode} options={[["nearest", "Nearest interval"], ["up", "Always up"], ["down", "Always down"]]} />
                    </div>
                  </SettingsSection>
                  <SettingsSection icon={SlidersHorizontal} title="Automatic time management" description="Repair common swipe mistakes automatically; every intervention is recorded in the entry history.">
                    <div className="grid gap-5 sm:grid-cols-2">
                      <SelectField formId="workspace-settings" label="Short interruption handling" name="auto_merge_enabled" value={settings.auto_merge_enabled} options={[["true", "Merge into one shift"], ["false", "Keep sessions separate"]]} />
                      <NumberField formId="workspace-settings" label="Merge gap window" name="auto_merge_minutes" defaultValue={settings.auto_merge_minutes} suffix="minutes" min={1} max={120} description="A clock-in in this window after clock-out reopens the prior shift." />
                      <SelectField formId="workspace-settings" label="Open-shift safety close" name="auto_close_enabled" value={settings.auto_close_enabled} options={[["true", "Automatically close"], ["false", "Leave open for review"]]} />
                      <NumberField formId="workspace-settings" label="Maximum shift length" name="max_shift_hours" defaultValue={settings.max_shift_hours} suffix="hours" min={1} max={24} description="Open shifts exceeding this limit are closed and logged." />
                      <NumberField formId="workspace-settings" label="Duplicate scan protection" name="duplicate_window_seconds" defaultValue={settings.duplicate_window_seconds} suffix="seconds" min={0} max={120} description="Rapid repeat scans from the same terminal are ignored." />
                    </div>
                    <div className="rounded-xl border border-amber-200 bg-amber-50 p-4 text-xs leading-5 text-amber-900">Automatic changes appear with their reason in the Time entries audit trail. You can always edit a result manually.</div>
                  </SettingsSection>
                </div>,
              },
              {
                value: "reports",
                label: "Reports",
                icon: "reports",
                content: <SettingsSection icon={SlidersHorizontal} title="Reports" description="Set the default time window for reporting. Terminal-specific options live on each device card.">
                  <div className="grid gap-5 sm:grid-cols-3">
                    <SelectField formId="workspace-settings" label="Default report window" name="default_report_window" value={settings.default_report_window} options={[["7", "7 days"], ["14", "14 days"], ["30", "30 days"], ["60", "2 months"], ["90", "90 days"], ["365", "12 months"]]} />
                  </div>
                </SettingsSection>,
              },
              {
                value: "security",
                label: "Security",
                icon: "security",
                content: <SettingsSection icon={KeyRound} title="Admin password" description="Change the password used to sign in to this dashboard.">
                  {password === "changed" && <Notice tone="success">Password updated.</Notice>}
                  {password === "incorrect" && <Notice tone="error">Current password is not correct.</Notice>}
                  {password === "invalid" && <Notice tone="error">Use a new password of 8–128 characters and enter it twice.</Notice>}
                  <form action={changePassword} className="grid gap-4 sm:max-w-xl">
                    <Field label="Current password" name="current_password" type="password" autoComplete="current-password" />
                    <Field label="New password" name="new_password" type="password" autoComplete="new-password" />
                    <Field label="Confirm password" name="confirm_password" type="password" autoComplete="new-password" />
                    <Button type="submit" variant="outline" className="w-fit"><KeyRound className="size-4" />Change password</Button>
                  </form>
                </SettingsSection>,
              },
              {
                value: "data",
                label: "Data & updates",
                icon: "data",
                content: <div className="space-y-5">
                  <SettingsSection icon={ArchiveRestore} title="Migration" description="Move employees, terminals, settings, time entries, and audit history to another TimeTone server.">
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
                  <SettingsSection icon={Rocket} title="Software updates" description="Keep TimeTone current with stable releases from GitHub.">
                    <ReleaseUpdater />
                  </SettingsSection>
                </div>,
              },
            ]}
          />
        </main>

        <aside className="grid h-fit gap-5 xl:sticky xl:top-6">
          <div className="rounded-2xl bg-[#17211b] p-5 text-white shadow-sm shadow-black/10">
            <p className="text-[11px] font-semibold uppercase tracking-[.16em] text-[#d8ff62]">How settings work</p>
            <p className="mt-2 text-sm leading-5 text-white/55">Switch tabs without losing values. Workspace changes are submitted together with Save settings.</p>
            <div className="mt-5 rounded-xl bg-white/8 p-3 text-xs leading-5 text-white/55">Terminal sleep, sync, theme, and firmware controls live on the Devices page.</div>
          </div>
          <div className="rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02]">
            <p className="text-[11px] font-semibold uppercase tracking-[.16em] text-black/35">Device controls</p>
            <p className="mt-2 text-sm leading-5 text-black/55">Manage terminal-specific timing, theme, synchronization, and firmware from the device list.</p>
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

function SettingsSection({ icon: Icon, title, description, className, children }: { icon: typeof Settings2; title: string; description: string; className?: string; children: React.ReactNode }) {
  return <section className={`rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02] md:p-6 ${className || ""}`}><div className="mb-6 flex items-start gap-3"><span className="grid size-10 shrink-0 place-items-center rounded-xl bg-[#eef4e4] text-[#526b38]"><Icon className="size-5" /></span><div><h2 className="font-semibold">{title}</h2><p className="mt-1 max-w-2xl text-sm leading-5 text-black/50">{description}</p></div></div><div className="space-y-5">{children}</div></section>;
}

function Notice({ tone, children }: { tone: "success" | "error"; children: React.ReactNode }) {
  return <p className={`mb-4 rounded-lg px-3 py-2 text-sm ${tone === "success" ? "bg-emerald-50 text-emerald-800" : "bg-red-50 text-red-700"}`}>{children}</p>;
}

function Field({ label, description, formId, ...props }: React.ComponentProps<typeof Input> & { label: string; description?: string; formId?: string }) {
  return <div className="space-y-2"><Label htmlFor={props.name}>{label}</Label><Input id={props.name} form={formId} className="h-10 bg-white" required {...props} />{description && <p className="text-xs text-black/40">{description}</p>}</div>;
}

function NumberField({ label, suffix, description, formId, ...props }: React.ComponentProps<typeof Input> & { label: string; suffix: string; description?: string; formId?: string }) {
  return <div className="space-y-2"><Label htmlFor={props.name}>{label}</Label><div className="relative"><Input id={props.name} form={formId} type="number" className="h-10 bg-white pr-20" required {...props} /><span className="pointer-events-none absolute inset-y-0 right-3 grid place-items-center text-xs text-black/40">{suffix}</span></div>{description && <p className="text-xs text-black/40">{description}</p>}</div>;
}

function SelectField({ label, name, value, options, formId }: { label: string; name: string; value: string; options: string[][]; formId?: string }) {
  return <div className="space-y-2"><Label htmlFor={name}>{label}</Label><select id={name} name={name} form={formId} defaultValue={value} className="h-10 w-full rounded-lg border border-input bg-white px-3 text-sm">{options.map(([key, text]) => <option key={key} value={key}>{text}</option>)}</select></div>;
}
