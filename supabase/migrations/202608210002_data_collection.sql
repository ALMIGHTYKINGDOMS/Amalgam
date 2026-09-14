-- Data collection infrastructure for Amalgam.
-- Captures usage events, performance metrics, errors, and audit trail
-- across the launcher, runtime agent, and web surfaces.

-- ---------------------------------------------------------------------------
-- Events: generic structured event log (append-only)
-- ---------------------------------------------------------------------------
create table if not exists public.events (
    id bigint generated always as identity primary key,
    received_at timestamptz not null default now(),

    -- Source identification
    source text not null default 'launcher' check (source in (
        'launcher', 'runtime', 'web', 'api', 'edge', 'mobile'
    )),
    source_version text not null default '',
    source_node_id uuid,  -- runtime node that emitted the event

    -- User context (nullable for anonymous/unauthenticated events)
    user_id uuid references auth.users(id) on delete set null,
    session_id text,  -- launcher session or browser session

    -- Event classification
    category text not null default 'generic' check (category in (
        'generic', 'auth', 'server', 'session', 'social', 'mod',
        'profile', 'billing', 'performance', 'error', 'security',
        'install', 'launch', 'ai', 'feedback', 'admin'
    )),
    event_name text not null default '',

    -- Payload (flexible JSONB for different event types)
    properties jsonb not null default '{}'::jsonb,

    -- Numeric metrics (for aggregation without JSON parsing)
    duration_ms bigint,
    value numeric,

    -- Context
    page text not null default '',
    screen text not null default '',
    ip_address inet,
    user_agent text not null default ''
);

alter table public.events enable row level security;

-- Users can insert their own events and read their own events
create policy "users insert own events"
on public.events for insert to authenticated
with check (user_id = auth.uid() or user_id is null);

create policy "users read own events"
on public.events for select to authenticated
using (user_id = auth.uid());

-- Staff can read all events
create policy "staff read all events"
on public.events for select to authenticated
using (public.is_project_staff());

-- Runtime nodes can insert events (with node_id context)
create policy "runtime nodes insert events"
on public.events for insert to authenticated
with check (source = 'runtime' and source_node_id is not null);

-- Indexes for common queries
create index if not exists events_user_idx on public.events(user_id, received_at desc);
create index if not exists events_category_idx on public.events(category, received_at desc);
create index if not exists events_source_idx on public.events(source, received_at desc);
create index if not exists events_name_idx on public.events(event_name, received_at desc);
create index if not exists events_received_idx on public.events(received_at desc);

-- ---------------------------------------------------------------------------
-- Metrics: numeric time-series data (CPU, memory, TPS, player counts, etc.)
-- ---------------------------------------------------------------------------
create table if not exists public.metrics (
    id bigint generated always as identity primary key,
    recorded_at timestamptz not null default now(),

    source text not null default 'launcher' check (source in (
        'launcher', 'runtime', 'web', 'api'
    )),
    source_node_id uuid,
    user_id uuid references auth.users(id) on delete set null,

    -- Metric identification
    metric_name text not null default '',
    metric_type text not null default 'gauge' check (metric_type in (
        'gauge', 'counter', 'histogram', 'summary'
    )),

    -- Value
    value numeric not null default 0,
    unit text not null default '',

    -- Tags for dimensional analysis
    tags jsonb not null default '{}'::jsonb
);

alter table public.metrics enable row level security;

create policy "users read own metrics"
on public.metrics for select to authenticated
using (user_id = auth.uid());

create policy "staff read all metrics"
on public.metrics for select to authenticated
using (public.is_project_staff());

create policy "runtime nodes insert metrics"
on public.metrics for insert to authenticated
with check (source = 'runtime' and source_node_id is not null);

create policy "launcher insert metrics"
on public.metrics for insert to authenticated
with check (source = 'launcher');

create index if not exists metrics_name_idx on public.metrics(metric_name, recorded_at desc);
create index if not exists metrics_user_idx on public.metrics(user_id, metric_name, recorded_at desc);
create index if not exists metrics_recorded_idx on public.metrics(recorded_at desc);

-- ---------------------------------------------------------------------------
-- Errors: structured error tracking with stack traces
-- ---------------------------------------------------------------------------
create table if not exists public.errors (
    id bigint generated always as identity primary key,
    occurred_at timestamptz not null default now(),

    source text not null default 'launcher' check (source in (
        'launcher', 'runtime', 'web', 'api', 'edge'
    )),
    source_version text not null default '',
    source_node_id uuid,
    user_id uuid references auth.users(id) on delete set null,

    -- Error classification
    error_type text not null default '',  -- e.g., 'network', 'auth', 'server', 'ui'
    error_code text not null default '',  -- e.g., 'E_TIMEOUT', 'E_AUTH_EXPIRED'
    severity text not null default 'error' check (severity in (
        'debug', 'info', 'warning', 'error', 'fatal'
    )),

    -- Error details
    message text not null default '',
    stack_trace text not null default '',
    component text not null default '',  -- e.g., 'SupabaseClient', 'ServerManager'

    -- Context
    operation text not null default '',  -- what was being attempted
    properties jsonb not null default '{}'::jsonb,

    -- Deduplication
    fingerprint text not null default '',  -- hash of type+message+component for grouping

    -- Resolution tracking
    resolved boolean not null default false,
    resolved_at timestamptz,
    resolved_by uuid references auth.users(id)
);

alter table public.errors enable row level security;

create policy "users insert own errors"
on public.errors for insert to authenticated
with check (user_id = auth.uid() or user_id is null);

create policy "users read own errors"
on public.errors for select to authenticated
using (user_id = auth.uid());

create policy "staff read all errors"
on public.errors for select to authenticated
using (public.is_project_staff());

create policy "runtime nodes insert errors"
on public.errors for insert to authenticated
with check (source = 'runtime' and source_node_id is not null);

create index if not exists errors_fingerprint_idx on public.errors(fingerprint, occurred_at desc);
create index if not exists errors_type_idx on public.errors(error_type, occurred_at desc);
create index if not exists errors_severity_idx on public.errors(severity, occurred_at desc);
create index if not exists errors_user_idx on public.errors(user_id, occurred_at desc);
create index if not exists errors_unresolved_idx on public.errors(resolved, occurred_at desc)
    where resolved = false;

-- ---------------------------------------------------------------------------
-- Audit log: who did what to which resource (immutable)
-- ---------------------------------------------------------------------------
create table if not exists public.audit_log (
    id bigint generated always as identity primary key,
    performed_at timestamptz not null default now(),

    actor_id uuid references auth.users(id) on delete set null,
    actor_email text not null default '',
    actor_role text not null default '',

    -- What happened
    action text not null default '',  -- 'create', 'update', 'delete', 'login', 'logout', etc.
    resource_type text not null default '',  -- 'server', 'session', 'project', 'user', etc.
    resource_id text not null default '',

    -- Details
    changes jsonb not null default '{}'::jsonb,  -- {field: {old: x, new: y}}
    metadata jsonb not null default '{}'::jsonb,

    -- Context
    source text not null default 'launcher',
    ip_address inet,
    user_agent text not null default ''
);

alter table public.audit_log enable row level security;

-- Only staff can read audit logs
create policy "staff read audit log"
on public.audit_log for select to authenticated
using (public.is_project_staff());

-- Audit log is insert-only (no update/delete policies = immutable)
create policy "authenticated insert audit"
on public.audit_log for insert to authenticated
with check (true);

create index if not exists audit_actor_idx on public.audit_log(actor_id, performed_at desc);
create index if not exists audit_resource_idx on public.audit_log(resource_type, resource_id, performed_at desc);
create index if not exists audit_action_idx on public.audit_log(action, performed_at desc);

-- ---------------------------------------------------------------------------
-- Feature usage: track which features are used and how often
-- ---------------------------------------------------------------------------
create table if not exists public.feature_usage (
    id bigint generated always as identity primary key,
    recorded_at timestamptz not null default now(),

    user_id uuid references auth.users(id) on delete set null,
    source text not null default 'launcher',

    feature_name text not null default '',
    feature_category text not null default '',

    -- Usage metrics
    usage_count integer not null default 1,
    duration_ms bigint,
    success boolean not null default true,

    -- Context
    properties jsonb not null default '{}'::jsonb
);

alter table public.feature_usage enable row level security;

create policy "users insert own feature usage"
on public.feature_usage for insert to authenticated
with check (user_id = auth.uid() or user_id is null);

create policy "staff read all feature usage"
on public.feature_usage for select to authenticated
using (public.is_project_staff());

create index if not exists feature_usage_name_idx on public.feature_usage(feature_name, recorded_at desc);
create index if not exists feature_usage_user_idx on public.feature_usage(user_id, feature_name, recorded_at desc);

-- ---------------------------------------------------------------------------
-- Batch event insertion RPC (single call for multiple events)
-- ---------------------------------------------------------------------------
create or replace function public.insert_events(p_events jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    inserted_count integer := 0;
    item jsonb;
begin
    for item in select jsonb_array_elements(p_events)
    loop
        insert into public.events (
            source, source_version, source_node_id, user_id, session_id,
            category, event_name, properties, duration_ms, value,
            page, screen, user_agent
        ) values (
            coalesce(item->>'source', 'launcher'),
            coalesce(item->>'source_version', ''),
            (item->>'source_node_id')::uuid,
            coalesce((item->>'user_id')::uuid, auth.uid()),
            coalesce(item->>'session_id', ''),
            coalesce(item->>'category', 'generic'),
            coalesce(item->>'event_name', ''),
            coalesce(item->'properties', '{}'::jsonb),
            (item->>'duration_ms')::bigint,
            (item->>'value')::numeric,
            coalesce(item->>'page', ''),
            coalesce(item->>'screen', ''),
            coalesce(item->>'user_agent', '')
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

-- ---------------------------------------------------------------------------
-- Batch metrics insertion RPC
-- ---------------------------------------------------------------------------
create or replace function public.insert_metrics(p_metrics jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    inserted_count integer := 0;
    item jsonb;
begin
    for item in select jsonb_array_elements(p_metrics)
    loop
        insert into public.metrics (
            source, source_node_id, user_id,
            metric_name, metric_type, value, unit, tags
        ) values (
            coalesce(item->>'source', 'launcher'),
            (item->>'source_node_id')::uuid,
            coalesce((item->>'user_id')::uuid, auth.uid()),
            coalesce(item->>'metric_name', ''),
            coalesce(item->>'metric_type', 'gauge'),
            coalesce((item->>'value')::numeric, 0),
            coalesce(item->>'unit', ''),
            coalesce(item->'tags', '{}'::jsonb)
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

-- ---------------------------------------------------------------------------
-- Batch error insertion RPC
-- ---------------------------------------------------------------------------
create or replace function public.insert_errors(p_errors jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    inserted_count integer := 0;
    item jsonb;
    fp text;
begin
    for item in select jsonb_array_elements(p_errors)
    loop
        -- Generate fingerprint for deduplication
        fp := encode(
            digest(
                coalesce(item->>'error_type', '') || ':' ||
                coalesce(item->>'message', '') || ':' ||
                coalesce(item->>'component', ''),
                'sha256'
            ), 'hex'
        );

        insert into public.errors (
            source, source_version, source_node_id, user_id,
            error_type, error_code, severity,
            message, stack_trace, component,
            operation, properties, fingerprint
        ) values (
            coalesce(item->>'source', 'launcher'),
            coalesce(item->>'source_version', ''),
            (item->>'source_node_id')::uuid,
            coalesce((item->>'user_id')::uuid, auth.uid()),
            coalesce(item->>'error_type', ''),
            coalesce(item->>'error_code', ''),
            coalesce(item->>'severity', 'error'),
            coalesce(item->>'message', ''),
            coalesce(item->>'stack_trace', ''),
            coalesce(item->>'component', ''),
            coalesce(item->>'operation', ''),
            coalesce(item->'properties', '{}'::jsonb),
            fp
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

-- ---------------------------------------------------------------------------
-- Batch feature usage insertion RPC
-- ---------------------------------------------------------------------------
create or replace function public.insert_feature_usage(p_usage jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    inserted_count integer := 0;
    item jsonb;
begin
    for item in select jsonb_array_elements(p_usage)
    loop
        insert into public.feature_usage (
            user_id, source,
            feature_name, feature_category,
            usage_count, duration_ms, success, properties
        ) values (
            coalesce((item->>'user_id')::uuid, auth.uid()),
            coalesce(item->>'source', 'launcher'),
            coalesce(item->>'feature_name', ''),
            coalesce(item->>'feature_category', ''),
            coalesce((item->>'usage_count')::integer, 1),
            (item->>'duration_ms')::bigint,
            coalesce((item->>'success')::boolean, true),
            coalesce(item->'properties', '{}'::jsonb)
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

-- ---------------------------------------------------------------------------
-- Record audit log entry RPC
-- ---------------------------------------------------------------------------
create or replace function public.record_audit(
    p_action text,
    p_resource_type text,
    p_resource_id text default '',
    p_changes jsonb default '{}'::jsonb,
    p_metadata jsonb default '{}'::jsonb,
    p_source text default 'launcher'
)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    insert into public.audit_log (
        actor_id, actor_email, actor_role,
        action, resource_type, resource_id,
        changes, metadata, source
    ) values (
        auth.uid(),
        coalesce((select email from auth.users where id = auth.uid()), ''),
        case when public.is_project_staff() then 'staff' else 'user' end,
        p_action,
        p_resource_type,
        p_resource_id,
        p_changes,
        p_metadata,
        p_source
    );

    return jsonb_build_object('success', true);
end;
$$;

-- ---------------------------------------------------------------------------
-- Revoke and grant batch RPCs
-- ---------------------------------------------------------------------------
revoke all on function public.insert_events(jsonb) from public;
revoke all on function public.insert_metrics(jsonb) from public;
revoke all on function public.insert_errors(jsonb) from public;
revoke all on function public.insert_feature_usage(jsonb) from public;
revoke all on function public.record_audit(text,text,text,jsonb,jsonb,text) from public;

grant execute on function public.insert_events(jsonb) to authenticated;
grant execute on function public.insert_metrics(jsonb) to authenticated;
grant execute on function public.insert_errors(jsonb) to authenticated;
grant execute on function public.insert_feature_usage(jsonb) to authenticated;
grant execute on function public.record_audit(text,text,text,jsonb,jsonb,text) to authenticated;

-- ---------------------------------------------------------------------------
-- Materialized view: hourly event aggregates (for dashboards)
-- ---------------------------------------------------------------------------
create materialized view if not exists public.event_hourly_stats as
select
    date_trunc('hour', received_at) as hour,
    source,
    category,
    event_name,
    count(*) as event_count,
    count(distinct user_id) as unique_users,
    avg(duration_ms) as avg_duration_ms,
    avg(value) as avg_value
from public.events
group by 1, 2, 3, 4;

create unique index if not exists event_hourly_stats_idx
    on public.event_hourly_stats(hour, source, category, event_name);

-- ---------------------------------------------------------------------------
-- Materialized view: error summaries (for dashboards)
-- ---------------------------------------------------------------------------
create materialized view if not exists public.error_daily_stats as
select
    date_trunc('day', occurred_at) as day,
    source,
    error_type,
    severity,
    fingerprint,
    message,
    count(*) as occurrence_count,
    count(distinct user_id) as affected_users,
    min(occurred_at) as first_seen,
    max(occurred_at) as last_seen,
    resolved
from public.errors
group by 1, 2, 3, 4, 5, 6, 12;

create unique index if not exists error_daily_stats_idx
    on public.error_daily_stats(day, source, error_type, fingerprint);
