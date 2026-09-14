import { format } from "date-fns";
import { Download, History, Plus, Search } from "lucide-react";
import { addManualEntry, deleteTimeEntry, updateTimeEntry } from "@/app/actions";
import { PageHeading } from "@/components/page-heading";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { EmployeeMultiSelect } from "@/components/employee-multi-select";
import { EntriesFilterDates } from "@/components/entries-filter-dates";
import { TimeEntryDateField } from "@/components/time-entry-date-field";
import { EditEntryPopover } from "@/components/edit-entry-popover";
import { durationMinutes, formatDuration, roundDuration } from "@/lib/domain";
import { getEmployees, getEntryChanges, getFilteredEntries, getSettings } from "@/lib/db";

type Query = { q?: string; employee?: string; status?: string; source?: string; from?: string; to?: string; entryError?: string; entrySuccess?: string };

export default async function EntriesPage({ searchParams }: { searchParams: Promise<Query> }) {
  const query = await searchParams;
  const entries = getFilteredEntries({
      search: query.q,
      employeeId: query.employee || undefined,
      status: query.status === "open" || query.status === "closed" ? query.status : undefined,
      source: query.source || undefined,
      from: query.from ? new Date(`${query.from}T00:00:00`).toISOString() : undefined,
      to: query.to ? new Date(`${query.to}T23:59:59`).toISOString() : undefined,
    }),
    employees = getEmployees(false),
    allEmployees = getEmployees(),
    settings = getSettings(),
    changes = getEntryChanges(8);
  const queryString = new URLSearchParams(Object.entries(query).filter(([, value]) => value) as [string, string][]).toString();
  return (
    <>
      <PageHeading
        eyebrow="Audit trail"
        title="Time entries"
        description="Raw clock times stay intact. Rounded duration is calculated separately for transparent reporting."
      />
      {query.entryError && (
        <div role="alert" className="mb-6 rounded-xl border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-800">
          <span className="font-semibold">Time entry not saved.</span> {query.entryError}
        </div>
      )}
      {query.entrySuccess && !query.entryError && (
        <div role="status" aria-live="polite" className="mb-6 rounded-xl border border-emerald-200 bg-emerald-50 px-4 py-3 text-sm text-emerald-800">
          <span className="font-semibold">Entry saved.</span> The manual entry is now in the list below.
        </div>
      )}
      <form className="mb-6 grid gap-3 rounded-2xl border border-black/6 bg-white p-4 shadow-sm shadow-black/[.02] md:grid-cols-[1.35fr_repeat(4,minmax(0,1fr))_auto]">
        <label className="relative"><Search className="pointer-events-none absolute left-3 top-2.5 size-4 text-black/35" /><input name="q" defaultValue={query.q} placeholder="Search employee or note" className="h-9 w-full rounded-lg border border-black/10 bg-white pl-9 pr-3 text-sm" /></label>
        <select name="employee" defaultValue={query.employee || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All employees</option>{allEmployees.map((employee) => <option key={employee.id} value={employee.id}>{employee.name}</option>)}</select>
        <select name="status" defaultValue={query.status || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All statuses</option><option value="open">Open</option><option value="closed">Closed</option></select>
        <select name="source" defaultValue={query.source || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All sources</option><option value="device">Device</option><option value="manual">Manual</option><option value="automatic">Automatic</option></select>
        <EntriesFilterDates from={query.from} to={query.to} />
        <Button type="submit" variant="outline">Filter</Button>
      </form>
      <div className="mb-4 flex items-center justify-between text-sm text-black/45"><span>{entries.length} matching entr{entries.length === 1 ? "y" : "ies"}</span><a href={`/api/export?type=entries&${queryString}`} className="inline-flex items-center gap-1.5 font-medium text-black/60 hover:text-black"><Download className="size-4" />Export this view</a></div>
      <div className="grid gap-6 2xl:grid-cols-[1fr_360px]">
        <div className="overflow-hidden rounded-2xl border border-black/6 bg-white">
          <div className="overflow-x-auto">
            <table className="w-full text-left text-sm">
              <thead className="border-b border-black/6 bg-black/[.015] text-xs uppercase tracking-wider text-black/35">
                <tr>
                  <th className="px-5 py-3">Employee</th>
                  <th className="px-5 py-3">In</th>
                  <th className="px-5 py-3">Out</th>
                  <th className="px-5 py-3">Exact</th>
                  <th className="px-5 py-3">Rounded</th>
                  <th className="px-5 py-3">Source</th>
                  <th className="px-5 py-3 text-right">Actions</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-black/5">
                {entries.map((entry) => {
                  const exact = durationMinutes(
                    entry.clock_in,
                    entry.clock_out,
                  );
                  return (
                    <tr key={entry.id}>
                      <td className="whitespace-nowrap px-5 py-4 font-medium">
                        {entry.employee_name}
                      </td>
                      <td className="whitespace-nowrap px-5 py-4 text-black/55">
                        {format(new Date(entry.clock_in), "d MMM, HH:mm")}
                      </td>
                      <td className="whitespace-nowrap px-5 py-4 text-black/55">
                        {entry.clock_out
                          ? format(new Date(entry.clock_out), "d MMM, HH:mm")
                          : (
                            <Badge className="bg-emerald-100 text-emerald-700">
                              Open
                            </Badge>
                          )}
                      </td>
                      <td className="px-5 py-4 font-mono text-xs">
                        {formatDuration(exact)}
                      </td>
                      <td className="px-5 py-4 font-mono text-xs font-semibold">
                        {formatDuration(
                          roundDuration(
                            exact,
                            Number(settings.rounding_minutes),
                            settings.rounding_mode,
                          ),
                        )}
                      </td>
                      <td className="px-5 py-4 capitalize text-black/40">
                        <span>{entry.source}</span>
                        {entry.note && (
                          <span className="mt-1 block max-w-40 truncate text-xs normal-case text-black/35" title={entry.note}>
                            {entry.note}
                          </span>
                        )}
                      </td>
                      <td className="px-5 py-4 text-right">
                        <EditEntryPopover
                          id={entry.id}
                          employeeId={entry.employee_id}
                          clockIn={toDateTimeLocal(entry.clock_in)}
                          clockOut={entry.clock_out ? toDateTimeLocal(entry.clock_out) : null}
                          note={entry.note}
                          allEmployees={allEmployees}
                          updateAction={updateTimeEntry}
                          deleteAction={deleteTimeEntry}
                        />
                      </td>
                    </tr>
                  );
                })}
                {entries.length === 0 && (
                  <tr>
                    <td colSpan={7} className="p-10 text-center text-black/40">
                      No entries recorded yet.
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </div>
        <div className="space-y-6">
        <div className="h-fit rounded-2xl bg-[#17211b] p-6 text-white">
          <div className="mb-5 grid size-10 place-items-center rounded-xl bg-[#d8ff62] text-[#17211b]">
            <Plus className="size-5" />
          </div>
          <h2 className="text-xl font-semibold">Manual entry</h2>
          <p className="mt-1 text-xs leading-5 text-white/45">
            For corrections, remote work, or a missed clock-in. Clock out is optional; leave it blank to save an open shift.
          </p>
          <form action={addManualEntry} className="mt-6 space-y-4">
            <EmployeeMultiSelect employees={employees} />
            <TimeEntryDateField label="Clock in" name="clock_in" required surface="dark" />
            <TimeEntryDateField label="Clock out" name="clock_out" clearable clearLabel="Clear end time" surface="dark" />
            <div className="space-y-2">
              <Label htmlFor="note">Note</Label>
              <Input
                id="note"
                name="note"
                placeholder="Reason for correction"
                className="border-white/15 bg-white/8 placeholder:text-white/25"
              />
            </div>
            <Button type="submit" className="w-full bg-[#d8ff62] text-[#17211b] hover:bg-[#c9ef58]">
              Save entry
            </Button>
          </form>
        </div>
        <div className="rounded-2xl border border-black/6 bg-white p-5"><div className="flex items-center gap-2"><span className="grid size-8 place-items-center rounded-lg bg-[#eef4e4] text-[#526b38]"><History className="size-4" /></span><div><h2 className="font-semibold">Recent automation</h2><p className="text-xs text-black/40">Explainable rule decisions</p></div></div><div className="mt-4 space-y-3">{changes.map((change) => <div key={change.id} className="border-l-2 border-[#d8ff62] pl-3"><p className="text-xs font-medium capitalize">{change.action.replaceAll("_", " ")}</p><p className="mt-0.5 text-xs leading-4 text-black/45">{change.reason}</p><p className="mt-1 text-[10px] text-black/35">{format(new Date(change.created_at), "d MMM HH:mm")}</p></div>)}{changes.length === 0 && <p className="text-sm text-black/40">No automatic or manual changes yet.</p>}</div></div>
        </div>
      </div>
    </>
  );
}
function toDateTimeLocal(value: string) {
  return format(new Date(value), "yyyy-MM-dd'T'HH:mm");
}
