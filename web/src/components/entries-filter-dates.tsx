"use client";

import * as React from "react";
import { DatePicker } from "@/components/date-picker";

type Props = { from?: string; to?: string };

export function EntriesFilterDates({ from, to }: Props) {
  const [fromValue, setFromValue] = React.useState(from ?? "");
  const [toValue, setToValue] = React.useState(to ?? "");
  return (
    <div className="flex min-w-0 gap-2">
      <DatePicker aria-label="From date" name="from" value={fromValue} onChange={setFromValue} size="sm" />
      <DatePicker aria-label="To date" name="to" value={toValue} onChange={setToValue} size="sm" />
    </div>
  );
}
