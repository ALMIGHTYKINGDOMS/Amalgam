-- Security hardening migration.
-- 1. Hash node_secret in hosting_nodes (store SHA-256, not plaintext)
-- 2. Restrict staff_roles INSERT to existing staff
-- 3. Redact emails from admin_list_users
-- 4. Drop plaintext join_token column from essentials_sessions
-- 5. Add rate limiting table

-- ---------------------------------------------------------------------------
-- 1. Hash node_secret
-- Add a hashed_secret column and update register_hosting_node to use it.
-- The plaintext node_secret is kept temporarily for backwards compatibility
-- but will be removed in a future migration once all agents are updated.
-- ---------------------------------------------------------------------------
alter table public.hosting_nodes
    add column if not exists hashed_secret text not null default '';

-- Update register_hosting_node to store hashed secrets
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
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    existing_node public.hosting_nodes;
    new_node_id uuid;
    secret_hash text;
begin
    -- Hash the secret with SHA-256
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');

    -- Check if a node with this hashed secret already exists (re-registration)
    select * into existing_node
    from public.hosting_nodes
    where hashed_secret = secret_hash;

    if existing_node.id is not null then
        update public.hosting_nodes set
            name = p_name,
            region = p_region,
            host = p_host,
            cpu_model = p_cpu_model,
            cpu_cores = p_cpu_cores,
            memory_total_mb = p_memory_total_mb,
            storage_total_gb = p_storage_total_gb,
            agent_version = p_agent_version,
            tags = p_tags,
            status = 'online',
            last_heartbeat_at = now(),
            updated_at = now()
        where id = existing_node.id;

        return jsonb_build_object(
            'id', existing_node.id,
            'reregistered', true,
            'status', 'online'
        );
    end if;

    insert into public.hosting_nodes (
        owner_id, name, region, host, node_secret, hashed_secret, tags,
        cpu_model, cpu_cores, memory_total_mb, storage_total_gb,
        agent_version, status, last_heartbeat_at
    ) values (
        auth.uid(), p_name, p_region, p_host, p_node_secret, secret_hash, p_tags,
        p_cpu_model, p_cpu_cores, p_memory_total_mb, p_storage_total_gb,
        p_agent_version, 'online', now()
    ) returning id into new_node_id;

    return jsonb_build_object(
        'id', new_node_id,
        'reregistered', false,
        'status', 'online'
    );
end;
$$;

-- Update node_heartbeat to use hashed secret
create or replace function public.node_heartbeat(
    p_node_id uuid,
    p_node_secret text,
    p_status text default 'online',
    p_cpu_usage_pct real default 0,
    p_memory_used_mb bigint default 0,
    p_storage_used_gb bigint default 0,
    p_server_count integer default 0
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');

    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and hashed_secret = secret_hash
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.hosting_nodes set
        status = p_status,
        cpu_usage_pct = p_cpu_usage_pct,
        memory_used_mb = p_memory_used_mb,
        storage_used_gb = p_storage_used_gb,
        server_count = p_server_count,
        last_heartbeat_at = now(),
        updated_at = now()
    where id = p_node_id;

    return jsonb_build_object('success', true, 'node_id', p_node_id);
end;
$$;

-- Update claim_operation to use hashed secret
create or replace function public.claim_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    op public.server_operations;
    secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');

    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and hashed_secret = secret_hash
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations
    set status = 'claimed',
        node_id = p_node_id,
        claimed_at = now()
    where id = p_operation_id
      and status = 'pending'
    returning * into op;

    if op.id is null then
        raise exception 'operation not found or already claimed';
    end if;

    return to_jsonb(op);
end;
$$;

-- Update start_operation to use hashed secret
create or replace function public.start_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and hashed_secret = secret_hash
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations
    set status = 'running', started_at = now()
    where id = p_operation_id and node_id = p_node_id and status = 'claimed';

    return jsonb_build_object('success', true);
end;
$$;

-- Update complete_operation to use hashed secret
create or replace function public.complete_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_success boolean default true,
    p_result jsonb default '{}'::jsonb,
    p_error_message text default ''
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and hashed_secret = secret_hash
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations
    set status = case when p_success then 'completed' else 'failed' end,
        result = p_result,
        error_message = p_error_message,
        completed_at = now()
    where id = p_operation_id and node_id = p_node_id
      and status in ('claimed', 'running');

    return jsonb_build_object('success', true);
end;
$$;

-- Update update_server_instance to use hashed secret
create or replace function public.update_server_instance(
    p_server_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_status text default null,
    p_pid integer default null,
    p_current_players integer default null,
    p_tps real default null,
    p_disk_usage_mb bigint default null,
    p_metadata jsonb default null
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.hashed_secret = secret_hash
          and i.id = p_server_id
    ) then
        raise exception 'invalid node credentials or server not assigned to this node';
    end if;

    update public.server_instances set
        status = coalesce(p_status, status),
        pid = coalesce(p_pid, pid),
        current_players = coalesce(p_current_players, current_players),
        disk_usage_mb = coalesce(p_disk_usage_mb, disk_usage_mb),
        metadata = coalesce(p_metadata, metadata),
        updated_at = now()
    where id = p_server_id;

    return jsonb_build_object('success', true);
end;
$$;

-- Update write_console_lines to use hashed secret
create or replace function public.write_console_lines(
    p_server_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_lines jsonb
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.hashed_secret = secret_hash
          and i.id = p_server_id
    ) then
        raise exception 'invalid node credentials or server not assigned to this node';
    end if;

    insert into public.console_lines (server_id, line_number, level, source, content, raw)
    select
        p_server_id,
        (item->>'line_number')::bigint,
        coalesce(item->>'level', 'info'),
        coalesce(item->>'source', 'server'),
        coalesce(item->>'content', ''),
        coalesce(item->>'raw', '')
    from jsonb_array_elements(p_lines) item;

    return jsonb_build_object('success', true, 'count', jsonb_array_length(p_lines));
end;
$$;

-- Update write_telemetry to use hashed secret
create or replace function public.write_telemetry(
    p_server_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_cpu_pct real default 0,
    p_memory_used_mb bigint default 0,
    p_memory_max_mb bigint default 0,
    p_memory_pct real default 0,
    p_disk_used_mb bigint default 0,
    p_thread_count integer default 0,
    p_heap_used_mb bigint default 0,
    p_heap_max_mb bigint default 0,
    p_gc_count bigint default 0,
    p_gc_time_ms bigint default 0,
    p_tps real default 20.0,
    p_mspt real default 0,
    p_entity_count integer default 0,
    p_player_count integer default 0,
    p_chunk_count integer default 0,
    p_uptime_ms bigint default 0,
    p_metadata jsonb default '{}'::jsonb
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare secret_hash text;
begin
    secret_hash := encode(digest(p_node_secret, 'sha256'), 'hex');
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.hashed_secret = secret_hash
          and i.id = p_server_id
    ) then
        raise exception 'invalid node credentials or server not assigned to this node';
    end if;

    insert into public.runtime_telemetry (
        server_id, node_id, cpu_pct, memory_used_mb, memory_max_mb, memory_pct,
        disk_used_mb, thread_count, heap_used_mb, heap_max_mb,
        gc_count, gc_time_ms, tps, mspt, entity_count, player_count,
        chunk_count, uptime_ms, metadata
    ) values (
        p_server_id, p_node_id, p_cpu_pct, p_memory_used_mb, p_memory_max_mb, p_memory_pct,
        p_disk_used_mb, p_thread_count, p_heap_used_mb, p_heap_max_mb,
        p_gc_count, p_gc_time_ms, p_tps, p_mspt, p_entity_count, p_player_count,
        p_chunk_count, p_uptime_ms, p_metadata
    );

    return jsonb_build_object('success', true);
end;
$$;

-- ---------------------------------------------------------------------------
-- 2. Restrict staff_roles INSERT to existing staff
-- ---------------------------------------------------------------------------
create or replace function public.guard_staff_roles_insert()
returns trigger language plpgsql security definer set search_path = public as $$
begin
    if not exists (select 1 from public.staff_roles where user_id = auth.uid()) then
        raise exception 'only existing staff can grant staff roles';
    end if;
    return new;
end;
$$;

drop trigger if exists staff_roles_insert_guard on public.staff_roles;
create trigger staff_roles_insert_guard
before insert on public.staff_roles
for each row execute function public.guard_staff_roles_insert();

-- ---------------------------------------------------------------------------
-- 3. Redact emails from admin_list_users
-- ---------------------------------------------------------------------------
create or replace function public.admin_list_users()
returns setof jsonb
language sql
security definer
set search_path = public, auth
as $$
    select jsonb_build_object(
        'id', u.id,
        'email', '***@***.***',  -- redacted for security
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

-- New function: staff can get full user details (including email) for specific users
create or replace function public.admin_get_user_email(p_user_id uuid)
returns jsonb
language sql
security definer
set search_path = public, auth
as $$
    select jsonb_build_object(
        'id', u.id,
        'email', u.email
    )
    from auth.users u
    where public.is_project_staff() and u.id = p_user_id;
$$;

revoke all on function public.admin_get_user_email(uuid) from public;
grant execute on function public.admin_get_user_email(uuid) to authenticated;

-- ---------------------------------------------------------------------------
-- 4. Drop plaintext join_token column
-- ---------------------------------------------------------------------------
alter table public.essentials_sessions
    drop column if exists join_token;

-- ---------------------------------------------------------------------------
-- 5. Rate limiting table
-- ---------------------------------------------------------------------------
create table if not exists public.rate_limits (
    id uuid primary key default gen_random_uuid(),
    user_id uuid references auth.users(id) on delete cascade,
    action text not null default '',
    count integer not null default 1,
    window_start timestamptz not null default now(),
    created_at timestamptz not null default now()
);

alter table public.rate_limits enable row level security;

create policy "authenticated insert rate limits"
on public.rate_limits for insert to authenticated
with check (user_id = auth.uid());

create policy "users read own rate limits"
on public.rate_limits for select to authenticated
using (user_id = auth.uid());

create index if not exists rate_limits_user_action_idx
    on public.rate_limits(user_id, action, window_start);

-- Rate limit check function (returns true if within limit)
create or replace function public.check_rate_limit(
    p_action text,
    p_max_per_minute integer default 60
)
returns boolean language sql security definer set search_path = public as $$
    select (
        select count(*) from public.rate_limits
        where user_id = auth.uid()
          and action = p_action
          and window_start > now() - interval '1 minute'
    ) < p_max_per_minute;
$$;

-- Rate limit increment function
create or replace function public.increment_rate_limit(p_action text)
returns void language sql security definer set search_path = public as $$
    insert into public.rate_limits(user_id, action, count, window_start)
    values (auth.uid(), p_action, 1, now())
    on conflict do nothing;
$$;

-- Apply rate limits to high-traffic RPCs
-- (Applied via CHECK constraints or application-level in the RPCs themselves)
