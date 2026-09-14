-- Free Supabase control plane for Amalgam Essentials.
-- No Minecraft packets or world traffic pass through these tables/functions.

create extension if not exists pgcrypto;

create table if not exists public.essentials_sessions (
    id text primary key,
    host_user_id uuid not null references auth.users(id) on delete cascade,
    world_name text not null,
    privacy integer not null default 0,
    player_limit integer not null default 8 check (player_limit between 1 and 64),
    state integer not null default 0,
    join_token_hash text not null,
    minecraft_version text not null default '',
    loader text not null default '',
    loader_version text not null default '',
    created_at timestamptz not null default now(),
    expires_at timestamptz not null default (now() + interval '24 hours'),
    updated_at timestamptz not null default now()
);

create table if not exists public.essentials_session_members (
    session_id text not null references public.essentials_sessions(id) on delete cascade,
    user_id uuid not null references auth.users(id) on delete cascade,
    role text not null default 'player' check (role in ('host', 'player')),
    joined_at timestamptz not null default now(),
    left_at timestamptz,
    primary key (session_id, user_id)
);

create table if not exists public.essentials_session_bans (
    session_id text not null references public.essentials_sessions(id) on delete cascade,
    user_id uuid not null references auth.users(id) on delete cascade,
    reason text not null default '',
    banned_by uuid not null references auth.users(id),
    banned_at timestamptz not null default now(),
    primary key (session_id, user_id)
);

create table if not exists public.essentials_invites (
    id text primary key,
    session_id text not null references public.essentials_sessions(id) on delete cascade,
    host_user_id uuid not null references auth.users(id) on delete cascade,
    target_user_id uuid not null references auth.users(id) on delete cascade,
    status text not null default 'pending' check (status in ('pending','accepted','declined','cancelled','expired')),
    world_name text not null default '',
    minecraft_version text not null default '',
    loader text not null default '',
    loader_version text not null default '',
    mod_count integer not null default 0,
    created_at timestamptz not null default now(),
    expires_at timestamptz not null default (now() + interval '24 hours'),
    updated_at timestamptz not null default now()
);

create table if not exists public.essentials_manifests (
    session_id text primary key references public.essentials_sessions(id) on delete cascade,
    host_user_id uuid not null references auth.users(id) on delete cascade,
    profile_id text not null default '',
    profile_name text not null default '',
    minecraft_version text not null default '',
    loader text not null default '',
    loader_version text not null default '',
    required_resource_packs jsonb not null default '[]'::jsonb,
    required_configs jsonb not null default '{}'::jsonb,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

create table if not exists public.essentials_manifest_mods (
    session_id text not null references public.essentials_manifests(session_id) on delete cascade,
    name text not null,
    mod_id text not null default '',
    version text not null default '',
    hash text not null default '',
    source text not null default '',
    enabled boolean not null default true,
    primary key (session_id, name)
);

alter table public.essentials_sessions enable row level security;
alter table public.essentials_session_members enable row level security;
alter table public.essentials_session_bans enable row level security;
alter table public.essentials_invites enable row level security;
alter table public.essentials_manifests enable row level security;
alter table public.essentials_manifest_mods enable row level security;

-- State transitions are intentionally performed by security-definer RPCs.
create or replace function public.create_session(
    session_id text, host_user_id uuid, world_name text, privacy integer,
    player_limit integer, join_token text default '', minecraft_version text default '',
    loader text default '', loader_version text default '')
returns jsonb language plpgsql security definer set search_path = public as $$
declare result jsonb;
begin
    if auth.uid() is null or auth.uid() <> host_user_id then raise exception 'authenticated host required'; end if;
    insert into essentials_sessions(id, host_user_id, world_name, privacy, player_limit, state,
        join_token_hash, minecraft_version, loader, loader_version)
    values (session_id, host_user_id, world_name, privacy, player_limit, 0,
        encode(digest(join_token, 'sha256'), 'hex'), minecraft_version, loader, loader_version)
    on conflict (id) do update set world_name = excluded.world_name,
        privacy = excluded.privacy, player_limit = excluded.player_limit,
        updated_at = now();
    insert into essentials_session_members(session_id, user_id, role)
    values (session_id, host_user_id, 'host') on conflict do nothing;
    select jsonb_build_object('id', id, 'state', state, 'created_at', created_at)
      into result from essentials_sessions where id = session_id;
    return result;
end $$;

create or replace function public.start_session(session_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set state = 2, updated_at = now()
    where id = session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    return jsonb_build_object('success', true, 'session_id', session_id, 'state', 2);
end $$;

create or replace function public.stop_session(session_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set state = 4, updated_at = now()
    where id = session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    update essentials_session_members set left_at = now()
    where essentials_session_members.session_id = stop_session.session_id and left_at is null;
    return jsonb_build_object('success', true, 'session_id', session_id, 'state', 4);
end $$;

create or replace function public.join_session(session_id text, join_token text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare s essentials_sessions;
begin
    select * into s from essentials_sessions where id = session_id and expires_at > now();
    if s.id is null then raise exception 'session not found or expired'; end if;
    if s.state not in (0, 1, 2) then raise exception 'session is not online'; end if;
    if s.join_token_hash <> encode(digest(join_token, 'sha256'), 'hex') then raise exception 'invalid join token'; end if;
    if exists (select 1 from essentials_session_bans b where b.session_id = s.id and b.user_id = auth.uid()) then raise exception 'user is banned'; end if;
    if s.privacy = 3 and not exists (select 1 from essentials_session_members m where m.session_id = s.id and m.user_id = auth.uid()) then
        raise exception 'private session';
    end if;
    insert into essentials_session_members(session_id, user_id, role)
    values (s.id, auth.uid(), 'player') on conflict (session_id, user_id)
    do update set left_at = null;
    return jsonb_build_object('success', true, 'session_id', s.id, 'host_user_id', s.host_user_id);
end $$;

create or replace function public.leave_session(session_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_session_members set left_at = now()
    where essentials_session_members.session_id = leave_session.session_id and user_id = auth.uid();
    return jsonb_build_object('success', true, 'session_id', session_id);
end $$;

create or replace function public.update_session(session_id text, world_name text, privacy integer,
    player_limit integer, state integer)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set world_name = update_session.world_name,
        privacy = update_session.privacy, player_limit = update_session.player_limit,
        state = update_session.state, updated_at = now()
    where id = update_session.session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    return jsonb_build_object('success', true, 'session_id', session_id);
end $$;

create or replace function public.upsert_session_manifest(session_id text, profile_id text default '',
    profile_name text default '', minecraft_version text default '', loader text default '', loader_version text default '',
    required_resource_packs jsonb default '[]'::jsonb, required_configs jsonb default '{}'::jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (select 1 from essentials_sessions where id = session_id and host_user_id = auth.uid()) then
        raise exception 'host session not found';
    end if;
    insert into essentials_manifests(session_id, host_user_id, profile_id, profile_name,
        minecraft_version, loader, loader_version, required_resource_packs, required_configs)
    values (session_id, auth.uid(), profile_id, profile_name, minecraft_version, loader,
        loader_version, required_resource_packs, required_configs)
    on conflict (session_id) do update set profile_id = excluded.profile_id,
        profile_name = excluded.profile_name, minecraft_version = excluded.minecraft_version,
        loader = excluded.loader, loader_version = excluded.loader_version,
        required_resource_packs = excluded.required_resource_packs,
        required_configs = excluded.required_configs, updated_at = now();
    return jsonb_build_object('success', true, 'session_id', session_id);
end $$;

create or replace function public.get_session_manifest(session_id text, join_token text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare s essentials_sessions; m essentials_manifests; result jsonb;
begin
    select * into s from essentials_sessions where id = session_id and expires_at > now();
    if s.id is null or s.join_token_hash <> encode(digest(join_token, 'sha256'), 'hex') then raise exception 'invalid session credentials'; end if;
    select * into m from essentials_manifests where essentials_manifests.session_id = get_session_manifest.session_id;
    if m.session_id is null then raise exception 'manifest not found'; end if;
    select jsonb_build_object('session_id', m.session_id, 'host_user_id', m.host_user_id,
        'profile_id', m.profile_id, 'profile_name', m.profile_name,
        'minecraft_version', m.minecraft_version, 'loader', m.loader,
        'loader_version', m.loader_version, 'required_resource_packs', m.required_resource_packs,
        'required_configs', m.required_configs, 'mods', coalesce((select jsonb_agg(to_jsonb(mm))
            from essentials_manifest_mods mm where mm.session_id = m.session_id), '[]'::jsonb)) into result;
    return result;
end $$;
