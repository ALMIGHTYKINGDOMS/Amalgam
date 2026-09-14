-- Authenticated beta feedback. Diagnostics are intentionally caller-supplied
-- summaries; credentials and raw logs must never be stored here.
create table if not exists public.beta_feedback (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    category text not null check (category in ('bug', 'ui', 'performance', 'feature', 'other')),
    rating integer not null check (rating between 1 and 5),
    message text not null check (char_length(message) between 1 and 4000),
    page text not null default '',
    app_version text not null default '',
    diagnostics jsonb not null default '{}'::jsonb,
    status text not null default 'open' check (status in ('open', 'triaged', 'resolved', 'closed')),
    created_at timestamptz not null default now()
);

alter table public.beta_feedback enable row level security;
create policy "users submit own beta feedback" on public.beta_feedback
    for insert to authenticated with check (user_id = auth.uid());
create policy "staff review beta feedback" on public.beta_feedback
    for select to authenticated using (public.is_project_staff());

create or replace function public.submit_beta_feedback(
    category text, rating integer, message text, page text default '',
    app_version text default '', diagnostics jsonb default '{}'::jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
declare feedback_id uuid;
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    insert into public.beta_feedback(user_id, category, rating, message, page, app_version, diagnostics)
    values (auth.uid(), category, rating, left(message, 4000), left(page, 120), left(app_version, 64), diagnostics)
    returning id into feedback_id;
    return jsonb_build_object('success', true, 'id', feedback_id);
end;
$$;

revoke all on function public.submit_beta_feedback(text, integer, text, text, text, jsonb) from public;
grant execute on function public.submit_beta_feedback(text, integer, text, text, text, jsonb) to authenticated;
