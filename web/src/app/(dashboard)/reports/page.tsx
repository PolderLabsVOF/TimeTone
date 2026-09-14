import { addDays, eachDayOfInterval, endOfDay, endOfWeek, format, startOfDay, startOfMonth, startOfWeek, subDays } from "date-fns";
import { BarChart3, Download, FileSpreadsheet, SlidersHorizontal, CalendarDays, ScanLine } from "lucide-react";
import { PageHeading } from "@/components/page-heading";
import { EntriesFilterDates } from "@/components/entries-filter-dates";
import { Button } from "@/components/ui/button";
import { durationMinutes, formatDuration, roundDuration } from "@/lib/domain";
import { getEmployees, getFilteredEntries, getSettings } from "@/lib/db";

type Query = { window?: string; from?: string; to?: string; employee?: string };
const reportPalette = ["#d8ff62", "#7dd3fc", "#c4b5fd", "#fda4af", "#86efac", "#fdba74", "#67e8f9"];

export default async function ReportsPage({ searchParams }: { searchParams: Promise<Query> }) {
  const query = await searchParams;
  const settings = getSettings();
  const employees = getEmployees();
  const windowDays = Math.max(1, Math.min(365, Number(query.window || settings.default_report_window)));
  const end = query.to ? endOfDay(new Date(query.to)) : new Date();
  const start = query.from ? startOfDay(new Date(query.from)) : startOfDay(subDays(end, windowDays - 1));
  const employeeId = query.employee || undefined;
  const entries = getFilteredEntries({ from: start.toISOString(), to: new Date(end.getTime() + 1).toISOString(), employeeId });
  const totals = new Map<string, number>();
  for (const entry of entries) {
    const minutes = roundDuration(durationMinutes(entry.clock_in, entry.clock_out), Number(settings.rounding_minutes), settings.rounding_mode);
    totals.set(entry.employee_name, (totals.get(entry.employee_name) || 0) + minutes);
  }
  const days = eachDayOfInterval({ start, end });
  const daily = officeOccupancyByDay(entries, start, end, Number(settings.rounding_minutes), settings.rounding_mode);
  // Keep a full-year, seven-row canvas and data scale regardless of the
  // selected report window.
  const heatmapEnd = endOfWeek(new Date(), { weekStartsOn: 0 });
  const heatmapStart = startOfWeek(subDays(heatmapEnd, 364), { weekStartsOn: 0 });
  const heatmapDays = eachDayOfInterval({ start: heatmapStart, end: heatmapEnd });
  const heatmapEntries = getFilteredEntries({
    from: heatmapStart.toISOString(),
    to: new Date(heatmapEnd.getTime() + 1).toISOString(),
    employeeId,
  });
  const heatmapDaily = officeOccupancyByDay(heatmapEntries, heatmapStart, heatmapEnd, Number(settings.rounding_minutes), settings.rounding_mode);
  const maxHeatmapDaily = Math.max(60, ...heatmapDaily.values());
  const heatmapMonths = Array.from({ length: 12 }, (_, index) => format(new Date(startOfMonth(heatmapStart).getFullYear(), startOfMonth(heatmapStart).getMonth() + index, 1), "MMM"));
  const activity = days.map((day) => ({ day, minutes: daily.get(format(day, "yyyy-MM-dd")) || 0 }));
  const rows = employees.filter((employee) => !employeeId || employee.id === employeeId).map((employee) => ({ employee, minutes: totals.get(employee.name) || 0 })).sort((a, b) => b.minutes - a.minutes);
  const totalMinutes = [...totals.values()].reduce((sum, value) => sum + value, 0);
  const occupiedDays = activity.filter((item) => item.minutes > 0);
  const averageOccupancyMinutes = occupiedDays.length
    ? Math.round(occupiedDays.reduce((sum, item) => sum + item.minutes, 0) / occupiedDays.length)
    : 0;
  const maxDaily = Math.max(60, ...activity.map((item) => item.minutes));
  const maxEmployee = Math.max(60, ...rows.map((item) => item.minutes));
  const queryString = new URLSearchParams({ from: format(start, "yyyy-MM-dd"), to: format(end, "yyyy-MM-dd"), ...(employeeId ? { employee: employeeId } : {}) }).toString();

  return <>
    <PageHeading eyebrow={`${format(start, "d MMM")} - ${format(end, "d MMM yyyy")}`} title="Reports" description={`Rounded ${settings.rounding_mode} to ${settings.rounding_minutes}-minute intervals. Use the date window to explore any period.`} action={<div className="flex flex-wrap gap-2"><a href={`/api/export?type=entries&${queryString}`}><Button variant="outline" className="bg-white transition hover:-translate-y-0.5"><Download className="size-4" />Entries</Button></a><a href={`/api/export?type=summary&${queryString}`}><Button className="bg-[#17211b] transition hover:-translate-y-0.5"><FileSpreadsheet className="size-4" />Summary</Button></a></div>} />
    <form className="mb-6 grid gap-3 rounded-2xl border border-black/6 bg-white p-4 shadow-sm shadow-black/[.02] md:grid-cols-[auto_auto_auto_1fr_auto]">
      <span className="grid size-9 place-items-center rounded-lg bg-[#eef4e4] text-[#526b38]"><SlidersHorizontal className="size-4" /></span>
      <select name="window" defaultValue={String(windowDays)} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="7">Last 7 days</option><option value="14">Last 14 days</option><option value="30">Last 30 days</option><option value="60">Last 2 months</option><option value="90">Last 90 days</option><option value="365">Last 12 months</option></select>
      <select name="employee" defaultValue={employeeId || ""} className="h-9 rounded-lg border border-black/10 bg-white px-3 text-sm"><option value="">All employees</option>{employees.map((employee) => <option key={employee.id} value={employee.id}>{employee.name}</option>)}</select>
      <EntriesFilterDates from={query.from} to={query.to} />
      <Button type="submit" variant="outline">Apply window</Button>
    </form>
    <section className="grid gap-4 md:grid-cols-3"><Metric label="Employee time" value={formatDuration(totalMinutes)} detail={`${entries.length} session${entries.length === 1 ? "" : "s"}`} /><Metric label="Average office occupation" value={formatDuration(averageOccupancyMinutes)} detail={occupiedDays.length ? `${occupiedDays.length} attended day${occupiedDays.length === 1 ? "" : "s"} · overlapping time counted once` : "No attended days in this window"} /><Metric label="People with time" value={String(rows.filter((row) => row.minutes > 0).length)} detail={`of ${rows.length} in the selected view`} /></section>
    <section className="mt-6 animate-rise-in rounded-2xl border border-black/6 bg-white p-4 shadow-sm shadow-black/[.02]"><div className="flex flex-wrap items-center justify-between gap-3"><div><p className="font-semibold">Export this report</p><p className="mt-1 text-xs text-black/45">Download a detailed ledger, payroll-friendly daily totals, audit events, or a team summary.</p></div><div className="flex flex-wrap gap-2"><a href={`/api/export?type=entries&${queryString}`}><Button size="sm" variant="outline"><Download className="size-4" />Detailed CSV</Button></a><a href={`/api/export?type=daily&${queryString}`}><Button size="sm" variant="outline"><CalendarDays className="size-4" />Daily totals</Button></a><a href={`/api/export?type=summary&${queryString}`}><Button size="sm" variant="outline"><FileSpreadsheet className="size-4" />Payroll summary</Button></a><a href={`/api/export?type=events&${queryString}`}><Button size="sm" variant="outline"><ScanLine className="size-4" />Terminal audit</Button></a></div></div></section>
    <section className="mt-6 grid gap-6 xl:grid-cols-[1.35fr_.65fr]">
      <div className="animate-rise-in rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02]"><div className="flex items-start justify-between"><div><h2 className="font-semibold">Daily office occupation</h2><p className="mt-1 text-sm text-black/45">Occupied office time by day. Overlapping employee sessions count once.</p></div><BarChart3 className="size-5 text-black/35" /></div><div className="mt-8 flex h-52 items-end gap-1.5">{activity.map(({ day, minutes }, index) => <div key={day.toISOString()} className="group relative flex h-full min-w-0 flex-1 items-end"><div className="report-intensity-fill w-full origin-bottom rounded-t-md transition-all duration-700 ease-out group-hover:brightness-110" style={{ height: `${Math.max(minutes ? 4 : 1, minutes / maxDaily * 100)}%`, "--heat": Math.min(1, minutes / maxDaily), transitionDelay: `${index * 18}ms` } as React.CSSProperties} /><div className="report-tooltip bottom-full left-1/2 mb-2 -translate-x-1/2"><strong>{format(day, "EEE d MMM")}</strong><br />{formatDuration(minutes)} occupied</div></div>)}</div><div className="mt-3 flex justify-between text-[10px] text-black/40"><span>{format(start, "d MMM")}</span><span>{format(end, "d MMM")}</span></div><div className="mt-4 flex items-center gap-2 text-[10px] text-black/45 dark:text-white/45"><span>Less</span><i className="report-intensity-legend h-2 w-24 rounded-full" /><span>More occupation</span></div></div>
      <div className="rounded-2xl bg-[#17211b] p-5 text-white"><p className="text-xs font-semibold uppercase tracking-[.16em] text-[#d8ff62]">At a glance</p><h2 className="mt-2 text-xl font-semibold">Team distribution</h2><div className="mt-6 space-y-4">{rows.slice(0, 6).map(({ employee, minutes }, index) => <div key={employee.id} className="group relative"><div className="mb-1.5 flex justify-between gap-3 text-sm"><span className="flex min-w-0 items-center gap-2 truncate"><i className="size-2 shrink-0 rounded-full" style={{ backgroundColor: employee.color }} />{employee.name}</span><span className="font-mono text-xs text-white/65">{formatDuration(minutes)}</span></div><div className="h-2 overflow-hidden rounded-full bg-white/10"><div className="h-full rounded-full transition-all duration-500" style={{ width: `${minutes / maxEmployee * 100}%`, backgroundColor: employee.color || reportPalette[index % reportPalette.length] }} /></div><div className="report-tooltip bottom-full right-0 mb-2"><strong>{employee.name}</strong><br />{formatDuration(minutes)} rounded in this window</div></div>)}{rows.length === 0 && <p className="text-sm text-white/45">No employees match this view.</p>}</div></div>
    </section>
    <section className="mt-6 rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02]"><div className="flex items-start justify-between"><div><h2 className="font-semibold">Activity map</h2><p className="mt-1 text-sm text-black/45">A fixed rolling year of seven rows and 53 week columns. Intensity is based on rounded hours for each day.</p></div><CalendarDays className="size-5 text-black/35" /></div><div className="report-month-labels mt-6 grid grid-cols-12 text-[10px] text-black/40 dark:text-white/45">{heatmapMonths.map((month) => <span key={month}>{month}</span>)}</div><div className="report-heat-grid mt-2">{heatmapDays.map((day) => { const isFuture = day > endOfDay(new Date()); const minutes = heatmapDaily.get(format(day, "yyyy-MM-dd")) || 0; return <div key={day.toISOString()} className="group relative aspect-square min-w-0"><div title={`${format(day, "EEE d MMM")}: ${isFuture ? "Future date" : formatDuration(minutes)}`} aria-label={`${format(day, "EEE d MMM")}: ${isFuture ? "Future date" : formatDuration(minutes)}`} className={`${isFuture ? "report-heat-cell-future" : "report-heat-cell"} size-full rounded-[3px] border border-black/5 transition-transform group-hover:scale-125 dark:border-white/5`} style={isFuture ? undefined : { "--heat": Math.min(1, minutes / maxHeatmapDaily) } as React.CSSProperties} /><div className="report-tooltip bottom-full left-1/2 mb-2 -translate-x-1/2"><strong>{format(day, "EEE d MMM")}</strong><br />{isFuture ? "Future date" : `${formatDuration(minutes)} rounded`}</div></div>; })}</div><div className="mt-4 flex items-center justify-end gap-2 text-[10px] text-black/45 dark:text-white/45"><span>Less</span><i className="report-intensity-legend h-2 w-24 rounded-full" /><span>More</span></div></section>
    <section className="mt-6 rounded-2xl border border-black/6 bg-white p-5"><div className="mb-4"><h2 className="font-semibold">Employee totals</h2><p className="mt-1 text-sm text-black/45">Click a filter above to focus on one person or a custom date range.</p></div><div className="space-y-3">{rows.map(({ employee, minutes }, index) => <div key={employee.id} className="group relative grid grid-cols-[minmax(100px,220px)_1fr_auto] items-center gap-4"><span className="flex min-w-0 items-center gap-2 truncate text-sm font-medium"><i className="size-2.5 shrink-0 rounded-full" style={{ backgroundColor: employee.color || reportPalette[index % reportPalette.length] }} />{employee.name}</span><div className="employee-total-track h-3 overflow-hidden rounded-full bg-[#eef0eb]"><div className="employee-total-bar h-full rounded-full transition-all duration-500" style={{ width: `${minutes / maxEmployee * 100}%`, backgroundColor: employee.color || reportPalette[index % reportPalette.length] }} /></div><span className="font-mono text-sm font-semibold">{formatDuration(minutes)}</span><div className="report-tooltip bottom-full right-0 mb-2"><strong>{employee.name}</strong><br />{formatDuration(minutes)} rounded · {Math.round(minutes / 60 * 10) / 10} hours</div></div>)}</div></section>
  </>;
}

function Metric({ label, value, detail }: { label: string; value: string; detail: string }) { return <div className="rounded-2xl border border-black/6 bg-white p-5 shadow-sm shadow-black/[.02]"><p className="text-xs font-semibold uppercase tracking-[.14em] text-black/40">{label}</p><p className="mt-2 text-2xl font-semibold tracking-tight">{value}</p><p className="mt-1 text-xs text-black/45">{detail}</p></div>; }

function officeOccupancyByDay(
  entries: Awaited<ReturnType<typeof getFilteredEntries>>,
  rangeStart: Date,
  rangeEnd: Date,
  roundingMinutes: number,
  roundingMode: string,
) {
  const intervals = new Map<string, Array<{ start: number; end: number }>>();
  const ceiling = Math.min(rangeEnd.getTime(), Date.now());
  for (const entry of entries) {
    let cursor = Math.max(new Date(entry.clock_in).getTime(), rangeStart.getTime());
    const finish = Math.min(entry.clock_out ? new Date(entry.clock_out).getTime() : Date.now(), ceiling);
    if (!Number.isFinite(cursor) || !Number.isFinite(finish) || finish <= cursor) continue;
    while (cursor < finish) {
      const day = startOfDay(new Date(cursor));
      const nextDay = addDays(day, 1).getTime();
      const segmentEnd = Math.min(finish, nextDay);
      const key = format(day, "yyyy-MM-dd");
      const dayIntervals = intervals.get(key) || [];
      dayIntervals.push({ start: cursor, end: segmentEnd });
      intervals.set(key, dayIntervals);
      cursor = segmentEnd;
    }
  }
  const occupancy = new Map<string, number>();
  for (const [day, dayIntervals] of intervals) {
    dayIntervals.sort((a, b) => a.start - b.start);
    let occupiedMs = 0;
    let currentStart = dayIntervals[0].start;
    let currentEnd = dayIntervals[0].end;
    for (const interval of dayIntervals.slice(1)) {
      if (interval.start <= currentEnd) currentEnd = Math.max(currentEnd, interval.end);
      else {
        occupiedMs += currentEnd - currentStart;
        currentStart = interval.start;
        currentEnd = interval.end;
      }
    }
    occupiedMs += currentEnd - currentStart;
    occupancy.set(day, roundDuration(Math.round(occupiedMs / 60_000), roundingMinutes, roundingMode));
  }
  return occupancy;
}
