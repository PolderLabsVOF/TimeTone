import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";

const geistSans = Geist({ variable: "--font-geist-sans", subsets: ["latin"] });

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: { default: "TimeTone", template: "%s · TimeTone" },
  description: "Office time tracking for the ESP32 Cheap Yellow Display",
  icons: { icon: "/timetone-mark.svg" },
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="en"
      className={`${geistSans.variable} ${geistMono.variable} antialiased`}
      suppressHydrationWarning
    >
      <head>
        <script
          // Runs before the page is painted, preventing a light-mode flash
          // when a visitor has previously selected dark mode.
          dangerouslySetInnerHTML={{ __html: `(function(){try{var saved=localStorage.getItem('timekeep-theme');var dark=saved?saved==='dark':matchMedia('(prefers-color-scheme: dark)').matches;document.documentElement.classList.toggle('dark',dark);document.documentElement.style.colorScheme=dark?'dark':'light'}catch(e){}})()` }}
        />
      </head>
      <body>{children}</body>
    </html>
  );
}
