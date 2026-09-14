-- 202608210008 — rate limiting (direct-RPC paths), admin privacy, TURN quota.
--
-- 1. Rate limiting: the Edge Functions already call enforce_rate_limit for the
--    social layer, but the launcher calls several Essentials RPCs directly via
--    PostgREST (create_session, join_session, send_session_invite, block_user,
--    submit_beta_feedback, upsert_session_manifest). Those write paths now get
--    server-authoritative per-user limits through table triggers, so no RPC
--    body transcription is required and every insert path is covered.
-- 2. Admin privacy: admin_list_users no longer returns full email addresses.
--    Staff get a masked email; the full address is available only through an
--    explicit, staff-gated admin_get_user_detail lookup.
-- 3. TURN accounting: authoritative quota evaluation and service-role-only
--    usage ingestion. Normal users can read their allowance and check the
--    quota, but CANNOT write usage (client-reported bytes are never trusted).

-- ---------------------------------------------------------------------------
-- 1. Rate limiting
-- ---------------------------------------------------------------------------

create or replace function public.enforce_table_rate_limit()
returns trigger language plpgsql security definer set search_path = public as $$
begin
    if not public.enforce_rate_limit(TG_ARGV[0], TG_ARGV[1]::integer) then
        raise exception 'rate limit exceeded for %', TG_ARGV[0]
            using errcode = 'RATE_LMT';
    end if;
    return new;
end;
$$;

drop trigger if exists trg_rate_limit_create_session on public.essentials_sessions;
create trigger trg_rate_limit_create_session
before insert on public.essentials_sessions
for each row execute function public.enforce_table_rate_limit('create_session', 5);

drop trigger if exists trg_rate_limit_session_invite on public.essentials_invites;
create trigger trg_rate_limit_session_invite
before insert on public.essentials_invites
for each row execute function public.enforce_table_rate_limit('send_session_invite', 10);

drop trigger if exists trg_rate_limit_join_session on public.essentials_session_members;
create trigger trg_rate_limit_join_session
before insert on public.essentials_session_members
for each row execute function public.enforce_table_rate_limit('join_session', 10);

drop trigger if exists trg_rate_limit_block_user on public.blocked_users;
create trigger trg_rate_limit_block_user
before insert on public.blocked_users
for each row execute function public.enforce_table_rate_limit('block_user', 10);

drop trigger if exists trg_rate_limit_beta_feedback on public.beta_feedback;
create trigger trg_rate_limit_beta_feedback
before insert on public.beta_feedback
for each row execute function public.enforce_table_rate_limit('submit_beta_feedback', 10);

drop trigger if exists trg_rate_limit_session_manifest on public.essentials_manifests;
create trigger trg_rate_limit_session_manifest
before insert on public.essentials_manifests
for each row execute function public.enforce_table_rate_limit('upsert_session_manifest', 10);

revoke all on function public.enforce_table_rate_limit() from public;
grant execute on function public.enforce_table_rate_limit() to authenticated, service_role;

-- ---------------------------------------------------------------------------
-- 2. Admin directory privacy
-- ---------------------------------------------------------------------------

-- The launcher reads the 'email' key; keep the key, mask the value.
create or replace function public.admin_list_users()
returns setof jsonb
language sql
security definer
set search_path = public, auth
as $$
    select jsonb_build_object(
        'id', u.id,
        'email', case
            when u.email is null or u.email = '' then ''
            else left(u.email, 2) || '***@' || split_part(u.email, '@', 2)
        end,
        'username', coalesce(u.raw_user_meta_data->>'username', split_part(coalesce(u.email, ''), '@', 1)),
        'display_name', coalesce(u.raw_user_meta_data->>'display_name', u.raw_user_meta_data->>'full_name', ''),
        'avatar_url', coalesce(u.raw_user_meta_data->>'avatar_url', ''),
        'created_at', extract(epoch from u.created_at)::bigint,
        'updated_at', extract(epoch from u.updated_at)::bigint,
        'last_login_at', extract(epoch from u.last_sign_in_at)::bigint
    )
    from auth.users u
    where public.is_project_staff()
    order by u.created_at desc;
$$;

revoke all on function public.admin_list_users() from public;
grant execute on function public.admin_list_users() to authenticated;

-- Full email address, only via an explicit staff-gated lookup (support/
-- moderation action), never part of bulk enumeration.
create or replace function public.admin_get_user_detail(p_user_id uuid)
returns jsonb
language sql
security definer
set search_path = public, auth
as $$
    select jsonb_build_object(
        'id', u.id,
        'email', u.email,
        'username', coalesce(u.raw_user_meta_data->>'username', split_part(coalesce(u.email, ''), '@', 1)),
        'display_name', coalesce(u.raw_user_meta_data->>'display_name', u.raw_user_meta_data->>'full_name', ''),
        'created_at', extract(epoch from u.created_at)::bigint
    )
    from auth.users u
    where u.id = p_user_id and public.is_project_staff();
$$;

revoke all on function public.admin_get_user_detail(uuid) from public;
grant execute on function public.admin_get_user_detail(uuid) to authenticated;

-- ---------------------------------------------------------------------------
-- 3. TURN accounting (authoritative)
-- ---------------------------------------------------------------------------

-- Backend-owned plan quota configuration. The launcher never decides these.
create table if not exists public.plan_quotas (
    plan text primary key,
    turn_allowance_bytes bigint not null default 0 check (turn_allowance_bytes >= 0),
    updated_at timestamptz not null default now()
);

alter table public.plan_quotas enable row level security;
create policy "anyone reads plan quotas"
on public.plan_quotas for select to authenticated using (true);

-- Seeded defaults; the backend may update these without a client release.
insert into public.plan_quotas(plan, turn_allowance_bytes) values
    ('free', 1073741824),            -- 1 GiB default relay allowance
    ('amalgam_plus', 85899345920)    -- 80 GiB / month (Amalgam+)
on conflict (plan) do nothing;

alter table public.turn_usage
    add column if not exists last_source text not null default '';

-- Append-only ingestion ledger (service-role writes only). event_id makes
-- replay/double-ingestion idempotent.
create table if not exists public.turn_usage_events (
    id bigint generated always as identity primary key,
    user_id uuid not null references auth.users(id) on delete cascade,
    bytes bigint not null check (bytes >= 0),
    source text not null default 'relay',
    event_id text,
    ingested_at timestamptz not null default now()
);

create unique index if not exists turn_usage_events_event_uidx
    on public.turn_usage_events(event_id)
    where event_id is not null;

alter table public.turn_usage_events enable row level security;
-- Deliberately NO authenticated policy: only service_role ingestion writes.

-- Trusted ingestion path (service-role only). The TURN provider/relay
-- metrics pipeline calls this with authoritative byte counts. Ordinary
-- users have no path to write usage: the ledger has no authenticated
-- policy, and this function refuses any caller whose JWT role is not
-- service_role (anon and authenticated users are both rejected).
create or replace function public.ingest_turn_usage(
    p_user_id uuid,
    p_bytes bigint,
    p_source text default 'relay',
    p_event_id text default null
)
returns jsonb
language plpgsql
security definer
set search_path = public, auth
as $$
declare
    caller_role text := coalesce(auth.jwt() ->> 'role', '');
begin
    if caller_role <> 'service_role' then
        raise exception 'not authorized';
    end if;
    if p_bytes < 0 then
        raise exception 'bytes must be non-negative';
    end if;
    if not exists (select 1 from auth.users where id = p_user_id) then
        raise exception 'user not found';
    end if;

    -- Replay protection: the same provider event ingested twice is a no-op
    -- (the unique partial index on event_id is the authoritative guard).
    if p_event_id is not null and exists (
        select 1 from public.turn_usage_events where event_id = p_event_id
    ) then
        return jsonb_build_object('ingested', false, 'duplicate', true);
    end if;

    insert into public.turn_usage_events(user_id, bytes, source, event_id)
    values (p_user_id, p_bytes, coalesce(p_source, 'relay'), p_event_id);

    -- Roll over an expired period before aggregating so bytes never land in
    -- a stale monthly window.
    perform public.rollover_turn_period(p_user_id);

    -- Aggregate into the monthly total. Period rows are created lazily by
    -- enforce_turn_quota; if missing, create one with the default allowance.
    insert into public.turn_usage(
        user_id, used_bytes, monthly_bytes, period_start, period_end, last_source
    )
    values (
        p_user_id, p_bytes, 0, extract(epoch from now())::bigint,
        extract(epoch from now() + interval '30 days')::bigint, p_source
    )
    on conflict (user_id) do update
        set used_bytes = public.turn_usage.used_bytes + p_bytes,
            last_source = excluded.last_source,
            updated_at = now();

    return jsonb_build_object('ingested', true);
end;
$$;

revoke all on function public.ingest_turn_usage(uuid, bigint, text, text) from public, authenticated;
grant execute on function public.ingest_turn_usage(uuid, bigint, text, text) to service_role;

-- Lazy monthly rollover (epoch-second periods as stored in turn_usage).
create or replace function public.rollover_turn_period(p_user_id uuid)
returns void language plpgsql security definer set search_path = public as $$
begin
    update public.turn_usage set
        used_bytes = 0,
        period_start = period_end,
        period_end = period_end + 2592000,  -- 30 days
        updated_at = now()
    where user_id = p_user_id
      and period_end > 0
      and period_end < extract(epoch from now());
end;
$$;

-- Server-authoritative quota evaluation. Called by get-turn-credentials before
-- issuing any relay credential. Raises TURNQUOTA when exhausted; direct P2P is
-- intentionally unaffected (no relay credential is required for it).
create or replace function public.enforce_turn_quota()
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    actor uuid := auth.uid();
    allowance bigint := 0;
    used bigint := 0;
    period_end_ts bigint := 0;
begin
    if actor is null then raise exception 'authentication required'; end if;

    perform public.rollover_turn_period(actor);

    -- Allowance comes from the backend-owned plan table (active subscription
    -- wins; otherwise the FREE default).
    select coalesce(max(pq.turn_allowance_bytes), 0) into allowance
      from public.plan_quotas pq
      join public.subscriptions s
        on lower(s.plan_id) = pq.plan and s.user_id = actor and s.status = 'active';
    if allowance = 0 then
        select pq.turn_allowance_bytes into allowance
          from public.plan_quotas pq where pq.plan = 'free';
    end if;
    if allowance = 0 then
        select monthly_bytes into allowance from public.turn_usage where user_id = actor;
    end if;
    if allowance = 0 then allowance := 1073741824; end if;

    select coalesce(used_bytes, 0), coalesce(period_end, 0)
      into used, period_end_ts
      from public.turn_usage where user_id = actor;

    if period_end_ts = 0 then
        insert into public.turn_usage(user_id, used_bytes, monthly_bytes, period_start, period_end, last_source)
        values (actor, 0, allowance, extract(epoch from now())::bigint,
                extract(epoch from now() + interval '30 days')::bigint, 'quota-init')
        on conflict (user_id) do update set monthly_bytes = excluded.monthly_bytes;
        period_end_ts := extract(epoch from now() + interval '30 days')::bigint;
    end if;

    if used >= allowance then
        raise exception 'TURN relay allowance exhausted'
            using errcode = 'TURNQUOTA',
                  detail = jsonb_build_object(
                      'used_bytes', used, 'allowance', allowance,
                      'period_end', period_end_ts)::text;
    end if;

    return jsonb_build_object(
        'allowed', true,
        'used_bytes', used,
        'allowance', allowance,
        'remaining_bytes', greatest(0, allowance - used),
        'period_end', period_end_ts);
end;
$$;

revoke all on function public.enforce_turn_quota() from public;
grant execute on function public.enforce_turn_quota() to authenticated;

-- Trusted ingestion. Service-role only (backed by relay/provider metrics).
-- Normal users CANNOT call this: client-reported bytes are never trusted.
create or replace function public.record_turn_usage(
    p_user_id uuid, p_bytes bigint, p_source text default 'relay', p_event_id text default null)
returns boolean language plpgsql security definer set search_path = public as $$
begin
    if p_user_id is null or p_bytes is null or p_bytes < 0 then
        raise exception 'invalid usage payload';
    end if;
    if p_bytes = 0 then return true; end if;

    if p_event_id is not null and exists (
        select 1 from public.turn_usage_events where event_id = p_event_id) then
        return true;  -- idempotent: already ingested
    end if;

    perform public.rollover_turn_period(p_user_id);

    insert into public.turn_usage_events(user_id, bytes, source, event_id)
    values (p_user_id, p_bytes, coalesce(p_source, 'relay'), p_event_id);

    insert into public.turn_usage(user_id, used_bytes, monthly_bytes, period_start, period_end, last_source)
    values (p_user_id, p_bytes, 0, extract(epoch from now())::bigint,
            extract(epoch from now() + interval '30 days')::bigint, coalesce(p_source, 'relay'))
    on conflict (user_id) do update
      set used_bytes = public.turn_usage.used_bytes + excluded.used_bytes,
          last_source = excluded.last_source,
          updated_at = now();
    return true;
end;
$$;

revoke all on function public.record_turn_usage(uuid,bigint,text,text) from public;
revoke all on function public.record_turn_usage(uuid,bigint,text) from public;
grant execute on function public.record_turn_usage(uuid,bigint,text,text) to service_role;
grant execute on function public.record_turn_usage(uuid,bigint,text) to service_role;

revoke all on function public.rollover_turn_period(uuid) from public;
grant execute on function public.rollover_turn_period(uuid) to service_role;
