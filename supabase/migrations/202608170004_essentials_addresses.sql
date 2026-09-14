-- Friendly Amalgam-only addresses. These are identifiers, not credentials.
alter table public.essentials_sessions
    add column if not exists alias text not null default '',
    add column if not exists address text not null default '',
    add column if not exists status text not null default 'offline';

update public.essentials_sessions
set alias = case when alias = '' then 'world-' || left(id, 8) else lower(alias) end,
    address = case when address = '' then
        (case when alias = '' then 'world-' || left(id, 8) else lower(alias) end) || '.amalgam'
        else lower(address) end,
    status = case when state in (1, 2) then 'online' else 'offline' end
where alias = '' or address = '' or status = 'offline';

alter table public.essentials_sessions
    drop constraint if exists essentials_sessions_status_check;
alter table public.essentials_sessions
    add constraint essentials_sessions_status_check
    check (status in ('starting', 'online', 'offline', 'expired'));

create unique index if not exists essentials_sessions_active_alias_idx
    on public.essentials_sessions(lower(alias))
    where status in ('starting', 'online');
create unique index if not exists essentials_sessions_active_address_idx
    on public.essentials_sessions(lower(address))
    where status in ('starting', 'online');
create index if not exists essentials_sessions_address_lookup_idx
    on public.essentials_sessions(lower(address), status);

create or replace function public.essentials_alias_valid(session_alias text)
returns boolean language sql immutable as $$
    select session_alias ~ '^[a-z0-9][a-z0-9-]{2,31}$'
       and session_alias not in ('admin', 'support', 'official', 'api', 'login', 'server', 'amalgam')
$$;

create or replace function public.is_essentials_alias_available(session_alias text)
returns boolean language sql security definer set search_path = public as $$
    select auth.uid() is not null
       and public.essentials_alias_valid(lower(session_alias))
       and not exists (
           select 1 from public.essentials_sessions
           where lower(alias) = lower(session_alias)
             and status in ('starting', 'online')
       )
$$;

create or replace function public.resolve_essentials_address(session_address text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare result jsonb;
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    select jsonb_build_object(
        'session_id', id, 'host_user_id', host_user_id,
        'alias', alias, 'address', address, 'status', status,
        'created_at', created_at, 'expires_at', expires_at,
        'world_name', world_name, 'privacy', privacy,
        'player_limit', player_limit, 'player_count',
            (select count(*) from public.essentials_session_members m
             where m.session_id = essentials_sessions.id and m.left_at is null)
    ) into result
    from public.essentials_sessions
    where lower(address) = lower(session_address)
      and status = 'online' and expires_at > now()
    limit 1;
    if result is null then raise exception 'world is offline or address was not found'; end if;
    return result;
end $$;

drop function if exists public.create_session(text, uuid, text, integer, integer, text, text, text, text);
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
    if lower(session_address) <> (normalized_alias || '.amalgam') then
        raise exception 'invalid Essentials address';
    end if;
    insert into essentials_sessions(id, host_user_id, world_name, privacy, player_limit, state,
        join_token_hash, minecraft_version, loader, loader_version, alias, address, status)
    values (session_id, host_user_id, world_name, privacy, player_limit, 0,
        encode(digest(join_token, 'sha256'), 'hex'), minecraft_version, loader, loader_version,
        normalized_alias, normalized_alias || '.amalgam', 'starting')
    on conflict (id) do update set world_name = excluded.world_name,
        privacy = excluded.privacy, player_limit = excluded.player_limit,
        alias = excluded.alias, address = excluded.address, status = 'starting',
        updated_at = now();
    insert into essentials_session_members(session_id, user_id, role)
    values (session_id, host_user_id, 'host') on conflict do nothing;
    return jsonb_build_object('id', session_id, 'alias', normalized_alias,
        'address', normalized_alias || '.amalgam', 'status', 'starting');
end $$;

create or replace function public.start_session(session_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set state = 2, status = 'online', updated_at = now()
    where id = session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    return jsonb_build_object('success', true, 'session_id', session_id, 'state', 2, 'status', 'online');
end $$;

create or replace function public.stop_session(session_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set state = 4, status = 'offline', updated_at = now()
    where id = session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    update essentials_session_members set left_at = now()
    where essentials_session_members.session_id = stop_session.session_id and left_at is null;
    return jsonb_build_object('success', true, 'session_id', session_id, 'state', 4, 'status', 'offline');
end $$;

create or replace function public.update_session(session_id text, world_name text, privacy integer,
    player_limit integer, state integer)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update essentials_sessions set world_name = update_session.world_name,
        privacy = update_session.privacy, player_limit = update_session.player_limit,
        state = update_session.state,
        status = case when update_session.state in (1, 2) then 'online' else 'offline' end,
        updated_at = now()
    where id = update_session.session_id and host_user_id = auth.uid();
    if not found then raise exception 'host session not found'; end if;
    return jsonb_build_object('success', true, 'session_id', session_id);
end $$;
