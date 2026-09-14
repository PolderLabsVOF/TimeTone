import { format } from "date-fns";
import { Download, History, Pencil, Plus, Search, Trash2 } from "lucide-react";
import { addManualEntry, deleteTimeEntry, updateTimeEntry } from "@/app/actions";
import { PageHeading } from "@/components/page-heading";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { EmployeeMultiSelect } from "@/components/employee-multi-select";
import { TimeEntryDateField } from "@/components/time-entry-date-field";
import { durationMinutes, formatDuration, roundDuration } from "@/lib/domain";
import { getEmployees, getEntryChanges, getFilteredEntries, getSettings } from "@/lib/db";

type Query = { q?: string; employee?: string; status?: string; source?: string; from?: string; to?: string; entryError?: string };

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
      <form className="mb-6 grid gap-3 rounded-2xl border border-black/6 bg-white p-4 shadow-sm shadow-black/[.02] md:grid-cols-[1.35fr_repeat(4,minmax(0,1fr))_auto]">
        <label className="relative"><Search className="pointer-events-none absolute left-3 top-2.5 size-4 text-black/35" /><input name="q" defaultValue={query.q} placeholder="Search employee or note" className="h-9 w-full rounded-lg border border-black/10 bg-white pl-9 pr-3 text-sm" /></label>
        <select name="employee" defaultValue={query.employee || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All employees</option>{allEmployees.map((employee) => <option key={employee.id} value={employee.id}>{employee.name}</option>)}</select>
        <select name="status" defaultValue={query.status || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All statuses</option><option value="open">Open</option><option value="closed">Closed</option></select>
        <select name="source" defaultValue={query.source || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All sources</option><option value="device">Device</option><option value="manual">Manual</option><option value="automatic">Automatic</option></select>
        <div className="flex gap-2"><input aria-label="From date" type="date" name="from" defaultValue={query.from} className="h-9 min-w-0 rounded-lg border border-black/10 px-2 text-sm" /><input aria-label="To date" type="date" name="to" defaultValue={query.to} className="h-9 min-w-0 rounded-lg border border-black/10 px-2 text-sm" /></div>
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
                        <button
                          type="button"
                          popoverTarget={`edit-entry-${entry.id}`}
                          popoverTargetAction="toggle"
                          className="inline-flex h-8 items-center gap-1.5 rounded-lg border border-black/10 px-2.5 text-xs font-medium text-black/60 transition-colors hover:bg-black/[.03]"
                        >
                          <Pencil className="size-3.5" />
                          Edit
                        </button>
                        <div
                          id={`edit-entry-${entry.id}`}
                          popover="auto"
                          className="fixed inset-0 m-auto max-h-[calc(100vh-2rem)] w-[min(21rem,calc(100vw-2rem))] overflow-y-auto rounded-2xl border border-black/10 bg-white p-4 text-left shadow-xl shadow-black/10"
                          style={{ margin: "auto" }}
                        >
                            <div className="mb-3">
                              <p className="text-sm font-semibold">Edit time entry</p>
                              <p className="mt-0.5 text-xs text-black/45">
                                Adjust the raw times; reports will recalculate automatically.
                              </p>
                            </div>
                            <form action={updateTimeEntry} className="space-y-3">
                              <input type="hidden" name="id" value={entry.id} />
                              <div className="space-y-1.5">
                                <Label htmlFor={`employee-${entry.id}`}>Employee</Label>
                                <select
                                  id={`employee-${entry.id}`}
                                  name="employee_id"
                                  defaultValue={entry.employee_id}
                                  className="h-9 w-full rounded-lg border border-black/10 bg-white px-2.5 text-sm"
                                  required
                                >
                                  {allEmployees.map((employee) => (
                                    <option key={employee.id} value={employee.id}>
                                      {employee.name}{employee.active ? "" : " (inactive)"}
                                    </option>
                                  ))}
                                </select>
                              </div>
                              <TimeEntryDateField
                                label="Clock in"
                                name="clock_in"
                                id={`clock-in-${entry.id}`}
                                defaultValue={toDateTimeLocal(entry.clock_in)}
                                required
                                light
                              />
                              <TimeEntryDateField
                                label="Clock out"
                                name="clock_out"
                                id={`clock-out-${entry.id}`}
                                defaultValue={entry.clock_out ? toDateTimeLocal(entry.clock_out) : ""}
                                light
                              />
                              {!entry.clock_out && <p className="-mt-1 text-xs leading-4 text-emerald-700">This entry is open. Leave Clock out empty to keep it open, or set a time to close it.</p>}
                              <div className="space-y-1.5">
                                <Label htmlFor={`note-${entry.id}`}>Note</Label>
                                <Input
                                  id={`note-${entry.id}`}
                                  name="note"
                                  defaultValue={entry.note || ""}
                                  placeholder="Reason for correction"
                                  className="h-9 border-black/10 bg-white"
                                />
                              </div>
                              <Button type="submit" size="sm" className="w-full bg-[#17211b] text-white hover:bg-[#26352c]">
                                Save changes
                              </Button>
                            </form>
                        </div>
                        <form action={deleteTimeEntry} className="mt-2"><input type="hidden" name="id" value={entry.id} /><Button type="submit" size="xs" variant="ghost" className="w-full text-red-600 hover:bg-red-50 hover:text-red-700"><Trash2 className="size-3" />Delete entry</Button></form>
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
            For corrections, remote work, or a missed clock-in. Leave Clock out empty for an open shift.
          </p>
          <form action={addManualEntry} className="mt-6 space-y-4">
            <EmployeeMultiSelect employees={employees} />
            <TimeEntryDateField label="Clock in" name="clock_in" required />
            <TimeEntryDateField label="Clock out" name="clock_out" />
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
