import { createClient } from '@supabase/supabase-js';

const supabaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL || 'https://srplqxdmquaznqjmdcwo.supabase.co';
const supabaseKey = process.env.NEXT_PUBLIC_SUPABASE_PUBLISHABLE_KEY || 'sb_publishable_VNDb_oG58qHRqBps3BwdJQ_yUEGRQK2';

export const supabase = createClient(supabaseUrl, supabaseKey);
