-- Runtime node control plane.
-- These tables power the Amalgam runtime node agent, which hosts Minecraft
-- servers on behalf of users. The launcher (browser/app) never writes to
-- these tables directly; it calls RPCs or the API layer does.

-- ---------------------------------------------------------------------------
-- Hosting nodes: physical or virtual machines that run Minecraft servers
-- ---------------------------------------------------------------------------
create table if not exists public.hosting_nodes (
    id uuid primary key default gen_random_uuid(),
    owner_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    region text not null default 'us-east-1',
    host text not null default '',
    ssh_port integer not null default 22,
    api_port integer not null default 8080,
    status text not null default 'offline' check (status in ('online', 'offline', 'maintenance', 'overloaded')),
    node_secret text not null default '',  -- pre-shared key for agent auth
    tags text[] not null default '{}',

    -- Hardware specs (reported by agent)
    cpu_model text not null default '',
    cpu_cores integer not null default 0,
    memory_total_mb bigint not null default 0,
    storage_total_gb bigint not null default 0,

    -- Live resource usage (updated by heartbeat)
    cpu_usage_pct real not null default 0,
    memory_used_mb bigint not null default 0,
    storage_used_gb bigint not null default 0,
    server_count integer not null default 0,

    -- Heartbeat tracking
    last_heartbeat_at timestamptz,
    agent_version text not null default '',

    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.hosting_nodes enable row level security;

create policy "node owners manage nodes"
on public.hosting_nodes for all to authenticated
using (owner_id = auth.uid()) with check (owner_id = auth.uid());

-- Staff can read all nodes (admin dashboard)
create policy "staff read nodes"
on public.hosting_nodes for select to authenticated
using (public.is_project_staff());

create index if not exists hosting_nodes_owner_idx on public.hosting_nodes(owner_id);
create index if not exists hosting_nodes_status_idx on public.hosting_nodes(status);

-- ---------------------------------------------------------------------------
-- Hosting templates: reusable server configurations
-- ---------------------------------------------------------------------------
create table if not exists public.hosting_templates (
    id uuid primary key default gen_random_uuid(),
    owner_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    description text not null default '',
    minecraft_version text not null default 'latest',
    loader text not null default 'vanilla',
    loader_version text not null default '',
    server_jar_url text not null default '',
    min_memory_mb integer not null default 1024,
    max_memory_mb integer not null default 2048,
    jvm_args text[] not null default '{}',
    server_properties jsonb not null default '{}'::jsonb,
    required_mods jsonb not null default '[]'::jsonb,
    required_plugins jsonb not null default '[]'::jsonb,
    auto_restart boolean not null default true,
    max_players integer not null default 20,
    is_public boolean not null default false,
    tags text[] not null default '{}',
    use_count integer not null default 0,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.hosting_templates enable row level security;

create policy "template owners manage templates"
on public.hosting_templates for all to authenticated
using (owner_id = auth.uid()) with check (owner_id = auth.uid());

create policy "public reads public templates"
on public.hosting_templates for select to authenticated
using (is_public = true or owner_id = auth.uid());

create index if not exists hosting_templates_owner_idx on public.hosting_templates(owner_id);

-- ---------------------------------------------------------------------------
-- Server instances: running Minecraft servers managed by the runtime
-- ---------------------------------------------------------------------------
create table if not exists public.server_instances (
    id uuid primary key default gen_random_uuid(),
    user_id uuid not null references auth.users(id) on delete cascade,
    node_id uuid not null references public.hosting_nodes(id) on delete cascade,
    template_id uuid references public.hosting_templates(id) on delete set null,

    name text not null default '',
    alias text not null default '',
    address text not null default '',
    status text not null default 'stopped' check (status in (
        'stopped', 'starting', 'running', 'stopping', 'restarting', 'error', 'maintenance'
    )),

    -- Connection info
    host text not null default '',
    port integer not null default 25565,
    query_port integer not null default 25565,
    rcon_port integer not null default 0,

    -- Server config
    minecraft_version text not null default '',
    loader text not null default 'vanilla',
    loader_version text not null default '',
    motd text not null default '',
    max_players integer not null default 20,
    current_players integer not null default 0,
    world_name text not null default 'world',
    gamemode text not null default 'survival',
    difficulty text not null default 'normal',
    whitelist_enabled boolean not null default false,
    online_mode boolean not null default true,

    -- Resource allocation
    memory_mb integer not null default 2048,
    jvm_args text[] not null default '{}',
    disk_usage_mb bigint not null default 0,

    -- Process tracking
    pid integer not null default 0,
    java_path text not null default '',
    working_dir text not null default '',

    -- Timestamps
    last_started_at timestamptz,
    last_stopped_at timestamptz,
    last_backup_at timestamptz,
    uptime_seconds bigint not null default 0,

    -- Labels and metadata
    tags text[] not null default '{}',
    metadata jsonb not null default '{}'::jsonb,

    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.server_instances enable row level security;

create policy "instance owners manage instances"
on public.server_instances for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

create index if not exists server_instances_user_idx on public.server_instances(user_id);
create index if not exists server_instances_node_idx on public.server_instances(node_id);
create index if not exists server_instances_status_idx on public.server_instances(status);

-- ---------------------------------------------------------------------------
-- Server operations: queued commands for runtime nodes to execute
-- ---------------------------------------------------------------------------
create table if not exists public.server_operations (
    id uuid primary key default gen_random_uuid(),
    server_id uuid not null references public.server_instances(id) on delete cascade,
    node_id uuid references public.hosting_nodes(id) on delete set null,
    user_id uuid not null references auth.users(id) on delete cascade,

    operation text not null check (operation in (
        'start', 'stop', 'restart', 'command', 'backup', 'restore',
        'settings', 'install_jar', 'update_jar', 'delete'
    )),
    status text not null default 'pending' check (status in (
        'pending', 'claimed', 'running', 'completed', 'failed', 'cancelled'
    )),

    -- Operation payload
    params jsonb not null default '{}'::jsonb,
    command text not null default '',

    -- Results
    result jsonb not null default '{}'::jsonb,
    error_message text not null default '',
    progress_pct integer not null default 0,

    -- Timing
    claimed_at timestamptz,
    started_at timestamptz,
    completed_at timestamptz,
    timeout_seconds integer not null default 300,

    created_at timestamptz not null default now()
);

alter table public.server_operations enable row level security;

create policy "operation owners see own operations"
on public.server_operations for select to authenticated
using (user_id = auth.uid());

create policy "node owners claim and update operations"
on public.server_operations for update to authenticated
using (
    exists (
        select 1 from public.hosting_nodes n
        where n.id = server_operations.node_id and n.owner_id = auth.uid()
    )
);

create index if not exists server_operations_server_idx on public.server_operations(server_id);
create index if not exists server_operations_node_idx on public.server_operations(node_id);
create index if not exists server_operations_status_idx on public.server_operations(status);
create index if not exists server_operations_pending_idx
    on public.server_operations(status, created_at)
    where status = 'pending';

-- ---------------------------------------------------------------------------
-- Console lines: server output streamed by the runtime node
-- ---------------------------------------------------------------------------
create table if not exists public.console_lines (
    id bigint generated always as identity primary key,
    server_id uuid not null references public.server_instances(id) on delete cascade,
    line_number bigint not null default 0,
    timestamp timestamptz not null default now(),
    level text not null default 'info' check (level in ('info', 'warn', 'error', 'command', 'system')),
    source text not null default 'server' check (source in ('server', 'plugin', 'command', 'system')),
    content text not null default '',
    raw text not null default ''
);

alter table public.console_lines enable row level security;

-- Users can read console lines for their own servers
create policy "instance owners read console"
on public.console_lines for select to authenticated
using (
    exists (
        select 1 from public.server_instances i
        where i.id = console_lines.server_id and i.user_id = auth.uid()
    )
);

-- Only the node that owns the server can insert console lines
create policy "node writes console"
on public.console_lines for insert to authenticated
with check (
    exists (
        select 1 from public.server_instances i
        join public.hosting_nodes n on n.id = i.node_id
        where i.id = console_lines.server_id and n.owner_id = auth.uid()
    )
);

create index if not exists console_lines_server_idx on public.console_lines(server_id, line_number);
create index if not exists console_lines_timestamp_idx on public.console_lines(server_id, timestamp);

-- ---------------------------------------------------------------------------
-- Runtime telemetry: periodic resource usage snapshots
-- ---------------------------------------------------------------------------
create table if not exists public.runtime_telemetry (
    id bigint generated always as identity primary key,
    server_id uuid not null references public.server_instances(id) on delete cascade,
    node_id uuid not null references public.hosting_nodes(id) on delete cascade,
    recorded_at timestamptz not null default now(),

    -- Server metrics
    cpu_pct real not null default 0,
    memory_used_mb bigint not null default 0,
    memory_max_mb bigint not null default 0,
    memory_pct real not null default 0,
    disk_used_mb bigint not null default 0,
    thread_count integer not null default 0,
    heap_used_mb bigint not null default 0,
    heap_max_mb bigint not null default 0,
    gc_count bigint not null default 0,
    gc_time_ms bigint not null default 0,
    tps real not null default 20.0,
    mspt real not null default 0,
    entity_count integer not null default 0,
    player_count integer not null default 0,
    chunk_count integer not null default 0,
    uptime_ms bigint not null default 0,

    -- Extra metadata (tick rate, world size, etc.)
    metadata jsonb not null default '{}'::jsonb
);

alter table public.runtime_telemetry enable row level security;

create policy "instance owners read telemetry"
on public.runtime_telemetry for select to authenticated
using (
    exists (
        select 1 from public.server_instances i
        where i.id = runtime_telemetry.server_id and i.user_id = auth.uid()
    )
);

create policy "node writes telemetry"
on public.runtime_telemetry for insert to authenticated
with check (
    exists (
        select 1 from public.hosting_nodes n
        where n.id = runtime_telemetry.node_id and n.owner_id = auth.uid()
    )
);

create index if not exists telemetry_server_idx on public.runtime_telemetry(server_id, recorded_at);
create index if not exists telemetry_node_idx on public.runtime_telemetry(node_id, recorded_at);

-- ---------------------------------------------------------------------------
-- Extend the existing 'servers' table with node assignment columns
-- ---------------------------------------------------------------------------
alter table public.servers
    add column if not exists node_id uuid references public.hosting_nodes(id) on delete set null,
    add column if not exists instance_id uuid references public.server_instances(id) on delete set null,
    add column if not exists template_id uuid references public.hosting_templates(id) on delete set null,
    add column if not exists status text not null default 'stopped' check (status in (
        'stopped', 'starting', 'running', 'stopping', 'restarting', 'error', 'maintenance'
    ));

-- ---------------------------------------------------------------------------
-- RPCs: Node registration, heartbeat, operation claiming
-- ---------------------------------------------------------------------------

-- Register a hosting node (called by the runtime agent with service key)
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
begin
    -- Check if a node with this secret already exists (re-registration)
    select * into existing_node
    from public.hosting_nodes
    where node_secret = p_node_secret;

    if existing_node.id is not null then
        -- Re-register: update fields, keep id
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

    -- New registration
    insert into public.hosting_nodes (
        owner_id, name, region, host, node_secret, tags,
        cpu_model, cpu_cores, memory_total_mb, storage_total_gb,
        agent_version, status, last_heartbeat_at
    ) values (
        auth.uid(), p_name, p_region, p_host, p_node_secret, p_tags,
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

-- Node heartbeat: update resource usage and status
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
begin
    -- Verify node credentials
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and node_secret = p_node_secret
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

-- Claim a pending operation
create or replace function public.claim_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare op public.server_operations;
begin
    -- Verify node
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and node_secret = p_node_secret
    ) then
        raise exception 'invalid node credentials';
    end if;

    -- Atomically claim: only claim if still pending
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

-- Start running an operation
create or replace function public.start_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and node_secret = p_node_secret
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations
    set status = 'running', started_at = now()
    where id = p_operation_id and node_id = p_node_id and status = 'claimed';

    return jsonb_build_object('success', true);
end;
$$;

-- Complete an operation
create or replace function public.complete_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_success boolean default true,
    p_result jsonb default '{}'::jsonb,
    p_error_message text default ''
)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and node_secret = p_node_secret
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

-- Update server instance status (called by the runtime agent)
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
begin
    -- Verify node owns this server
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.node_secret = p_node_secret
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

-- Write a batch of console lines
create or replace function public.write_console_lines(
    p_server_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_lines jsonb  -- array of {line_number, level, source, content, raw}
)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.node_secret = p_node_secret
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

-- Write a telemetry snapshot
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
begin
    if not exists (
        select 1 from public.hosting_nodes n
        join public.server_instances i on i.node_id = n.id
        where n.id = p_node_id and n.node_secret = p_node_secret
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

-- Assign a server to a node
create or replace function public.assign_server_to_node(
    p_server_id uuid,
    p_node_id uuid,
    p_user_id uuid
)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    -- Verify user owns both the server and the node
    if not exists (
        select 1 from public.server_instances
        where id = p_server_id and user_id = p_user_id
    ) then
        raise exception 'server not found or not owned by user';
    end if;
    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and owner_id = p_user_id
    ) then
        raise exception 'node not found or not owned by user';
    end if;

    update public.server_instances
    set node_id = p_node_id, updated_at = now()
    where id = p_server_id;

    return jsonb_build_object('success', true);
end;
$$;

-- Create a server instance from a template
create or replace function public.create_server_from_template(
    p_template_id uuid,
    p_node_id uuid,
    p_name text,
    p_user_id uuid,
    p_alias text default '',
    p_port integer default 25565
)
returns jsonb language plpgsql security definer set search_path = public as $$
declare
    tpl public.hosting_templates;
    new_id uuid;
    computed_address text;
begin
    select * into tpl from public.hosting_templates where id = p_template_id;
    if tpl.id is null then raise exception 'template not found'; end if;

    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and owner_id = p_user_id
    ) then
        raise exception 'node not found or not owned by user';
    end if;

    computed_address := case when p_alias = '' then
        lower(replace(left(p_name, 24), ' ', '-'))
    else lower(p_alias) end;

    insert into public.server_instances (
        user_id, node_id, template_id, name, alias, address,
        host, port, minecraft_version, loader, loader_version,
        max_players, memory_mb, auto_restart
    ) values (
        p_user_id, p_node_id, p_template_id, p_name, p_alias, computed_address,
        '0.0.0.0', p_port, tpl.minecraft_version, tpl.loader, tpl.loader_version,
        tpl.max_players, tpl.max_memory_mb, tpl.auto_restart
    ) returning id into new_id;

    -- Update template use count
    update public.hosting_templates set use_count = use_count + 1 where id = p_template_id;

    return jsonb_build_object('id', new_id, 'address', computed_address);
end;
$$;

-- Revoke and grant RPCs to authenticated users
revoke all on function public.register_hosting_node(text,text,text,text,text,integer,bigint,bigint,text,text[]) from public;
revoke all on function public.node_heartbeat(uuid,text,text,real,bigint,bigint,integer) from public;
revoke all on function public.claim_operation(uuid,uuid,text) from public;
revoke all on function public.start_operation(uuid,uuid,text) from public;
revoke all on function public.complete_operation(uuid,uuid,text,boolean,jsonb,text) from public;
revoke all on function public.update_server_instance(uuid,uuid,text,text,integer,integer,real,bigint,jsonb) from public;
revoke all on function public.write_console_lines(uuid,uuid,text,jsonb) from public;
revoke all on function public.write_telemetry(uuid,uuid,text,real,bigint,bigint,real,bigint,integer,bigint,bigint,bigint,bigint,real,real,integer,integer,integer,bigint,jsonb) from public;
revoke all on function public.assign_server_to_node(uuid,uuid,uuid) from public;
revoke all on function public.create_server_from_template(uuid,uuid,text,uuid,text,integer) from public;

grant execute on function public.register_hosting_node(text,text,text,text,text,integer,bigint,bigint,text,text[]) to authenticated;
grant execute on function public.node_heartbeat(uuid,text,text,real,bigint,bigint,integer) to authenticated;
grant execute on function public.claim_operation(uuid,uuid,text) to authenticated;
grant execute on function public.start_operation(uuid,uuid,text) to authenticated;
grant execute on function public.complete_operation(uuid,uuid,text,boolean,jsonb,text) to authenticated;
grant execute on function public.update_server_instance(uuid,uuid,text,text,integer,integer,real,bigint,jsonb) to authenticated;
grant execute on function public.write_console_lines(uuid,uuid,text,jsonb) to authenticated;
grant execute on function public.write_telemetry(uuid,uuid,text,real,bigint,bigint,real,bigint,integer,bigint,bigint,bigint,bigint,real,real,integer,integer,integer,bigint,jsonb) to authenticated;
grant execute on function public.assign_server_to_node(uuid,uuid,uuid) to authenticated;
grant execute on function public.create_server_from_template(uuid,uuid,text,uuid,text,integer) to authenticated;
