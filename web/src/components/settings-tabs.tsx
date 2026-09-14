"use client";

import type { ReactNode } from "react";
import { ArchiveRestore, Clock3, KeyRound, Settings2, SlidersHorizontal } from "lucide-react";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";

type SettingsTab = {
  value: string;
  label: string;
  icon: "workspace" | "time" | "reports" | "security" | "data";
  content: ReactNode;
};

const icons = {
  workspace: Settings2,
  time: Clock3,
  reports: SlidersHorizontal,
  security: KeyRound,
  data: ArchiveRestore,
} as const;

export function SettingsTabs({ defaultValue, tabs }: { defaultValue: string; tabs: SettingsTab[] }) {
  return (
    <Tabs defaultValue={defaultValue} className="w-full">
      <TabsList className="!h-auto min-h-14 grid w-full grid-cols-2 items-stretch gap-1 rounded-xl bg-[#eef0eb] p-1 dark:bg-[#243127] sm:grid-cols-3 lg:grid-cols-5">
        {tabs.map((tab) => {
          const Icon = icons[tab.icon];
          return (
            <TabsTrigger key={tab.value} value={tab.value} className="!h-auto min-h-12 min-w-0 justify-center px-2 py-2 text-center text-xs leading-4 whitespace-normal sm:px-3 sm:text-sm">
              <Icon className="size-4" />
              <span>{tab.label}</span>
            </TabsTrigger>
          );
        })}
      </TabsList>
      {tabs.map((tab) => (
        <TabsContent
          key={tab.value}
          value={tab.value}
          keepMounted
          className="mt-5"
        >
          {tab.content}
        </TabsContent>
      ))}
    </Tabs>
  );
}
