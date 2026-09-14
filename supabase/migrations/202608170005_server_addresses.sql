-- Dedicated servers use the .amalgam namespace. Essentials worlds use
-- .amalgam-essentials and remain separate.
create table if not exists public.servers (
    id text primary key,
    user_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    host text not null default '',
    port integer not null default 25565 check (port between 1 and 65535),
    version text not null default '',
    type text not null default 'vanilla',
    motd text not null default '',
    max_players integer not null default 20 check (max_players between 1 and 1000),
    online boolean not null default false,
    ping_ms integer not null default 0,
    alias text not null default '',
    address text not null default '',
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.servers enable row level security;
drop policy if exists "server owners manage servers" on public.servers;
create policy "server owners manage servers" on public.servers
for all to authenticated using (user_id = auth.uid()) with check (user_id = auth.uid());

update public.essentials_sessions
set address = lower(alias) || '.amalgam-essentials'
where alias <> '';

alter table public.servers
    add column if not exists alias text not null default '',
    add column if not exists address text not null default '';

update public.servers
set alias = case when alias = '' then
        left(regexp_replace(lower(name), '[^a-z0-9]+', '-', 'g'), 24)
        else lower(alias) end,
    address = case when address = '' then
        left(regexp_replace(lower(name), '[^a-z0-9]+', '-', 'g'), 24) || '.amalgam'
        else lower(address) end
where alias = '' or address = '';

create unique index if not exists servers_active_alias_idx
    on public.servers(lower(alias)) where online = true;
create unique index if not exists servers_active_address_idx
    on public.servers(lower(address)) where online = true;
create index if not exists servers_address_lookup_idx
    on public.servers(lower(address), online);

create or replace function public.is_essentials_alias_available(session_alias text)
returns boolean language sql security definer set search_path = public as $$
    select auth.uid() is not null
       and public.essentials_alias_valid(lower(session_alias))
       and not exists (
           select 1 from public.essentials_sessions
           where lower(alias) = lower(session_alias)
             and status in ('starting', 'online')
       )
       and not exists (
           select 1 from public.servers
           where lower(alias) = lower(session_alias) and online = true
       )
$$;

create or replace function public.is_server_alias_available(server_alias text)
returns boolean language sql security definer set search_path = public as $$
    select auth.uid() is not null
       and public.essentials_alias_valid(lower(server_alias))
       and not exists (
           select 1 from public.servers
           where lower(alias) = lower(server_alias) and online = true
       )
       and not exists (
           select 1 from public.essentials_sessions
           where lower(alias) = lower(server_alias)
             and status in ('starting', 'online')
       )
$$;

create or replace function public.resolve_essentials_address(session_address text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare result jsonb;
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    select jsonb_build_object(
        'kind', 'essentials', 'session_id', id, 'host_user_id', host_user_id,
        'alias', alias, 'address', address, 'status', status,
        'created_at', created_at, 'expires_at', expires_at,
        'world_name', world_name, 'privacy', privacy,
        'player_limit', player_limit, 'player_count',
            (select count(*) from public.essentials_session_members m
             where m.session_id = essentials_sessions.id and m.left_at is null)
    ) into result
    from public.essentials_sessions
    where lower(address) = lower(session_address)
      and lower(address) like '%.amalgam-essentials'
      and status = 'online' and expires_at > now()
    limit 1;
    if result is not null then return result; end if;

    select jsonb_build_object(
        'kind', 'server', 'server_id', id, 'host_user_id', user_id,
        'alias', alias, 'address', address,
        'status', case when online then 'online' else 'offline' end,
        'created_at', created_at, 'updated_at', updated_at,
        'world_name', name, 'privacy', 0,
        'player_limit', max_players, 'player_count', 0
    ) into result
    from public.servers
    where lower(address) = lower(session_address)
      and lower(address) like '%.amalgam'
      and online = true
    limit 1;
    if result is null then raise exception 'server is offline or address was not found'; end if;
    return result;
end $$;

create or replace function public.create_session(
    session_id text, host_user_id uuid, world_name text, privacy integer,
    player_limit integer, join_token text default '', minecraft_version text default '',
    loader text default '', loader_version text default '', session_alias text default '',
    session_address text default '')
returns jsonb language plpgsql security definer set search_path = public as $$
declare normalized_alias text := lower(trim(session_alias));
begin
    if auth.uid() is null or auth.uid() <> host_user_id then raise exception 'authenticated host required'; end if;
    if not public.essentials_alias_valid(normalized_alias) then raise exception 'invalid or reserved Essentials address'; end if;
    if lower(session_address) <> (normalized_alias || '.amalgam-essentials') then
        raise exception 'invalid Essentials address';
    end if;
    insert into essentials_sessions(id, host_user_id, world_name, privacy, player_limit, state,
        join_token_hash, minecraft_version, loader, loader_version, alias, address, status)
    values (session_id, host_user_id, world_name, privacy, player_limit, 0,
        encode(digest(join_token, 'sha256'), 'hex'), minecraft_version, loader, loader_version,
        normalized_alias, normalized_alias || '.amalgam-essentials', 'starting')
    on conflict (id) do update set world_name = excluded.world_name,
        privacy = excluded.privacy, player_limit = excluded.player_limit,
        alias = excluded.alias, address = excluded.address, status = 'starting',
        updated_at = now();
    insert into essentials_session_members(session_id, user_id, role)
    values (session_id, host_user_id, 'host') on conflict do nothing;
    return jsonb_build_object('id', session_id, 'alias', normalized_alias,
        'address', normalized_alias || '.amalgam-essentials', 'status', 'starting');
end $$;
