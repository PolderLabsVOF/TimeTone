"use client";

import * as React from "react";
import { Pencil, Trash2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { TimeEntryDateField } from "@/components/time-entry-date-field";
import {
  Popover,
  PopoverPortal,
  PopoverPositioner,
  PopoverPopup,
  PopoverTrigger,
} from "@/components/ui/popover";
import type { Employee } from "@/lib/domain";

export function EditEntryPopover(props: {
  id: string;
  employeeId: string;
  clockIn: string;
  clockOut: string | null;
  note: string | null;
  allEmployees: Employee[];
  updateAction: (formData: FormData) => void;
  deleteAction: (formData: FormData) => void;
}) {
  const { id, employeeId, clockIn, clockOut, note, allEmployees, updateAction, deleteAction } = props;
  const [open, setOpen] = React.useState(false);

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger
        type="button"
        className="inline-flex h-8 items-center gap-1.5 rounded-lg border border-black/10 px-2.5 text-xs font-medium text-black/60 transition-colors hover:bg-black/[.03]"
      >
        <Pencil className="size-3.5" />
        Edit
      </PopoverTrigger>
      <PopoverPortal>
        <PopoverPositioner side="bottom" align="end" sideOffset={6} collisionAvoidance={{ side: "flip", align: "shift", fallbackAxisSide: "none" }} collisionPadding={16}>
          <PopoverPopup
            role="dialog"
            aria-label="Edit time entry"
            className="max-h-[calc(100vh-2rem)] w-[min(21rem,calc(100vw-2rem))] overflow-y-auto rounded-2xl border border-black/10 bg-white p-4 text-left shadow-xl shadow-black/10"
            style={{ maxHeight: "min(calc(100vh - 2rem), var(--available-height, 100vh - 2rem))" } as React.CSSProperties}
          >
            <div className="mb-3">
              <p className="text-sm font-semibold">Edit time entry</p>
              <p className="mt-0.5 text-xs text-black/45">Adjust the raw times; reports will recalculate automatically.</p>
            </div>
            <form action={updateAction} className="space-y-3">
              <input type="hidden" name="id" value={id} />
              <div className="space-y-1.5">
                <Label htmlFor={`employee-${id}`}>Employee</Label>
                <select
                  id={`employee-${id}`}
                  name="employee_id"
                  defaultValue={employeeId}
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
              <TimeEntryDateField label="Clock in" name="clock_in" id={`clock-in-${id}`} defaultValue={clockIn} required light size="sm" />
              <TimeEntryDateField label="Clock out" name="clock_out" id={`clock-out-${id}`} defaultValue={clockOut ?? ""} light size="sm" />
              {!clockOut && <p className="-mt-1 text-xs leading-4 text-emerald-700">This entry is open. Leave Clock out empty to keep it open, or set a time to close it.</p>}
              <div className="space-y-1.5">
                <Label htmlFor={`note-${id}`}>Note</Label>
                <Input id={`note-${id}`} name="note" defaultValue={note || ""} placeholder="Reason for correction" className="h-9 border-black/10 bg-white" />
              </div>
              <Button type="submit" size="sm" className="w-full bg-[#17211b] text-white hover:bg-[#26352c]">Save changes</Button>
            </form>
            <form action={deleteAction} className="mt-2">
              <input type="hidden" name="id" value={id} />
              <Button type="submit" size="xs" variant="ghost" className="w-full text-red-600 hover:bg-red-50 hover:text-red-700">
                <Trash2 className="size-3" />Delete entry
              </Button>
            </form>
          </PopoverPopup>
        </PopoverPositioner>
      </PopoverPortal>
    </Popover>
  );
}
