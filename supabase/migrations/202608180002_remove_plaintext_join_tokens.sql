-- Replace the temporary plaintext invite-token bridge with a one-time invite
-- credential. The session retains only its SHA-256 token hash; an accepted
-- invite ID authorizes the invited user to join that session.

alter table public.essentials_sessions
    drop column if exists join_token;

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
        minecraft_version, loader, loader_version, alias, address, status)
    values (
        session_id, host_user_id, world_name, privacy, player_limit, 0,
        encode(digest(join_token, 'sha256'), 'hex'), minecraft_version,
        loader, loader_version, normalized_alias, normalized_alias || '.amalgam-essentials', 'starting')
    on conflict (id) do update set world_name = excluded.world_name,
        privacy = excluded.privacy, player_limit = excluded.player_limit,
        join_token_hash = excluded.join_token_hash, alias = excluded.alias,
        address = excluded.address, status = 'starting', updated_at = now();
    insert into public.essentials_session_members(session_id, user_id, role)
    values (session_id, host_user_id, 'host') on conflict do nothing;
    return jsonb_build_object('id', session_id, 'alias', normalized_alias,
        'address', normalized_alias || '.amalgam-essentials', 'status', 'starting');
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
     where i.id = invite_id and i.target_user_id = auth.uid() and i.status = 'pending';
    return jsonb_build_object('success', true, 'invite_id', invite_id,
        'session_id', accepted_session.id, 'session_address', accepted_session.address,
        'join_token', invite_id);
end;
$$;

create or replace function public.join_session(session_id text, join_token text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare s public.essentials_sessions;
begin
    select * into s from public.essentials_sessions
     where id = session_id and expires_at > now();
    if s.id is null then raise exception 'session not found or expired'; end if;
    if s.state not in (0, 1, 2) then raise exception 'session is not online'; end if;
    if s.join_token_hash <> encode(digest(join_token, 'sha256'), 'hex')
       and not exists (
           select 1 from public.essentials_invites i
           where i.id = join_token and i.session_id = s.id
             and i.target_user_id = auth.uid() and i.status = 'accepted'
             and i.expires_at > now()) then
        raise exception 'invalid join token';
    end if;
    if exists (select 1 from public.essentials_session_bans b
               where b.session_id = s.id and b.user_id = auth.uid()) then
        raise exception 'user is banned';
    end if;
    insert into public.essentials_session_members(session_id, user_id, role)
    values (s.id, auth.uid(), 'player') on conflict (session_id, user_id) do update set left_at = null;
    return jsonb_build_object('success', true, 'session_id', s.id, 'host_user_id', s.host_user_id);
end;
$$;

create or replace function public.get_session_manifest(session_id text, join_token text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare s public.essentials_sessions; m public.essentials_manifests; result jsonb;
begin
    select * into s from public.essentials_sessions
     where id = session_id and expires_at > now();
    if s.id is null or (
        s.join_token_hash <> encode(digest(join_token, 'sha256'), 'hex') and
        not exists (select 1 from public.essentials_invites i
                   where i.id = join_token and i.session_id = s.id
                     and i.target_user_id = auth.uid() and i.status = 'accepted'
                     and i.expires_at > now())) then
        raise exception 'invalid session credentials';
    end if;
    select * into m from public.essentials_manifests where m.session_id = get_session_manifest.session_id;
    if m.session_id is null then raise exception 'manifest not found'; end if;
    select jsonb_build_object('session_id', m.session_id, 'host_user_id', m.host_user_id,
        'profile_id', m.profile_id, 'profile_name', m.profile_name,
        'minecraft_version', m.minecraft_version, 'loader', m.loader,
        'loader_version', m.loader_version, 'required_resource_packs', m.required_resource_packs,
        'required_configs', m.required_configs, 'mods', coalesce((select jsonb_agg(to_jsonb(mm))
            from public.essentials_manifest_mods mm where mm.session_id = m.session_id), '[]'::jsonb))
        into result;
    return result;
end;
$$;
