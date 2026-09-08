'use client';

import { FormEvent, useMemo, useState } from 'react';
import { supabase } from '@/lib/supabase';

type FormState = {
  respondent_name: string;
  match_label: string;
  performance_rating: number;
  enemy_support_gap: 'obviously' | 'yes' | 'delusional';
  loss_blame: 'me' | 'you' | 'our_adc' | 'riot_games' | 'enemy_smurfing';
  would_queue_again: 'yes' | 'no' | 'if_carried';
  tilt_rating: number;
  comment: string;
};

const initialForm: FormState = {
  respondent_name: '',
  match_label: '',
  performance_rating: 10,
  enemy_support_gap: 'obviously',
  loss_blame: 'you',
  would_queue_again: 'yes',
  tilt_rating: 5,
  comment: '',
};

export default function Home() {
  const [form, setForm] = useState<FormState>(initialForm);
  const [status, setStatus] = useState<'idle' | 'sending' | 'done' | 'error'>('idle');
  const [error, setError] = useState('');
  const verdict = useMemo(() => form.performance_rating >= 8 ? 'CERTIFIED GAP' : form.performance_rating >= 5 ? 'ACCEPTABLE COW' : 'HATER DETECTED', [form.performance_rating]);

  function update<K extends keyof FormState>(key: K, value: FormState[K]) {
    setForm((prev) => ({ ...prev, [key]: value }));
  }

  async function submit(e: FormEvent) {
    e.preventDefault();
    if (status === 'sending') return;
    setStatus('sending');
    setError('');

    const payload = {
      form_version: 'v1',
      champion: 'Alistar',
      match_label: form.match_label.trim() || null,
      respondent_name: form.respondent_name.trim() || null,
      performance_rating: form.performance_rating,
      enemy_support_gap: form.enemy_support_gap,
      loss_blame: form.loss_blame,
      would_queue_again: form.would_queue_again,
      tilt_rating: form.tilt_rating,
      comment: form.comment.trim() || null,
      answers: {},
    };

    const { error: insertError } = await supabase.from('ragebait_responses').insert(payload);
    if (insertError) {
      setError(insertError.message);
      setStatus('error');
      return;
    }

    setStatus('done');
  }

  if (status === 'done') {
    return (
      <main className="page-shell centered">
        <section className="success-card glass">
          <div className="cow-orb">🐮</div>
          <p className="eyebrow">VERDICT RECEIVED</p>
          <h1>THANK YOU FOR YOUR<br />VALUABLE COPIUM</h1>
          <p>Your opinion has been stored permanently and may be used against you in future solo queue arguments.</p>
          <button className="primary-button" onClick={() => { setForm(initialForm); setStatus('idle'); }}>RATE ANOTHER GAME</button>
        </section>
      </main>
    );
  }

  return (
    <main className="page-shell">
      <div className="stars" aria-hidden="true" />
      <section className="hero">
        <div className="hero-orb"><span>🐮</span></div>
        <p className="eyebrow">YOU HAVE BEEN SELECTED TO RATE</p>
        <h1>ALISTAR GAP</h1>
        <p className="subtitle">Please submit your completely objective post-game analysis.</p>
      </section>

      <form className="form-card glass" onSubmit={submit}>
        <div className="two-col">
          <label>
            <span>Your summoner name <b>optional</b></span>
            <input maxLength={80} value={form.respondent_name} onChange={(e) => update('respondent_name', e.target.value)} placeholder="EnemySupportDiff#EUW" />
          </label>
          <label>
            <span>Match label <b>optional</b></span>
            <input maxLength={80} value={form.match_label} onChange={(e) => update('match_label', e.target.value)} placeholder="23:41 loss / Game 184" />
          </label>
        </div>

        <fieldset>
          <legend>How good was my Alistar performance?</legend>
          <div className="rating-row">
            {Array.from({ length: 10 }, (_, i) => i + 1).map((n) => (
              <button type="button" key={n} className={form.performance_rating === n ? 'rating active' : 'rating'} onClick={() => update('performance_rating', n)}>{n}</button>
            ))}
          </div>
          <p className="verdict">{verdict}</p>
        </fieldset>

        <Choice title="Did I gap the enemy support?" value={form.enemy_support_gap} onChange={(v) => update('enemy_support_gap', v as FormState['enemy_support_gap'])} options={[
          ['obviously', 'Obviously'], ['yes', 'Yes'], ['delusional', 'No, I am delusional'],
        ]} />

        <Choice title="Who was responsible for the loss?" value={form.loss_blame} onChange={(v) => update('loss_blame', v as FormState['loss_blame'])} options={[
          ['me', 'You (the Alistar)'], ['you', 'Me'], ['our_adc', 'Our ADC'], ['riot_games', 'Riot Games'], ['enemy_smurfing', 'Enemy smurfing'],
        ]} />

        <Choice title="Would you willingly queue with this Alistar again?" value={form.would_queue_again} onChange={(v) => update('would_queue_again', v as FormState['would_queue_again'])} options={[
          ['yes', 'Yes'], ['if_carried', 'Only if I get carried'], ['no', 'Absolutely not'],
        ]} />

        <fieldset>
          <legend>How tilted did I make you?</legend>
          <div className="range-head"><span>Zen</span><strong>{form.tilt_rating}/10</strong><span>Keyboard endangered</span></div>
          <input className="slider" type="range" min="1" max="10" value={form.tilt_rating} onChange={(e) => update('tilt_rating', Number(e.target.value))} />
        </fieldset>

        <label>
          <span>Final statement for the tribunal</span>
          <textarea maxLength={1000} rows={5} value={form.comment} onChange={(e) => update('comment', e.target.value)} placeholder="Explain in detail why the engage at 22:17 was actually genius..." />
        </label>

        {error && <p className="error">Submission failed: {error}</p>}
        <button className="primary-button submit" disabled={status === 'sending'}>{status === 'sending' ? 'UPLOADING COPIUM...' : 'SUBMIT YOUR VERDICT'}</button>
        <p className="fineprint">Responses are stored privately. Other players cannot read submissions.</p>
      </form>
    </main>
  );
}

function Choice({ title, value, options, onChange }: { title: string; value: string; options: [string, string][]; onChange: (value: string) => void }) {
  return (
    <fieldset>
      <legend>{title}</legend>
      <div className="choice-grid">
        {options.map(([key, label]) => (
          <button type="button" key={key} className={value === key ? 'choice active' : 'choice'} onClick={() => onChange(key)}>{label}</button>
        ))}
      </div>
    </fieldset>
  );
}
