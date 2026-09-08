import type { Metadata } from 'next';
import './globals.css';

export const metadata: Metadata = {
  title: 'ALISTAR GAP — Rate My Game',
  description: 'You have been selected to rate this Alistar performance.',
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
