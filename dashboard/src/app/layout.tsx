import type { Metadata, Viewport } from "next";

import "./globals.css";

export const metadata: Metadata = {
  title: "NexusFS Control Plane",
  description:
    "Operations dashboard for the NexusFS distributed storage system.",
};

export const viewport: Viewport = {
  colorScheme: "dark",
  themeColor: "#07111f",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
