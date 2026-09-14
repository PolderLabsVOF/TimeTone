"use client";

import * as React from "react";
import { flushSync } from "react-dom";
import { X } from "lucide-react";

import { DatePicker } from "@/components/date-picker";
import { cn } from "@/lib/utils";

type Props = { from?: string; to?: string };

type FilterDateProps = {
  label: string;
  name: string;
  value: string;
  onChange: (value: string) => void;
};

/**
 * One date filter with its own clear control. The picker stores its value in a
 * hidden input, so the cleared state has to reach the DOM before the form is
 * submitted; `flushSync` gives that ordering, and `requestSubmit` keeps this
 * button out of implicit form submission (Enter in the search field).
 *
 * `label` is the bare field name ("From"), not "From date": DatePicker appends
 * the field kind itself, so naming this "From date" would render the trigger as
 * "From date date, ...".
 */
function FilterDate({ label, name, value, onChange }: FilterDateProps) {
  const clear = (event: React.MouseEvent<HTMLButtonElement>) => {
    const form = event.currentTarget.form;
    flushSync(() => onChange(""));
    form?.requestSubmit();
  };

  return (
    <div className="flex min-w-0 flex-1 items-center gap-1">
      <DatePicker
        aria-label={label}
        name={name}
        value={value}
        onChange={onChange}
        surface="light"
        size="sm"
        className="min-w-0 flex-1"
      />
      {value ? (
        <button
          type="button"
          onClick={clear}
          aria-label={`Clear ${label.toLowerCase()} date`}
          title={`Clear ${label.toLowerCase()} date`}
          className={cn(
            "grid size-8 shrink-0 place-items-center rounded-lg text-black/45 transition",
            "hover:bg-black/[.04] hover:text-black focus:outline-none focus:ring-2 focus:ring-[#d8ff62]/55",
          )}
        >
          <X className="size-4" aria-hidden="true" />
        </button>
      ) : null}
    </div>
  );
}

export function EntriesFilterDates({ from, to }: Props) {
  const [fromValue, setFromValue] = React.useState(from ?? "");
  const [toValue, setToValue] = React.useState(to ?? "");
  return (
    <div className="flex min-w-0 gap-2">
      <FilterDate label="From" name="from" value={fromValue} onChange={setFromValue} />
      <FilterDate label="To" name="to" value={toValue} onChange={setToValue} />
    </div>
  );
}
