'use client';

import { FormEvent, useEffect, useMemo, useState } from 'react';
import { supabase } from '@/lib/supabase';

type ResponseRow = {
  id: string;
  created_at: string;
  match_label: string | null;
  respondent_name: string | null;
  performance_rating: number | null;
  enemy_support_gap: string | null;
  loss_blame: string | null;
  would_queue_again: string | null;
  tilt_rating: number | null;
  comment: string | null;
};

export default function AdminPage() {
  const [sessionReady, setSessionReady] = useState(false);
  const [loggedIn, setLoggedIn] = useState(false);
  const [email, setEmail] = useState('jawiskatten@gmail.com');
  const [password, setPassword] = useState('');
  const [message, setMessage] = useState('');
  const [rows, setRows] = useState<ResponseRow[]>([]);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    supabase.auth.getSession().then(({ data }) => {
      setLoggedIn(Boolean(data.session));
      setSessionReady(true);
    });
    const { data: listener } = supabase.auth.onAuthStateChange((_event, session) => setLoggedIn(Boolean(session)));
    return () => listener.subscription.unsubscribe();
  }, []);

  useEffect(() => { if (loggedIn) loadRows(); }, [loggedIn]);

  async function login(e: FormEvent) {
    e.preventDefault();
    setMessage('');
    const { error } = await supabase.auth.signInWithPassword({ email, password });
    if (error) setMessage(error.message);
  }

  async function signup() {
    setMessage('');
    const { error } = await supabase.auth.signUp({ email, password });
    setMessage(error ? error.message : 'Account created. If email confirmation is enabled, confirm the email, then sign in.');
  }

  async function loadRows() {
    setLoading(true);
    const { data, error } = await supabase.from('ragebait_responses').select('*').order('created_at', { ascending: false }).limit(500);
    setLoading(false);
    if (error) { setMessage(error.message); setRows([]); return; }
    setRows((data || []) as ResponseRow[]);
  }

  async function remove(id: string) {
    if (!window.confirm('Delete this response?')) return;
    const { error } = await supabase.from('ragebait_responses').delete().eq('id', id);
    if (error) setMessage(error.message); else setRows((r) => r.filter((x) => x.id !== id));
  }

  const stats = useMemo(() => {
    if (!rows.length) return { avg: '—', tilt: '—' };
    const ratings = rows.map((r) => r.performance_rating).filter((n): n is number => typeof n === 'number');
    const tilts = rows.map((r) => r.tilt_rating).filter((n): n is number => typeof n === 'number');
    return {
      avg: ratings.length ? (ratings.reduce((a, b) => a + b, 0) / ratings.length).toFixed(1) : '—',
      tilt: tilts.length ? (tilts.reduce((a, b) => a + b, 0) / tilts.length).toFixed(1) : '—',
    };
  }, [rows]);

  if (!sessionReady) return <main className="admin-shell"><p>Loading...</p></main>;

  if (!loggedIn) {
    return (
      <main className="admin-shell centered">
        <form className="login-card glass" onSubmit={login}>
          <p className="eyebrow">PRIVATE AREA</p>
          <h1>ALISTAR CONTROL ROOM</h1>
          <label><span>Email</span><input type="email" value={email} onChange={(e) => setEmail(e.target.value)} /></label>
          <label><span>Password</span><input type="password" value={password} onChange={(e) => setPassword(e.target.value)} minLength={6} /></label>
          <button className="primary-button">SIGN IN</button>
          <button type="button" className="ghost-button" onClick={signup}>FIRST TIME? CREATE ADMIN ACCOUNT</button>
          {message && <p className="info">{message}</p>}
        </form>
      </main>
    );
  }

  return (
    <main className="admin-shell">
      <header className="admin-header">
        <div><p className="eyebrow">ALISTAR GAP</p><h1>Response Dashboard</h1></div>
        <div className="header-actions"><button className="ghost-button" onClick={loadRows}>Refresh</button><button className="ghost-button" onClick={() => supabase.auth.signOut()}>Sign out</button></div>
      </header>

      <section className="stats-grid">
        <Stat label="Responses" value={String(rows.length)} />
        <Stat label="Average performance" value={`${stats.avg}/10`} />
        <Stat label="Average tilt caused" value={`${stats.tilt}/10`} />
      </section>

      {message && <p className="info">{message}</p>}
      <section className="responses">
        {loading && <p>Loading responses...</p>}
        {!loading && !rows.length && <div className="empty glass">No victims have submitted a verdict yet.</div>}
        {rows.map((row) => (
          <article className="response-card glass" key={row.id}>
            <div className="response-top">
              <div><strong>{row.respondent_name || 'Anonymous victim'}</strong><span>{new Date(row.created_at).toLocaleString()}</span></div>
              <button className="delete" onClick={() => remove(row.id)}>Delete</button>
            </div>
            <div className="response-metrics">
              <span>Performance <b>{row.performance_rating ?? '—'}/10</b></span>
              <span>Tilt <b>{row.tilt_rating ?? '—'}/10</b></span>
              <span>Gap <b>{pretty(row.enemy_support_gap)}</b></span>
              <span>Blame <b>{pretty(row.loss_blame)}</b></span>
              <span>Queue again <b>{pretty(row.would_queue_again)}</b></span>
            </div>
            {row.match_label && <p className="match-label">Match: {row.match_label}</p>}
            {row.comment && <blockquote>{row.comment}</blockquote>}
          </article>
        ))}
      </section>
    </main>
  );
}

function pretty(value: string | null) { return value ? value.replaceAll('_', ' ') : '—'; }
function Stat({ label, value }: { label: string; value: string }) { return <div className="stat glass"><span>{label}</span><strong>{value}</strong></div>; }
