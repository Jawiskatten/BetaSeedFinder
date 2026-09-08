# League Ragebait v1

A small post-game rating site for League of Legends. Public visitors can submit a verdict; only the allowlisted admin account can read or delete responses.

## Stack

- Next.js
- Supabase Postgres + Auth + RLS
- Vercel-ready

## Supabase

This branch is wired to the existing Supabase project `srplqxdmquaznqjmdcwo` using its public publishable key. No secret/service-role key is included in the repository.

The database migration creates:

- `public.ragebait_responses`
- `public.ragebait_admins`
- RLS allowing public INSERT only
- authenticated SELECT/DELETE only for allowlisted admin email

The current admin allowlist contains `jawiskatten@gmail.com`.

## Local run

```bash
npm install
npm run dev
```

Open `http://localhost:3000` for the public form and `/admin` for the dashboard.

On `/admin`, use **FIRST TIME? CREATE ADMIN ACCOUNT** once, confirm the email if Supabase asks you to, then sign in.

## Deploy

Import this branch into Vercel as a Next.js project. The public Supabase URL/key have safe fallbacks in `lib/supabase.ts`, so the site will function even before adding environment variables. You can still add the values from `.env.example` in Vercel settings.

## Security notes

- Public users cannot SELECT responses through the Supabase API.
- Admin reads are enforced by Postgres RLS, not just hidden UI.
- There is no service-role key in browser code.
- Add Turnstile/rate limiting before sharing the URL broadly if spam becomes a problem.
