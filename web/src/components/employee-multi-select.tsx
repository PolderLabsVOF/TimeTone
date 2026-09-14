"use client";

import { Check, ChevronDown, Users } from "lucide-react";
import { useEffect, useId, useRef, useState } from "react";
import { Label } from "@/components/ui/label";

type Employee = { id: string; name: string };

export function EmployeeMultiSelect({ employees }: { employees: Employee[] }) {
  const generatedId = useId();
  const panelId = `employees-${generatedId}`;
  const [selectedIds, setSelectedIds] = useState<string[]>([]);
  const [open, setOpen] = useState(false);
  const [missingSelection, setMissingSelection] = useState(false);
  const pickerRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const selectedEmployees = employees.filter((employee) => selectedIds.includes(employee.id));
  const label = selectedEmployees.length === 0
    ? "Choose employees"
    : selectedEmployees.length === 1
      ? selectedEmployees[0].name
      : `${selectedEmployees.length} employees selected`;

  useEffect(() => {
    if (!open) return;
    const close = (event: MouseEvent) => {
      if (!pickerRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const escape = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", escape);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", escape);
    };
  }, [open]);

  useEffect(() => {
    const form = pickerRef.current?.closest("form");
    if (!form) return;
    const validateSelection = (event: SubmitEvent) => {
      if (selectedIds.length) return;
      event.preventDefault();
      setMissingSelection(true);
      setOpen(true);
      requestAnimationFrame(() => triggerRef.current?.focus());
    };
    form.addEventListener("submit", validateSelection);
    return () => form.removeEventListener("submit", validateSelection);
  }, [selectedIds.length]);

  const toggleEmployee = (employeeId: string) => {
    setSelectedIds((current) => current.includes(employeeId)
      ? current.filter((id) => id !== employeeId)
      : [...current, employeeId]);
    setMissingSelection(false);
  };

  return (
    <div className="relative space-y-2" ref={pickerRef}>
      <Label htmlFor={panelId}>Employees</Label>
      {selectedIds.map((employeeId) => <input key={employeeId} type="hidden" name="employee_ids" value={employeeId} />)}
      <button
        ref={triggerRef}
        id={panelId}
        type="button"
        aria-expanded={open}
        aria-controls={`${panelId}-options`}
        aria-haspopup="dialog"
        onClick={() => setOpen((visible) => !visible)}
        className={`inline-flex h-10 w-full items-center gap-2 rounded-lg border px-3 text-left text-sm outline-none transition hover:bg-white/12 focus:ring-2 focus:ring-[#d8ff62]/55 ${missingSelection ? "border-red-400" : "border-white/15"}`}
      >
        <Users className="size-4 shrink-0 text-white/45" aria-hidden="true" />
        <span className={`min-w-0 flex-1 truncate ${selectedIds.length ? "text-white" : "text-white/45"}`}>{label}</span>
        <ChevronDown className={`size-4 shrink-0 text-white/55 transition-transform ${open ? "rotate-180" : ""}`} aria-hidden="true" />
      </button>
      {missingSelection && <p className="text-xs text-red-300">Choose at least one employee.</p>}
      {open && (
        <div id={`${panelId}-options`} role="dialog" aria-label="Choose employees" className="absolute z-20 mt-1 w-full rounded-xl border border-white/15 bg-[#1d2a22] p-2 shadow-xl shadow-black/25">
          <div className="max-h-52 space-y-1 overflow-y-auto">
            {employees.map((employee) => {
              const selected = selectedIds.includes(employee.id);
              return (
                <label key={employee.id} className="flex min-h-10 cursor-pointer items-center gap-3 rounded-lg px-2.5 text-sm transition hover:bg-white/8">
                  <input type="checkbox" checked={selected} onChange={() => toggleEmployee(employee.id)} className="sr-only" />
                  <span className={`grid size-5 shrink-0 place-items-center rounded border ${selected ? "border-[#d8ff62] bg-[#d8ff62] text-[#17211b]" : "border-white/30"}`} aria-hidden="true">
                    {selected && <Check className="size-3.5" />}
                  </span>
                  <span className="min-w-0 flex-1 truncate">{employee.name}</span>
                </label>
              );
            })}
          </div>
          <div className="mt-2 flex items-center justify-between border-t border-white/10 pt-2">
            <span className="text-xs text-white/45">{selectedIds.length} selected</span>
            <button type="button" onClick={() => setOpen(false)} className="h-8 rounded-md bg-white/10 px-2.5 text-xs font-medium transition hover:bg-white/15 focus:ring-2 focus:ring-[#d8ff62]/55">Done</button>
          </div>
        </div>
      )}
    </div>
  );
}
