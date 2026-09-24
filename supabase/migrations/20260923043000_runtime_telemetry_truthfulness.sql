-- Runtime telemetry truthfulness.
--
-- A missing JVM/host measurement must remain NULL. Earlier schemas used
-- non-null defaults (notably TPS=20) and coalesced missing values into a
-- seemingly healthy sample. Historical values are intentionally not rewritten:
-- the old rows cannot reliably distinguish an observed zero from a fallback.

alter table public.hosting_nodes
    alter column storage_total_gb drop not null,
    alter column storage_total_gb drop default,
    alter column cpu_usage_pct drop not null,
    alter column cpu_usage_pct drop default,
    alter column storage_used_gb drop not null,
    alter column storage_used_gb drop default;

alter table public.runtime_telemetry
    alter column cpu_pct drop not null,
    alter column cpu_pct drop default,
    alter column memory_used_mb drop not null,
    alter column memory_used_mb drop default,
    alter column memory_max_mb drop not null,
    alter column memory_max_mb drop default,
    alter column memory_pct drop not null,
    alter column memory_pct drop default,
    alter column disk_used_mb drop not null,
    alter column disk_used_mb drop default,
    alter column thread_count drop not null,
    alter column thread_count drop default,
    alter column heap_used_mb drop not null,
    alter column heap_used_mb drop default,
    alter column heap_max_mb drop not null,
    alter column heap_max_mb drop default,
    alter column gc_count drop not null,
    alter column gc_count drop default,
    alter column gc_time_ms drop not null,
    alter column gc_time_ms drop default,
    alter column tps drop not null,
    alter column tps drop default,
    alter column mspt drop not null,
    alter column mspt drop default,
    alter column entity_count drop not null,
    alter column entity_count drop default,
    alter column player_count drop not null,
    alter column player_count drop default,
    alter column chunk_count drop not null,
    alter column chunk_count drop default;

create or replace function public.register_hosting_node(
    p_name text,
    p_region text,
    p_host text,
    p_node_secret text,
    p_cpu_model text default '',
    p_cpu_cores integer default 0,
    p_memory_total_mb bigint default 0,
    p_storage_total_gb bigint default null,
    p_agent_version text default '',
    p_tags text[] default '{}'
)
returns jsonb
language plpgsql
security definer
set search_path = ''
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

    secret_hash := pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex');
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
            storage_total_gb = case when p_storage_total_gb is null then null
                                    else greatest(0, p_storage_total_gb) end,
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
        greatest(0, coalesce(p_memory_total_mb, 0)),
        case when p_storage_total_gb is null then null else greatest(0, p_storage_total_gb) end,
        left(coalesce(p_agent_version, ''), 64), 'online', now()
    ) returning id into new_node_id;

    return jsonb_build_object('id', new_node_id, 'reregistered', false, 'status', 'online');
end;
$$;

create or replace function public.node_heartbeat(
    p_node_id uuid,
    p_node_secret text,
    p_status text default 'online',
    p_cpu_usage_pct real default null,
    p_memory_used_mb bigint default 0,
    p_storage_used_gb bigint default null,
    p_server_count integer default 0
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
begin
    if not exists (
        select 1 from public.hosting_nodes
         where id = p_node_id
           and hashed_secret = pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex')
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.hosting_nodes set
        status = case when p_status in ('online', 'offline', 'maintenance', 'overloaded') then p_status else 'online' end,
        cpu_usage_pct = case when p_cpu_usage_pct is null then null
                             else greatest(0, least(p_cpu_usage_pct, 100)) end,
        memory_used_mb = greatest(0, coalesce(p_memory_used_mb, 0)),
        storage_used_gb = case when p_storage_used_gb is null then null
                               else greatest(0, p_storage_used_gb) end,
        server_count = greatest(0, coalesce(p_server_count, 0)),
        last_heartbeat_at = now(),
        updated_at = now()
    where id = p_node_id;
    return jsonb_build_object('success', true, 'node_id', p_node_id);
end;
$$;

create or replace function public.write_telemetry(
    p_server_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_cpu_pct real default null,
    p_memory_used_mb bigint default null,
    p_memory_max_mb bigint default null,
    p_memory_pct real default null,
    p_disk_used_mb bigint default null,
    p_thread_count integer default null,
    p_heap_used_mb bigint default null,
    p_heap_max_mb bigint default null,
    p_gc_count bigint default null,
    p_gc_time_ms bigint default null,
    p_tps real default null,
    p_mspt real default null,
    p_entity_count integer default null,
    p_player_count integer default null,
    p_chunk_count integer default null,
    p_uptime_ms bigint default 0,
    p_metadata jsonb default '{}'::jsonb
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
begin
    if not exists (
        select 1
          from public.hosting_nodes as n
          join public.server_instances as i on i.node_id = n.id
         where n.id = p_node_id
           and n.hashed_secret = pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex')
           and i.id = p_server_id
    ) then
        raise exception 'invalid node credentials or server not assigned to this node';
    end if;

    insert into public.runtime_telemetry(
        server_id, node_id, cpu_pct, memory_used_mb, memory_max_mb, memory_pct,
        disk_used_mb, thread_count, heap_used_mb, heap_max_mb, gc_count, gc_time_ms,
        tps, mspt, entity_count, player_count, chunk_count, uptime_ms, metadata
    ) values (
        p_server_id, p_node_id,
        case when p_cpu_pct is null then null else greatest(0, least(p_cpu_pct, 100)) end,
        case when p_memory_used_mb is null then null else greatest(0, p_memory_used_mb) end,
        case when p_memory_max_mb is null then null else greatest(0, p_memory_max_mb) end,
        case when p_memory_pct is null then null else greatest(0, least(p_memory_pct, 100)) end,
        case when p_disk_used_mb is null then null else greatest(0, p_disk_used_mb) end,
        case when p_thread_count is null then null else greatest(0, p_thread_count) end,
        case when p_heap_used_mb is null then null else greatest(0, p_heap_used_mb) end,
        case when p_heap_max_mb is null then null else greatest(0, p_heap_max_mb) end,
        case when p_gc_count is null then null else greatest(0, p_gc_count) end,
        case when p_gc_time_ms is null then null else greatest(0, p_gc_time_ms) end,
        case when p_tps is null then null else greatest(0, least(p_tps, 100)) end,
        case when p_mspt is null then null else greatest(0, p_mspt) end,
        case when p_entity_count is null then null else greatest(0, p_entity_count) end,
        case when p_player_count is null then null else greatest(0, p_player_count) end,
        case when p_chunk_count is null then null else greatest(0, p_chunk_count) end,
        greatest(0, coalesce(p_uptime_ms, 0)),
        coalesce(p_metadata, '{}'::jsonb)
    );
    return jsonb_build_object('success', true);
end;
$$;

revoke all on function public.register_hosting_node(text, text, text, text, text, integer, bigint, bigint, text, text[]) from public, anon;
revoke all on function public.node_heartbeat(uuid, text, text, real, bigint, bigint, integer) from public, anon;
revoke all on function public.write_telemetry(uuid, uuid, text, real, bigint, bigint, real, bigint, integer, bigint, bigint, bigint, bigint, real, real, integer, integer, integer, bigint, jsonb) from public, anon;

grant execute on function public.register_hosting_node(text, text, text, text, text, integer, bigint, bigint, text, text[]) to authenticated;
grant execute on function public.node_heartbeat(uuid, text, text, real, bigint, bigint, integer) to authenticated, service_role;
grant execute on function public.write_telemetry(uuid, uuid, text, real, bigint, bigint, real, bigint, integer, bigint, bigint, bigint, bigint, real, real, integer, integer, integer, bigint, jsonb) to authenticated, service_role;
