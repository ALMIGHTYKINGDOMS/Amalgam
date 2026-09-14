-- Launcher-facing tables: profiles, bedrock_profiles, subscriptions, turn_usage.
-- These are read/written by the C++ launcher via PostgREST.

-- ---------------------------------------------------------------------------
-- Java Edition profiles
-- ---------------------------------------------------------------------------
create table if not exists public.profiles (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    minecraft_version text not null default '',
    loader text not null default 'vanilla',
    loader_version text not null default '',
    java_path text not null default '',
    memory_mb integer not null default 2048,
    mods text[] not null default '{}',
    resource_packs text[] not null default '{}',
    data_packs text[] not null default '{}',
    settings jsonb not null default '{}'::jsonb,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    last_played_at timestamptz
);

alter table public.profiles enable row level security;
create policy "owners manage profiles"
on public.profiles for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

create index if not exists profiles_user_idx on public.profiles(user_id, updated_at desc);

-- ---------------------------------------------------------------------------
-- Bedrock Edition profiles
-- ---------------------------------------------------------------------------
create table if not exists public.bedrock_profiles (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    minecraft_version text not null default '',
    banner_path text not null default '',
    icon_path text not null default '',
    created text not null default '',
    last_played text not null default '',
    last_played_ts bigint not null default 0,
    favorite boolean not null default false,
    "group" text not null default '',
    packs jsonb not null default '[]'::jsonb,
    worlds jsonb not null default '[]'::jsonb,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.bedrock_profiles enable row level security;
create policy "owners manage bedrock profiles"
on public.bedrock_profiles for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

create index if not exists bedrock_profiles_user_idx on public.bedrock_profiles(user_id, last_played_ts desc);

-- ---------------------------------------------------------------------------
-- Subscriptions (synced from Whop by backend)
-- ---------------------------------------------------------------------------
create table if not exists public.subscriptions (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    plan_id text not null default 'free',
    plan_label text not null default 'FREE',
    status text not null default 'active' check (status in (
        'active', 'cancelled', 'past_due', 'trialing', 'incomplete', 'expired'
    )),
    amount numeric not null default 0,
    currency text not null default 'USD',
    current_period_start bigint not null default 0,
    current_period_end bigint not null default 0,
    created_at timestamptz not null default now(),
    cancelled_at bigint not null default 0,
    provider text not null default 'whop',
    external_id text not null default '',
    updated_at timestamptz not null default now()
);

alter table public.subscriptions enable row level security;
create policy "owners read own subscriptions"
on public.subscriptions for select to authenticated
using (user_id = auth.uid());

-- Only service role / backend can write subscriptions
create policy "backend manages subscriptions"
on public.subscriptions for all to authenticated
using (true) with check (true);

create index if not exists subscriptions_user_idx on public.subscriptions(user_id, status);
create index if not exists subscriptions_external_idx on public.subscriptions(external_id);

-- ---------------------------------------------------------------------------
-- TURN usage tracking
-- ---------------------------------------------------------------------------
create table if not exists public.turn_usage (
    user_id uuid primary key references auth.users(id) on delete cascade,
    used_bytes bigint not null default 0,
    monthly_bytes bigint not null default 1073741824,  -- 1 GB default
    period_start bigint not null default 0,
    period_end bigint not null default 0,
    updated_at timestamptz not null default now()
);

alter table public.turn_usage enable row level security;
create policy "owners read own turn usage"
on public.turn_usage for select to authenticated
using (user_id = auth.uid());

-- ---------------------------------------------------------------------------
-- Extend 'servers' with missing columns the launcher writes
-- ---------------------------------------------------------------------------
alter table public.servers
    add column if not exists current_players integer not null default 0,
    add column if not exists last_ping bigint not null default 0,
    add column if not exists last_started bigint not null default 0,
    add column if not exists assigned_at bigint not null default 0;

-- ---------------------------------------------------------------------------
-- Launcher-side node tracking (simpler than hosting_nodes)
-- The launcher uses this for its node management UI.
-- ---------------------------------------------------------------------------
create table if not exists public.nodes (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    host text not null default '',
    port integer not null default 22,
    region text not null default 'us-east-1',
    zone text not null default '',
    online boolean not null default false,

    -- Hardware specs
    cpu_cores integer not null default 1,
    memory_total bigint not null default 1024,
    memory_used bigint not null default 0,
    storage_capacity bigint not null default 50,
    storage_used bigint not null default 0,
    storage_available bigint not null default 50,

    -- Heartbeat
    last_heartbeat bigint not null default 0,
    created_at bigint not null default 0,
    updated_at bigint not null default 0,

    -- Tags and metadata
    tags text[] not null default '{}',
    metadata jsonb not null default '{}'::jsonb
);

alter table public.nodes enable row level security;
create policy "owners manage nodes"
on public.nodes for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

create index if not exists nodes_user_idx on public.nodes(user_id);
