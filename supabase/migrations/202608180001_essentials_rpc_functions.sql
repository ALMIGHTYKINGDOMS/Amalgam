-- RPCs used by the native Essentials client.
-- These are PostgREST functions, not Edge Functions.  Game traffic remains
-- outside Supabase; the database only stores session control metadata.

alter table public.essentials_sessions
    add column if not exists join_token text not null default '';

drop function if exists public.create_session(
    text, uuid, text, integer, integer, text, text, text, text, text, text);

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
    insert into public.essentials_sessions(
        id, host_user_id, world_name, privacy, player_limit, state, join_token_hash,
        join_token, minecraft_version, loader, loader_version, alias, address, status)
    values (
        session_id, host_user_id, world_name, privacy, player_limit, 0,
        encode(digest(join_token, 'sha256'), 'hex'), join_token, minecraft_version,
        loader, loader_version, normalized_alias, normalized_alias || '.amalgam-essentials', 'starting')
    on conflict (id) do update set world_name = excluded.world_name,
        privacy = excluded.privacy, player_limit = excluded.player_limit,
        join_token_hash = excluded.join_token_hash, join_token = excluded.join_token,
        alias = excluded.alias, address = excluded.address, status = 'starting', updated_at = now();
    insert into public.essentials_session_members(session_id, user_id, role)
    values (session_id, host_user_id, 'host') on conflict do nothing;
    return jsonb_build_object('id', session_id, 'alias', normalized_alias,
        'address', normalized_alias || '.amalgam-essentials', 'status', 'starting');
end;
$$;

create or replace function public.send_session_invite(
    invite_id text, target_user_id uuid, session_id text, world_name text default '',
    minecraft_version text default '', loader text default '', loader_version text default '',
    mod_count integer default 0)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    if not exists (
        select 1 from public.essentials_sessions s
        where s.id = session_id and s.host_user_id = auth.uid()
          and s.expires_at > now() and s.status in ('starting', 'online')
    ) then raise exception 'host session not found or offline'; end if;
    if target_user_id = auth.uid() then raise exception 'cannot invite yourself'; end if;
    if not exists (select 1 from auth.users u where u.id = target_user_id) then
        raise exception 'target user not found';
    end if;
    insert into public.essentials_invites(
        id, session_id, host_user_id, target_user_id, status, world_name,
        minecraft_version, loader, loader_version, mod_count)
    values (
        invite_id, session_id, auth.uid(), target_user_id, 'pending', world_name,
        minecraft_version, loader, loader_version, mod_count)
    on conflict (id) do update set status = 'pending', updated_at = now();
    return jsonb_build_object('success', true, 'invite_id', invite_id);
end;
$$;

create or replace function public.accept_session_invite(invite_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare accepted_session public.essentials_sessions;
begin
    select s.* into accepted_session
      from public.essentials_invites i
      join public.essentials_sessions s on s.id = i.session_id
     where i.id = invite_id and i.target_user_id = auth.uid()
       and i.status = 'pending' and i.expires_at > now();
    if accepted_session.id is null then raise exception 'invite not found or expired'; end if;
    update public.essentials_invites i
       set status = 'accepted', updated_at = now()
     where i.id = invite_id and i.target_user_id = auth.uid()
       and i.status = 'pending' and i.expires_at > now();
    if not found then raise exception 'invite not found or expired'; end if;
    return jsonb_build_object('success', true, 'invite_id', invite_id,
        'session_id', accepted_session.id, 'session_address', accepted_session.address,
        'join_token', accepted_session.join_token);
end;
$$;

create or replace function public.decline_session_invite(invite_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update public.essentials_invites i
       set status = 'declined', updated_at = now()
     where i.id = invite_id and i.target_user_id = auth.uid() and i.status = 'pending';
    if not found then raise exception 'invite not found'; end if;
    return jsonb_build_object('success', true, 'invite_id', invite_id);
end;
$$;

create or replace function public.cancel_session_invite(invite_id text)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    update public.essentials_invites i
       set status = 'cancelled', updated_at = now()
     where i.id = invite_id and i.host_user_id = auth.uid() and i.status = 'pending';
    if not found then raise exception 'invite not found'; end if;
    return jsonb_build_object('success', true, 'invite_id', invite_id);
end;
$$;

create or replace function public.get_received_session_invites()
returns setof jsonb language sql security definer set search_path = public as $$
    select jsonb_build_object(
        'id', i.id,
        'host_user_id', i.host_user_id,
        'host_username', coalesce(u.raw_user_meta_data->>'username', u.raw_user_meta_data->>'user_name', split_part(u.email, '@', 1)),
        'target_user_id', i.target_user_id,
        'session_id', i.session_id,
        'session_address', s.address,
        'world_name', i.world_name,
        'minecraft_version', i.minecraft_version,
        'loader', i.loader,
        'loader_version', i.loader_version,
        'mod_count', i.mod_count,
        'status', case i.status when 'pending' then 0 when 'accepted' then 1
            when 'declined' then 2 when 'expired' then 3 else 4 end,
        'created_at', extract(epoch from i.created_at)::bigint,
        'expires_at', extract(epoch from i.expires_at)::bigint)
    from public.essentials_invites i
    join auth.users u on u.id = i.host_user_id
    join public.essentials_sessions s on s.id = i.session_id
    where i.target_user_id = auth.uid()
    order by i.created_at desc;
$$;

create or replace function public.get_sent_session_invites()
returns setof jsonb language sql security definer set search_path = public as $$
    select jsonb_build_object(
        'id', i.id,
        'host_user_id', i.host_user_id,
        'target_user_id', i.target_user_id,
        'session_id', i.session_id,
        'session_address', s.address,
        'world_name', i.world_name,
        'minecraft_version', i.minecraft_version,
        'loader', i.loader,
        'loader_version', i.loader_version,
        'mod_count', i.mod_count,
        'status', case i.status when 'pending' then 0 when 'accepted' then 1
            when 'declined' then 2 when 'expired' then 3 else 4 end,
        'created_at', extract(epoch from i.created_at)::bigint,
        'expires_at', extract(epoch from i.expires_at)::bigint)
    from public.essentials_invites i
    join public.essentials_sessions s on s.id = i.session_id
    where i.host_user_id = auth.uid()
    order by i.created_at desc;
$$;

create or replace function public.kick_player(session_id text, user_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (select 1 from public.essentials_sessions s
                   where s.id = session_id and s.host_user_id = auth.uid()) then
        raise exception 'host session not found';
    end if;
    update public.essentials_session_members m
       set left_at = now()
     where m.session_id = kick_player.session_id and m.user_id = kick_player.user_id
       and m.role <> 'host';
    return jsonb_build_object('success', true, 'session_id', session_id, 'user_id', user_id);
end;
$$;

create or replace function public.ban_player(session_id text, user_id uuid, reason text default '')
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (select 1 from public.essentials_sessions s
                   where s.id = session_id and s.host_user_id = auth.uid()) then
        raise exception 'host session not found';
    end if;
    insert into public.essentials_session_bans(session_id, user_id, reason, banned_by)
    values (session_id, user_id, reason, auth.uid())
    on conflict (session_id, user_id) do update set reason = excluded.reason,
        banned_by = excluded.banned_by, banned_at = now();
    update public.essentials_session_members m
       set left_at = now()
     where m.session_id = ban_player.session_id and m.user_id = ban_player.user_id;
    return jsonb_build_object('success', true, 'session_id', session_id, 'user_id', user_id);
end;
$$;

create or replace function public.unban_player(session_id text, user_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (select 1 from public.essentials_sessions s
                   where s.id = session_id and s.host_user_id = auth.uid()) then
        raise exception 'host session not found';
    end if;
    delete from public.essentials_session_bans b
     where b.session_id = unban_player.session_id and b.user_id = unban_player.user_id;
    return jsonb_build_object('success', true, 'session_id', session_id, 'user_id', user_id);
end;
$$;

revoke all on function public.create_session(text, uuid, text, integer, integer, text, text, text, text, text, text) from public;
revoke all on function public.send_session_invite(text, uuid, text, text, text, text, text, integer) from public;
revoke all on function public.accept_session_invite(text) from public;
revoke all on function public.decline_session_invite(text) from public;
revoke all on function public.cancel_session_invite(text) from public;
revoke all on function public.get_received_session_invites() from public;
revoke all on function public.get_sent_session_invites() from public;
revoke all on function public.kick_player(text, uuid) from public;
revoke all on function public.ban_player(text, uuid, text) from public;
revoke all on function public.unban_player(text, uuid) from public;
grant execute on function public.create_session(text, uuid, text, integer, integer, text, text, text, text, text, text) to authenticated;
grant execute on function public.send_session_invite(text, uuid, text, text, text, text, text, integer) to authenticated;
grant execute on function public.accept_session_invite(text) to authenticated;
grant execute on function public.decline_session_invite(text) to authenticated;
grant execute on function public.cancel_session_invite(text) to authenticated;
grant execute on function public.get_received_session_invites() to authenticated;
grant execute on function public.get_sent_session_invites() to authenticated;
grant execute on function public.kick_player(text, uuid) to authenticated;
grant execute on function public.ban_player(text, uuid, text) to authenticated;
grant execute on function public.unban_player(text, uuid) to authenticated;
