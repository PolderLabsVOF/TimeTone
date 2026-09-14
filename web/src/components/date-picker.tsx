"use client";

import * as React from "react";
import {
  addMonths,
  addDays,
  endOfMonth,
  endOfWeek,
  format,
  isSameDay,
  isSameMonth,
  isValid,
  parse,
  startOfMonth,
  startOfWeek,
  subMonths,
} from "date-fns";
import { Calendar, ChevronLeft, ChevronRight } from "lucide-react";

import { cn } from "@/lib/utils";
import {
  Popover,
  PopoverPortal,
  PopoverPositioner,
  PopoverPopup,
  PopoverTrigger,
} from "@/components/ui/popover";

export type DatePickerProps = {
  value: string;
  onChange: (value: string) => void;
  id?: string;
  name?: string;
  required?: boolean;
  placeholder?: string;
  /**
   * Surface the picker is placed on. Every dashboard surface is white, so "light"
   * is the default; "dark" keeps the picker legible on a dark card.
   */
  surface?: "light" | "dark";
  disabled?: boolean;
  size?: "sm" | "md";
  className?: string;
  "aria-label"?: string;
};

const WEEKDAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"] as const;

function parseIsoDate(value: string): Date | null {
  if (!value) return null;
  const d = parse(value, "yyyy-MM-dd", new Date());
  return isValid(d) ? d : null;
}

function isoOf(d: Date): string {
  return format(d, "yyyy-MM-dd");
}

/** Moves `day` into the month of `month`, keeping the day of month where it exists. */
function dayInMonth(day: Date, month: Date): Date {
  return new Date(month.getFullYear(), month.getMonth(), Math.min(day.getDate(), endOfMonth(month).getDate()));
}

export function DatePicker({
  value,
  onChange,
  id,
  name,
  required,
  placeholder = "Select date",
  surface = "light",
  disabled,
  size = "md",
  className,
  "aria-label": ariaLabel,
}: DatePickerProps): React.ReactNode {
  const isLight = surface === "light";
  const normalized = parseIsoDate(value);
  const initialCursor = normalized ?? new Date();
  const [cursor, setCursor] = React.useState(initialCursor);
  const [open, setOpen] = React.useState(false);
  const [focused, setFocused] = React.useState<Date | null>(null);
  const [rawText, setRawText] = React.useState(value);
  const dayBtnByIso = React.useRef<Map<string, HTMLButtonElement>>(new Map());

  const formattedValue = normalized ? format(normalized, "d MMM yyyy") : null;
  const labelText = formattedValue ?? placeholder;
  // The caller's aria-label names the field. Appending the value keeps the chosen
  // date in the trigger's accessible name instead of hiding it behind aria-label.
  const triggerLabel = ariaLabel ? `${ariaLabel}, ${formattedValue ?? "no date selected"}` : undefined;

  const prevOpenRef = React.useRef(open);
  React.useEffect(() => {
    const wasOpen = prevOpenRef.current;
    prevOpenRef.current = open;
    if (!wasOpen || open) return;
    setRawText(value);
  }, [open, value]);

  const prevValueRef = React.useRef(value);
  React.useEffect(() => {
    const prev = prevValueRef.current;
    prevValueRef.current = value;
    if (prev === value || !normalized) return;
    setCursor(normalized);
  }, [value, normalized]);

  const monthStart = startOfMonth(cursor);
  const gridStart = startOfWeek(monthStart, { weekStartsOn: 1 });
  const gridEnd = endOfWeek(addDays(gridStart, 41), { weekStartsOn: 1 });
  const days: Date[] = [];
  for (let d = new Date(gridStart); d <= gridEnd; d = addDays(d, 1)) days.push(new Date(d));
  const today = new Date();

  const focusedDate = focused ?? normalized ?? monthStart;
  // The focused day can sit outside the visible month (month buttons, PageUp/PageDown,
  // or an external value change). Carrying it into the visible month keeps exactly one
  // rendered day on the roving tabindex, so focus and the arrow keys cannot fall back
  // to the month that is no longer on screen.
  const activeDate = isSameMonth(focusedDate, cursor) ? focusedDate : dayInMonth(focusedDate, cursor);
  const focusedIso = isoOf(activeDate);

  const selectDay = React.useCallback((d: Date) => {
    const iso = isoOf(d);
    onChange(iso);
    setCursor(d);
    setFocused(d);
    setOpen(false);
  }, [onChange]);

  const commitText = React.useCallback(() => {
    const t = rawText.trim();
    if (t === "") { onChange(""); return; }
    const d = parse(t, "yyyy-MM-dd", new Date());
    if (isValid(d) && format(d, "yyyy-MM-dd") === t) {
      onChange(t);
      setCursor(d);
      setFocused(d);
    }
  }, [onChange, rawText]);

  const onGridKeyDown = (event: React.KeyboardEvent) => {
    let next: Date | null = null;
    switch (event.key) {
      case "ArrowLeft": next = addDays(activeDate, -1); break;
      case "ArrowRight": next = addDays(activeDate, 1); break;
      case "ArrowUp": next = addDays(activeDate, -7); break;
      case "ArrowDown": next = addDays(activeDate, 7); break;
      case "Home": next = startOfWeek(activeDate, { weekStartsOn: 1 }); break;
      case "End": next = endOfWeek(activeDate, { weekStartsOn: 1 }); break;
      case "PageUp": next = subMonths(activeDate, 1); break;
      case "PageDown": next = addMonths(activeDate, 1); break;
      case "Enter":
      case " ": { event.preventDefault(); selectDay(activeDate); return; }
      case "Escape": { event.preventDefault(); setOpen(false); return; }
      default: return;
    }
    if (!next) return;
    event.preventDefault();
    setFocused(next);
    if (!isSameMonth(next, cursor)) setCursor(next);
    requestAnimationFrame(() => dayBtnByIso.current.get(isoOf(next!))?.focus());
  };

  React.useEffect(() => {
    if (!open) return;
    const iso = focusedIso;
    requestAnimationFrame(() => dayBtnByIso.current.get(iso)?.focus());
  }, [open, focusedIso]);

  const triggerHeight = size === "sm" ? "h-9" : "h-10";
  const triggerBase =
    `inline-flex w-full items-center justify-between rounded-lg border px-2.5 text-sm ` +
    `outline-none transition focus:ring-2 focus:ring-[#d8ff62]/55 ` +
    (isLight
      ? "border-black/10 bg-white text-black hover:bg-black/[.02]"
      : "border-white/15 bg-white/8 text-white hover:bg-white/10");

  return (
    <>
      {name ? <input type="hidden" name={name} value={value} /> : null}
      <Popover open={open} onOpenChange={setOpen}>
        <PopoverTrigger
          id={id}
          type="button"
          disabled={disabled}
          aria-label={triggerLabel}
          aria-haspopup="dialog"
          aria-expanded={open}
          className={cn(triggerHeight, triggerBase, !value && (isLight ? "text-black/35" : "text-white/35"), className)}
        >
          <span className={cn("truncate", !value && (isLight ? "text-black/35" : "text-white/35"))}>{labelText}</span>
          <Calendar className={cn("ml-2 size-4 shrink-0", isLight ? "text-black/35" : "text-white/35")} aria-hidden="true" />
        </PopoverTrigger>
        <PopoverPortal>
          <PopoverPositioner
            side="bottom"
            align="start"
            sideOffset={6}
            collisionAvoidance={{ side: "flip", align: "shift", fallbackAxisSide: "none" }}
          >
            <PopoverPopup
              className={cn(
                `w-[19rem] rounded-xl border p-3 shadow-xl ` +
                  (isLight
                    ? "border-black/10 bg-white text-black shadow-black/10"
                    : "border-white/15 bg-[#1d2a22] text-white shadow-black/25"),
              )}
              role="dialog"
              aria-label={ariaLabel ? `${ariaLabel} calendar` : "Calendar"}
            >
              <div className="flex items-center justify-between gap-2">
                <button
                  type="button"
                  aria-label="Previous month"
                  onClick={() => setCursor((c) => subMonths(c, 1))}
                  className={cn(
                    "grid size-7 place-items-center rounded-md transition focus:ring-2 focus:ring-[#d8ff62]/55",
                    isLight ? "hover:bg-black/[.06]" : "hover:bg-white/10",
                  )}
                >
                  <ChevronLeft className="size-4" />
                </button>
                <span aria-live="polite" aria-atomic="true" className="text-sm font-medium tabular-nums">{format(cursor, "MMMM yyyy")}</span>
                <button
                  type="button"
                  aria-label="Next month"
                  onClick={() => setCursor((c) => addMonths(c, 1))}
                  className={cn(
                    "grid size-7 place-items-center rounded-md transition focus:ring-2 focus:ring-[#d8ff62]/55",
                    isLight ? "hover:bg-black/[.06]" : "hover:bg-white/10",
                  )}
                >
                  <ChevronRight className="size-4" />
                </button>
                <button
                  type="button"
                  onClick={() => { const d = new Date(); setCursor(d); setFocused(d); }}
                  className="ml-1 rounded-md bg-[#d8ff62] px-2 py-1 text-xs font-semibold text-[#17211b] transition hover:bg-[#c9ef58] focus:ring-2 focus:ring-[#d8ff62]/55"
                >
                  Today
                </button>
              </div>

              <table
                role="grid"
                aria-label="Choose date"
                className="mt-3 w-full border-collapse"
                onKeyDown={onGridKeyDown}
              >
                <thead>
                  <tr>
                    {WEEKDAYS.map((d) => (
                      <th key={d} scope="col" className={cn("py-1 text-center text-[11px] font-medium", isLight ? "text-black/45" : "text-white/45")}>{d}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {Array.from({ length: 6 }, (_, week) => (
                    <tr key={week}>
                      {days.slice(week * 7, week * 7 + 7).map((d) => {
                        const iso = isoOf(d);
                        const isSelected = value !== "" && normalized !== null && isSameDay(d, normalized);
                        const isToday = isSameDay(d, today);
                        const outside = !isSameMonth(d, cursor);
                        const isFocused = iso === focusedIso;
                        return (
                          <td
                            key={iso}
                            role="gridcell"
                            aria-selected={isSelected}
                            className="p-0.5 text-center"
                          >
                            <button
                              ref={(el) => {
                                if (el) dayBtnByIso.current.set(iso, el);
                                else dayBtnByIso.current.delete(iso);
                              }}
                              type="button"
                              tabIndex={isFocused ? 0 : -1}
                              aria-label={format(d, "EEEE, d MMMM yyyy")}
                              aria-current={isToday ? "date" : undefined}
                              onClick={() => selectDay(d)}
                              onFocus={() => setFocused(d)}
                              className={cn(
                                "grid size-8 place-items-center rounded-md text-sm tabular-nums transition focus:ring-2 focus:ring-[#d8ff62]/55",
                                outside && (isLight ? "text-black/35" : "text-white/35"),
                                isToday && !isSelected && "ring-1 ring-[#d8ff62]",
                                isSelected
                                  ? "bg-[#d8ff62] font-semibold text-[#17211b]"
                                  : isLight
                                    ? "hover:bg-black/[.06]"
                                    : "hover:bg-white/10",
                              )}
                            >
                              {String(d.getDate())}
                            </button>
                          </td>
                        );
                      })}
                    </tr>
                  ))}
                </tbody>
              </table>

              <div className={cn("mt-3 border-t pt-3", isLight ? "border-black/10" : "border-white/10")}>
                <label className={cn("text-xs", isLight ? "text-black/45" : "text-white/45")} htmlFor={id ? `${id}-fallback` : undefined}>
                  Or type yyyy-MM-dd
                </label>
                <input
                  id={id ? `${id}-fallback` : undefined}
                  type="text"
                  inputMode="numeric"
                  placeholder="yyyy-MM-dd"
                  value={rawText}
                  onChange={(e) => setRawText(e.target.value)}
                  onBlur={commitText}
                  onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); commitText(); } }}
                  className={cn(
                    `mt-1 h-9 w-full rounded-lg border px-2.5 text-sm outline-none transition focus:ring-2 focus:ring-[#d8ff62]/55`,
                    isLight ? "border-black/10 bg-white text-black placeholder:text-black/35" : "border-white/15 bg-white/8 text-white placeholder:text-white/35",
                  )}
                />
                {required && value === "" ? (
                  <p className="mt-1 text-xs text-red-600">Choose a date.</p>
                ) : null}
              </div>
            </PopoverPopup>
          </PopoverPositioner>
        </PopoverPortal>
      </Popover>
    </>
  );
}
