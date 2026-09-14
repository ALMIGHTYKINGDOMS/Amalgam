-- Production hardening follow-up.
-- This migration is intentionally additive and runs after 202608210006.
-- It removes the runtime node plaintext secret, tightens broad authenticated
-- writes, and adds the RPC needed by the conversation Edge Function.

create extension if not exists pgcrypto with schema extensions;

-- ---------------------------------------------------------------------------
-- Atomic rate limiting used by write-heavy Edge Functions.
-- ---------------------------------------------------------------------------
create or replace function public.enforce_rate_limit(p_action text, p_max_per_minute integer default 60)
returns boolean language plpgsql security definer set search_path = public as $$
declare current_count integer;
       actor uuid := auth.uid();
       lock_key integer;
begin
    if actor is null then raise exception 'authentication required'; end if;
    if length(trim(coalesce(p_action, ''))) = 0 or length(p_action) > 128 then raise exception 'invalid rate-limit action'; end if;
    if p_max_per_minute < 1 or p_max_per_minute > 10000 then raise exception 'invalid rate-limit ceiling'; end if;
    lock_key := hashtext(actor::text || ':' || p_action);
    perform pg_advisory_xact_lock(lock_key);
    delete from public.rate_limits where user_id = actor and window_start < now() - interval '2 minutes';
    select count(*) into current_count from public.rate_limits
     where user_id = actor and action = p_action and window_start > now() - interval '1 minute';
    if current_count >= p_max_per_minute then return false; end if;
    insert into public.rate_limits(user_id, action, count, window_start) values (actor, p_action, 1, now());
    return true;
end;
$$;
revoke all on function public.enforce_rate_limit(text, integer) from public;
grant execute on function public.enforce_rate_limit(text, integer) to authenticated;

-- ---------------------------------------------------------------------------
-- Runtime node credentials: hash at rest, then remove plaintext storage.
-- ---------------------------------------------------------------------------
alter table public.hosting_nodes
    add column if not exists hashed_secret text not null default '';

update public.hosting_nodes
set hashed_secret = encode(extensions.digest(node_secret, 'sha256'), 'hex')
where coalesce(node_secret, '') <> ''
  and coalesce(hashed_secret, '') = '';

create unique index if not exists hosting_nodes_hashed_secret_uidx
    on public.hosting_nodes(hashed_secret)
    where hashed_secret <> '';

create or replace function public.register_hosting_node(
    p_name text,
    p_region text,
    p_host text,
    p_node_secret text,
    p_cpu_model text default '',
    p_cpu_cores integer default 0,
    p_memory_total_mb bigint default 0,
    p_storage_total_gb bigint default 0,
    p_agent_version text default '',
    p_tags text[] default '{}'
)
returns jsonb
language plpgsql
security definer
set search_path = public, extensions
as $$
declare
    existing_node public.hosting_nodes;
    new_node_id uuid;
    secret_hash text;
begin
    if auth.uid() is null then
        raise exception 'authenticated owner required';
    end if;
    if length(trim(coalesce(p_node_secret, ''))) < 32 then
        raise exception 'node secret must be at least 32 characters';
    end if;

    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    select * into existing_node
      from public.hosting_nodes
     where hashed_secret = secret_hash;

    if existing_node.id is not null then
        if existing_node.owner_id <> auth.uid() then
            raise exception 'node secret belongs to another owner';
        end if;
        update public.hosting_nodes set
            name = left(coalesce(p_name, ''), 128),
            region = left(coalesce(p_region, 'us-east-1'), 64),
            host = left(coalesce(p_host, ''), 255),
            cpu_model = left(coalesce(p_cpu_model, ''), 255),
            cpu_cores = greatest(0, least(coalesce(p_cpu_cores, 0), 4096)),
            memory_total_mb = greatest(0, coalesce(p_memory_total_mb, 0)),
            storage_total_gb = greatest(0, coalesce(p_storage_total_gb, 0)),
            agent_version = left(coalesce(p_agent_version, ''), 64),
            tags = coalesce(p_tags, '{}'),
            status = 'online',
            last_heartbeat_at = now(),
            updated_at = now()
        where id = existing_node.id;
        return jsonb_build_object('id', existing_node.id, 'reregistered', true, 'status', 'online');
    end if;

    insert into public.hosting_nodes (
        owner_id, name, region, host, hashed_secret, tags,
        cpu_model, cpu_cores, memory_total_mb, storage_total_gb,
        agent_version, status, last_heartbeat_at
    ) values (
        auth.uid(), left(coalesce(p_name, ''), 128), left(coalesce(p_region, 'us-east-1'), 64),
        left(coalesce(p_host, ''), 255), secret_hash, coalesce(p_tags, '{}'),
        left(coalesce(p_cpu_model, ''), 255), greatest(0, least(coalesce(p_cpu_cores, 0), 4096)),
        greatest(0, coalesce(p_memory_total_mb, 0)), greatest(0, coalesce(p_storage_total_gb, 0)),
        left(coalesce(p_agent_version, ''), 64), 'online', now()
    ) returning id into new_node_id;

    return jsonb_build_object('id', new_node_id, 'reregistered', false, 'status', 'online');
end;
$$;

create or replace function public.node_heartbeat(
    p_node_id uuid,
    p_node_secret text,
    p_status text default 'online',
    p_cpu_usage_pct real default 0,
    p_memory_used_mb bigint default 0,
    p_storage_used_gb bigint default 0,
    p_server_count integer default 0
)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (
        select 1 from public.hosting_nodes
         where id = p_node_id
           and hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex')
    ) then raise exception 'invalid node credentials'; end if;

    update public.hosting_nodes set
        status = case when p_status in ('online', 'offline', 'maintenance', 'overloaded') then p_status else 'online' end,
        cpu_usage_pct = greatest(0, least(coalesce(p_cpu_usage_pct, 0), 100)),
        memory_used_mb = greatest(0, coalesce(p_memory_used_mb, 0)),
        storage_used_gb = greatest(0, coalesce(p_storage_used_gb, 0)),
        server_count = greatest(0, coalesce(p_server_count, 0)),
        last_heartbeat_at = now(), updated_at = now()
    where id = p_node_id;
    return jsonb_build_object('success', true, 'node_id', p_node_id);
end;
$$;

create or replace function public.claim_operation(p_operation_id uuid, p_node_id uuid, p_node_secret text)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
declare op public.server_operations;
begin
    if not exists (select 1 from public.hosting_nodes where id = p_node_id and hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex')) then
        raise exception 'invalid node credentials';
    end if;
    update public.server_operations
       set status = 'claimed', node_id = p_node_id, claimed_at = now()
     where id = p_operation_id and status = 'pending'
     returning * into op;
    if op.id is null then raise exception 'operation not found or already claimed'; end if;
    return to_jsonb(op);
end;
$$;

create or replace function public.start_operation(p_operation_id uuid, p_node_id uuid, p_node_secret text)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (select 1 from public.hosting_nodes where id = p_node_id and hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex')) then
        raise exception 'invalid node credentials';
    end if;
    update public.server_operations set status = 'running', started_at = now()
     where id = p_operation_id and node_id = p_node_id and status = 'claimed';
    if not found then raise exception 'operation is not claimed by this node'; end if;
    return jsonb_build_object('success', true);
end;
$$;

create or replace function public.complete_operation(
    p_operation_id uuid, p_node_id uuid, p_node_secret text,
    p_success boolean default true, p_result jsonb default '{}'::jsonb,
    p_error_message text default ''
)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (select 1 from public.hosting_nodes where id = p_node_id and hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex')) then
        raise exception 'invalid node credentials';
    end if;
    update public.server_operations set
        status = case when p_success then 'completed' else 'failed' end,
        result = coalesce(p_result, '{}'::jsonb),
        error_message = left(coalesce(p_error_message, ''), 4000), completed_at = now()
     where id = p_operation_id and node_id = p_node_id and status in ('claimed', 'running');
    if not found then raise exception 'operation is not active for this node'; end if;
    return jsonb_build_object('success', true);
end;
$$;

create or replace function public.update_server_instance(
    p_server_id uuid, p_node_id uuid, p_node_secret text,
    p_status text default null, p_pid integer default null,
    p_current_players integer default null, p_tps real default null,
    p_disk_usage_mb bigint default null, p_metadata jsonb default null
)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex') and i.id = p_server_id
    ) then raise exception 'invalid node credentials or server not assigned to this node'; end if;
    update public.server_instances set
        status = coalesce(p_status, status), pid = coalesce(p_pid, pid),
        current_players = greatest(0, coalesce(p_current_players, current_players)),
        disk_usage_mb = greatest(0, coalesce(p_disk_usage_mb, disk_usage_mb)),
        metadata = coalesce(p_metadata, metadata), updated_at = now()
     where id = p_server_id;
    return jsonb_build_object('success', true);
end;
$$;

create or replace function public.write_console_lines(p_server_id uuid, p_node_id uuid, p_node_secret text, p_lines jsonb)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (
        select 1 from public.hosting_nodes n join public.server_instances i on i.node_id = n.id
         where n.id = p_node_id and n.hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex') and i.id = p_server_id
    ) then raise exception 'invalid node credentials or server not assigned to this node'; end if;
    if jsonb_typeof(p_lines) <> 'array' or jsonb_array_length(p_lines) > 1000 then raise exception 'invalid console batch'; end if;
    insert into public.console_lines(server_id, line_number, level, source, content, raw)
    select p_server_id,
        greatest(0, coalesce((item->>'line_number')::bigint, 0)),
        coalesce(item->>'level', 'info'), coalesce(item->>'source', 'server'),
        left(coalesce(item->>'content', ''), 16000), left(coalesce(item->>'raw', ''), 16000)
      from jsonb_array_elements(p_lines) item;
    return jsonb_build_object('success', true, 'count', jsonb_array_length(p_lines));
end;
$$;

create or replace function public.write_telemetry(
    p_server_id uuid, p_node_id uuid, p_node_secret text,
    p_cpu_pct real default 0, p_memory_used_mb bigint default 0,
    p_memory_max_mb bigint default 0, p_memory_pct real default 0,
    p_disk_used_mb bigint default 0, p_thread_count integer default 0,
    p_heap_used_mb bigint default 0, p_heap_max_mb bigint default 0,
    p_gc_count bigint default 0, p_gc_time_ms bigint default 0,
    p_tps real default 20.0, p_mspt real default 0,
    p_entity_count integer default 0, p_player_count integer default 0,
    p_chunk_count integer default 0, p_uptime_ms bigint default 0,
    p_metadata jsonb default '{}'::jsonb
)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
begin
    if not exists (
        select 1 from public.hosting_nodes n join public.server_instances i on i.node_id = n.id
         where n.id = p_node_id and n.hashed_secret = encode(digest(p_node_secret, 'sha256'), 'hex') and i.id = p_server_id
    ) then raise exception 'invalid node credentials or server not assigned to this node'; end if;
    insert into public.runtime_telemetry(
        server_id, node_id, cpu_pct, memory_used_mb, memory_max_mb, memory_pct,
        disk_used_mb, thread_count, heap_used_mb, heap_max_mb, gc_count, gc_time_ms,
        tps, mspt, entity_count, player_count, chunk_count, uptime_ms, metadata
    ) values (
        p_server_id, p_node_id, greatest(0, least(coalesce(p_cpu_pct, 0), 100)),
        greatest(0, coalesce(p_memory_used_mb, 0)), greatest(0, coalesce(p_memory_max_mb, 0)),
        greatest(0, least(coalesce(p_memory_pct, 0), 100)), greatest(0, coalesce(p_disk_used_mb, 0)),
        greatest(0, coalesce(p_thread_count, 0)), greatest(0, coalesce(p_heap_used_mb, 0)),
        greatest(0, coalesce(p_heap_max_mb, 0)), greatest(0, coalesce(p_gc_count, 0)),
        greatest(0, coalesce(p_gc_time_ms, 0)), greatest(0, least(coalesce(p_tps, 20), 100)),
        greatest(0, coalesce(p_mspt, 0)), greatest(0, coalesce(p_entity_count, 0)),
        greatest(0, coalesce(p_player_count, 0)), greatest(0, coalesce(p_chunk_count, 0)),
        greatest(0, coalesce(p_uptime_ms, 0)), coalesce(p_metadata, '{}'::jsonb)
    );
    return jsonb_build_object('success', true);
end;
$$;

-- Nothing should be able to read or write the credential hash directly.
revoke all on public.hosting_nodes from anon, authenticated;
grant select, insert, update, delete on public.hosting_nodes to authenticated;

alter table public.hosting_nodes drop column if exists node_secret;

-- ---------------------------------------------------------------------------
-- Close broad authenticated write policies.
-- ---------------------------------------------------------------------------
drop policy if exists "backend manages subscriptions" on public.subscriptions;
drop policy if exists "system manages friendships" on public.friendships;
create policy "participants create own friendship"
on public.friendships for insert to authenticated
with check (user_id_a = auth.uid() or user_id_b = auth.uid());
create policy "participants update own friendship"
on public.friendships for update to authenticated
using (user_id_a = auth.uid() or user_id_b = auth.uid())
with check (user_id_a = auth.uid() or user_id_b = auth.uid());
create policy "participants remove own friendship"
on public.friendships for delete to authenticated
using (user_id_a = auth.uid() or user_id_b = auth.uid());

drop policy if exists "authenticated create conversations" on public.conversations;
drop policy if exists "authenticated add conversation participants" on public.conversation_participants;
drop policy if exists "system insert activity" on public.account_activity;
create policy "users insert own activity"
on public.account_activity for insert to authenticated
with check (user_id = auth.uid());

drop policy if exists "authenticated insert audit" on public.audit_log;
create policy "users insert own audit"
on public.audit_log for insert to authenticated
with check (actor_id = auth.uid());

-- A direct-message conversation must be created atomically with both members.
create or replace function public.create_direct_conversation(p_other_user_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    conversation_id uuid;
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    if p_other_user_id is null or p_other_user_id = auth.uid() then raise exception 'invalid participant'; end if;
    if not exists (select 1 from auth.users where id = p_other_user_id) then raise exception 'participant not found'; end if;

    select c.id into conversation_id
      from public.conversations c
      join public.conversation_participants me on me.conversation_id = c.id and me.user_id = auth.uid()
      join public.conversation_participants them on them.conversation_id = c.id and them.user_id = p_other_user_id
     where not exists (
         select 1 from public.conversation_participants extra
          where extra.conversation_id = c.id
            and extra.user_id not in (auth.uid(), p_other_user_id)
     )
     limit 1;

    if conversation_id is null then
        insert into public.conversations default values returning id into conversation_id;
        insert into public.conversation_participants(conversation_id, user_id)
        values (conversation_id, auth.uid()), (conversation_id, p_other_user_id);
    end if;
    return jsonb_build_object('id', conversation_id, 'participant_ids', jsonb_build_array(auth.uid(), p_other_user_id));
end;
$$;
revoke all on function public.create_direct_conversation(uuid) from public;
grant execute on function public.create_direct_conversation(uuid) to authenticated;

-- ---------------------------------------------------------------------------
-- Social mutations: use atomic SECURITY DEFINER functions instead of broad
-- authenticated table writes. The Edge Functions still authenticate first,
-- while these functions enforce ownership again at the database boundary.
-- ---------------------------------------------------------------------------
create or replace function public.send_friend_request(p_receiver_id uuid, p_message text default '')
returns jsonb language plpgsql security definer set search_path = public as $$
declare request_id uuid; a uuid; b uuid;
begin
    if auth.uid() is null or p_receiver_id is null or p_receiver_id = auth.uid() then raise exception 'invalid receiver'; end if;
    if length(coalesce(p_message, '')) > 500 then raise exception 'message too long'; end if;
    if not exists (select 1 from auth.users where id = p_receiver_id) then raise exception 'user not found'; end if;
    a := least(auth.uid(), p_receiver_id); b := greatest(auth.uid(), p_receiver_id);
    if exists (select 1 from public.friendships where user_id_a = a and user_id_b = b and status = 'accepted') then raise exception 'already friends'; end if;
    if exists (select 1 from public.friend_requests where status = 'pending' and ((sender_id = auth.uid() and receiver_id = p_receiver_id) or (sender_id = p_receiver_id and receiver_id = auth.uid()))) then raise exception 'request already pending'; end if;
    insert into public.friend_requests(sender_id, receiver_id, message) values (auth.uid(), p_receiver_id, coalesce(p_message, '')) returning id into request_id;
    return jsonb_build_object('success', true, 'request_id', request_id);
end;
$$;

create or replace function public.accept_friend_request(p_request_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
declare r public.friend_requests; a uuid; b uuid;
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    select * into r from public.friend_requests
     where id = p_request_id and receiver_id = auth.uid() and status = 'pending' for update;
    if r.id is null then raise exception 'request not found'; end if;
    a := least(r.sender_id, r.receiver_id); b := greatest(r.sender_id, r.receiver_id);
    update public.friend_requests set status = 'accepted', updated_at = now() where id = r.id;
    insert into public.friendships(user_id_a, user_id_b, status) values (a, b, 'accepted')
      on conflict (user_id_a, user_id_b) do update set status = 'accepted', updated_at = now();
    return jsonb_build_object('success', true, 'request_id', r.id);
end;
$$;

create or replace function public.reject_friend_request(p_request_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    update public.friend_requests set status = 'rejected', updated_at = now()
     where id = p_request_id and receiver_id = auth.uid() and status = 'pending';
    if not found then raise exception 'request not found'; end if;
    return jsonb_build_object('success', true, 'request_id', p_request_id);
end;
$$;

create or replace function public.remove_friend(p_friend_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
declare a uuid; b uuid;
begin
    if auth.uid() is null or p_friend_id is null or p_friend_id = auth.uid() then raise exception 'invalid friend'; end if;
    a := least(auth.uid(), p_friend_id); b := greatest(auth.uid(), p_friend_id);
    delete from public.friendships where user_id_a = a and user_id_b = b;
    return jsonb_build_object('success', true, 'friend_id', p_friend_id);
end;
$$;

create or replace function public.send_conversation_message(p_conversation_id uuid, p_content text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare message_row public.messages;
begin
    if auth.uid() is null or length(trim(coalesce(p_content, ''))) = 0 or length(p_content) > 4000 then raise exception 'invalid message'; end if;
    if not exists (select 1 from public.conversation_participants where conversation_id = p_conversation_id and user_id = auth.uid()) then
        raise exception 'not a participant';
    end if;
    insert into public.messages(conversation_id, sender_id, content)
      values (p_conversation_id, auth.uid(), trim(p_content)) returning * into message_row;
    update public.conversations set
      last_message_content = message_row.content,
      last_message_sender_id = auth.uid(),
      last_message_at = message_row.created_at,
      updated_at = now()
      where id = p_conversation_id;
    return to_jsonb(message_row);
end;
$$;

create or replace function public.mark_conversation_read(p_conversation_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null or not exists (select 1 from public.conversation_participants where conversation_id = p_conversation_id and user_id = auth.uid()) then
        raise exception 'not a participant';
    end if;
    update public.messages set is_read = true
     where conversation_id = p_conversation_id and sender_id <> auth.uid() and is_read = false;
    update public.conversation_participants set last_read_at = now()
     where conversation_id = p_conversation_id and user_id = auth.uid();
    return jsonb_build_object('success', true);
end;
$$;

create or replace function public.create_party(p_name text, p_is_public boolean default true, p_max_members integer default 10)
returns jsonb language plpgsql security definer set search_path = public, extensions as $$
declare party_row public.parties; invite text;
begin
    if auth.uid() is null or length(trim(coalesce(p_name, ''))) = 0 or length(p_name) > 64 then raise exception 'invalid party name'; end if;
    invite := upper(substr(encode(gen_random_bytes(8), 'hex'), 1, 8));
    insert into public.parties(owner_id, name, is_public, invite_code, max_members)
      values (auth.uid(), trim(p_name), coalesce(p_is_public, true), invite, greatest(2, least(coalesce(p_max_members, 10), 50)))
      returning * into party_row;
    insert into public.party_members(party_id, user_id, role) values (party_row.id, auth.uid(), 'owner');
    return jsonb_build_object('id', party_row.id, 'owner_id', party_row.owner_id, 'name', party_row.name,
      'is_public', party_row.is_public, 'invite_code', party_row.invite_code, 'max_members', party_row.max_members,
      'member_count', 1, 'created_at', extract(epoch from party_row.created_at)::bigint);
end;
$$;

create or replace function public.join_party(p_invite_code text)
returns jsonb language plpgsql security definer set search_path = public as $$
declare party_row public.parties; member_count integer;
begin
    if auth.uid() is null or length(trim(coalesce(p_invite_code, ''))) <> 8 then raise exception 'invalid invite code'; end if;
    select * into party_row from public.parties where invite_code = upper(trim(p_invite_code)) for update;
    if party_row.id is null then raise exception 'party not found'; end if;
    if exists (select 1 from public.party_members where party_id = party_row.id and user_id = auth.uid()) then raise exception 'already a member'; end if;
    select count(*) into member_count from public.party_members where party_id = party_row.id;
    if member_count >= party_row.max_members then raise exception 'party is full'; end if;
    insert into public.party_members(party_id, user_id, role) values (party_row.id, auth.uid(), 'member');
    return jsonb_build_object('success', true, 'party_id', party_row.id);
end;
$$;

create or replace function public.leave_party(p_party_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    if exists (select 1 from public.parties where id = p_party_id and owner_id = auth.uid()) then raise exception 'owner cannot leave; disband instead'; end if;
    delete from public.party_members where party_id = p_party_id and user_id = auth.uid();
    return jsonb_build_object('success', true, 'party_id', p_party_id);
end;
$$;

create or replace function public.disband_party(p_party_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null or not exists (select 1 from public.parties where id = p_party_id and owner_id = auth.uid()) then raise exception 'only the owner can disband'; end if;
    delete from public.parties where id = p_party_id;
    return jsonb_build_object('success', true, 'party_id', p_party_id);
end;
$$;

-- Remove direct mutation policies; the functions above are the only write path.
drop policy if exists "senders create friend requests" on public.friend_requests;
drop policy if exists "participants update friend requests" on public.friend_requests;
drop policy if exists "participants create own friendship" on public.friendships;
drop policy if exists "participants update own friendship" on public.friendships;
drop policy if exists "participants remove own friendship" on public.friendships;
drop policy if exists "participants send messages" on public.messages;
drop policy if exists "sender update own messages" on public.messages;
drop policy if exists "participants update conversations" on public.conversations;
drop policy if exists "members join parties" on public.party_members;
drop policy if exists "members leave parties" on public.party_members;
drop policy if exists "owners manage members" on public.party_members;
drop policy if exists "owners manage parties" on public.parties;
drop policy if exists "authenticated create parties" on public.parties;

-- Grant only the authenticated RPC surface.
revoke all on function public.send_friend_request(uuid,text) from public;
revoke all on function public.accept_friend_request(uuid) from public;
revoke all on function public.reject_friend_request(uuid) from public;
revoke all on function public.remove_friend(uuid) from public;
revoke all on function public.send_conversation_message(uuid,text) from public;
revoke all on function public.mark_conversation_read(uuid) from public;
revoke all on function public.create_party(text,boolean,integer) from public;
revoke all on function public.join_party(text) from public;
revoke all on function public.leave_party(uuid) from public;
revoke all on function public.disband_party(uuid) from public;
grant execute on function public.send_friend_request(uuid,text) to authenticated;
grant execute on function public.accept_friend_request(uuid) to authenticated;
grant execute on function public.reject_friend_request(uuid) to authenticated;
grant execute on function public.remove_friend(uuid) to authenticated;
grant execute on function public.send_conversation_message(uuid,text) to authenticated;
grant execute on function public.mark_conversation_read(uuid) to authenticated;
grant execute on function public.create_party(text,boolean,integer) to authenticated;
grant execute on function public.join_party(text) to authenticated;
grant execute on function public.leave_party(uuid) to authenticated;
grant execute on function public.disband_party(uuid) to authenticated;

-- Lock down function execution to the intended roles after replacement.
revoke all on function public.register_hosting_node(text,text,text,text,text,integer,bigint,bigint,text,text[]) from public;
revoke all on function public.node_heartbeat(uuid,text,text,real,bigint,bigint,integer) from public;
revoke all on function public.claim_operation(uuid,uuid,text) from public;
revoke all on function public.start_operation(uuid,uuid,text) from public;
revoke all on function public.complete_operation(uuid,uuid,text,boolean,jsonb,text) from public;
revoke all on function public.update_server_instance(uuid,uuid,text,text,integer,integer,real,bigint,jsonb) from public;
revoke all on function public.write_console_lines(uuid,uuid,text,jsonb) from public;
revoke all on function public.write_telemetry(uuid,uuid,text,real,bigint,bigint,real,bigint,integer,bigint,bigint,bigint,bigint,real,real,integer,integer,integer,bigint,jsonb) from public;
grant execute on function public.register_hosting_node(text,text,text,text,text,integer,bigint,bigint,text,text[]) to authenticated;
grant execute on function public.node_heartbeat(uuid,text,text,real,bigint,bigint,integer) to authenticated, service_role;
grant execute on function public.claim_operation(uuid,uuid,text) to authenticated, service_role;
grant execute on function public.start_operation(uuid,uuid,text) to authenticated, service_role;
grant execute on function public.complete_operation(uuid,uuid,text,boolean,jsonb,text) to authenticated, service_role;
grant execute on function public.update_server_instance(uuid,uuid,text,text,integer,integer,real,bigint,jsonb) to authenticated, service_role;
grant execute on function public.write_console_lines(uuid,uuid,text,jsonb) to authenticated, service_role;
grant execute on function public.write_telemetry(uuid,uuid,text,real,bigint,bigint,real,bigint,integer,bigint,bigint,bigint,bigint,real,real,integer,integer,integer,bigint,jsonb) to authenticated, service_role;

create unique index if not exists subscriptions_provider_external_uidx
    on public.subscriptions(provider, external_id)
    where external_id <> '';

-- ---------------------------------------------------------------------------
-- Billing webhook idempotency and provider identity mapping.
-- Whop customer IDs are not necessarily Supabase UUIDs; the mapping must be
-- explicit instead of trusting an external string as auth.users.id.
-- ---------------------------------------------------------------------------
create table if not exists public.whop_customer_mappings (
    whop_customer_id text primary key,
    user_id uuid not null references auth.users(id) on delete cascade,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);
alter table public.whop_customer_mappings enable row level security;
create policy "users read own whop mapping"
on public.whop_customer_mappings for select to authenticated
using (user_id = auth.uid());

create table if not exists public.webhook_events (
    provider text not null,
    event_id text not null,
    event_type text not null default '',
    payload jsonb not null default '{}'::jsonb,
    received_at timestamptz not null default now(),
    processed_at timestamptz,
    primary key (provider, event_id)
);
alter table public.webhook_events enable row level security;
create index if not exists webhook_events_received_idx on public.webhook_events(received_at desc);
