"use client";

import { Clock3, ChevronDown, X } from "lucide-react";
import { useEffect, useId, useRef, useState } from "react";
import { Label } from "@/components/ui/label";

type Props = {
  label: string;
  name: string;
  id?: string;
  defaultValue?: string;
  required?: boolean;
  light?: boolean;
};

const MINUTES = ["00", "15", "30", "45"];

function splitDateTime(value?: string) {
  if (!value) return { date: "", time: "" };
  const [date = "", time = ""] = value.split("T");
  return { date, time: time.slice(0, 5) };
}

export function TimeEntryDateField({ label, name, id, defaultValue, required, light }: Props) {
  const initial = splitDateTime(defaultValue);
  const generatedId = useId();
  const fieldId = id || `${name}-${generatedId}`;
  const panelId = `${fieldId}-time-picker`;
  const [date, setDate] = useState(initial.date);
  const [time, setTime] = useState(initial.time);
  const [open, setOpen] = useState(false);
  const [missingTime, setMissingTime] = useState(false);
  const pickerRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);

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
    if (!required) return;
    const form = pickerRef.current?.closest("form");
    if (!form) return;

    const validateTime = (event: SubmitEvent) => {
      if (!date || time) return;
      event.preventDefault();
      setMissingTime(true);
      setOpen(true);
      requestAnimationFrame(() => inputRef.current?.focus());
    };

    form.addEventListener("submit", validateTime);
    return () => form.removeEventListener("submit", validateTime);
  }, [date, required, time]);

  const chooseTime = (hour: string, minute: string) => {
    setTime(`${hour}:${minute}`);
    setMissingTime(false);
    setOpen(false);
  };
  const clearTime = () => {
    setTime("");
    setOpen(false);
  };
  const hour = time ? time.slice(0, 2) : "09";
  const inputClass = light
    ? "border-black/10 bg-white text-black"
    : "border-white/15 bg-white/8 text-white [color-scheme:dark]";
  const mutedClass = light ? "text-black/45" : "text-white/45";
  const panelClass = light
    ? "border-black/10 bg-white text-black shadow-xl shadow-black/10"
    : "border-white/15 bg-[#1d2a22] text-white shadow-xl shadow-black/25";

  return (
    <div className="relative space-y-2" ref={pickerRef}>
      <Label htmlFor={fieldId}>{label}</Label>
      <input type="hidden" name={name} value={date && time ? `${date}T${time}` : ""} />
      <div className="grid grid-cols-[minmax(0,1fr)_8.5rem] gap-2">
        <input
          id={fieldId}
          type="date"
          value={date}
          required={required}
          onChange={(event) => setDate(event.target.value)}
          className={`h-10 min-w-0 rounded-lg border px-2.5 text-sm outline-none transition focus:border-[#526b38] focus:ring-2 focus:ring-[#d8ff62]/45 ${inputClass}`}
        />
        <button
          type="button"
          aria-expanded={open}
          aria-controls={panelId}
          aria-haspopup="dialog"
          onClick={() => {
            setOpen((visible) => !visible);
            requestAnimationFrame(() => inputRef.current?.focus());
          }}
          className={`inline-flex h-10 items-center justify-between rounded-lg border px-2.5 text-sm outline-none transition hover:bg-black/[.04] focus:ring-2 focus:ring-[#d8ff62]/55 ${missingTime ? "border-red-500" : ""} ${inputClass}`}
        >
          <span className={time ? "tabular-nums" : mutedClass}>{time || "Set time"}</span>
          <ChevronDown className={`size-4 transition-transform ${open ? "rotate-180" : ""}`} aria-hidden="true" />
        </button>
      </div>
      {missingTime && <p className="text-xs text-red-600">Choose a clock-in time.</p>}
      {open && (
        <div id={panelId} role="dialog" aria-label={`${label} time picker`} className={`absolute z-20 mt-1 w-[min(22rem,calc(100vw-2rem))] rounded-xl border p-3 ${panelClass}`}>
          <div className="flex items-center gap-2">
            <Clock3 className={`size-4 ${mutedClass}`} aria-hidden="true" />
            <input
              ref={inputRef}
              type="time"
              value={time}
              onChange={(event) => {
                setTime(event.target.value);
                setMissingTime(false);
              }}
              className={`h-9 flex-1 rounded-md border px-2 text-sm tabular-nums outline-none focus:ring-2 focus:ring-[#d8ff62]/55 ${inputClass}`}
              aria-label={`${label} time`}
            />
            {time && <button type="button" onClick={clearTime} className={`grid size-9 place-items-center rounded-md transition hover:bg-black/[.06] ${mutedClass}`} aria-label={`Clear ${label.toLowerCase()} time`}><X className="size-4" /></button>}
          </div>
          <div className="mt-3 grid grid-cols-4 gap-1" aria-label="Quick time choices">
            {MINUTES.map((minute) => (
              <button key={minute} type="button" onClick={() => chooseTime(hour, minute)} className={`h-9 rounded-md text-sm tabular-nums transition focus:ring-2 focus:ring-[#d8ff62]/55 ${time === `${hour}:${minute}` ? "bg-[#d8ff62] font-semibold text-[#17211b]" : light ? "bg-black/[.035] hover:bg-black/[.07]" : "bg-white/8 hover:bg-white/14"}`}>
                {hour}:{minute}
              </button>
            ))}
          </div>
          <div className="mt-3 grid grid-cols-7 gap-1" role="group" aria-label="Hour">
            {["06", "07", "08", "09", "10", "11", "12", "13", "14", "15", "16", "17", "18"].map((value) => (
              <button key={value} type="button" onClick={() => setTime(`${value}:${time ? time.slice(3) : "00"}`)} className={`h-8 rounded-md text-xs tabular-nums transition ${hour === value ? "bg-[#d8ff62] font-semibold text-[#17211b]" : light ? "hover:bg-black/[.06]" : "hover:bg-white/12"}`}>{value}</button>
            ))}
          </div>
          <p className={`mt-2 text-[11px] ${mutedClass}`}>Type an exact time, or choose an hour and quarter-hour.</p>
        </div>
      )}
    </div>
  );
}
