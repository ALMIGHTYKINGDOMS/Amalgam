-- Bind user-facing SECURITY DEFINER RPCs to the authenticated caller and make
-- telemetry/audit writes flow through their intentionally scoped RPCs.  The
-- caller-facing signatures are retained so deployed clients do not need a
-- coordinated API change.
--
-- A service-role request remains a trusted server-side integration and may
-- supply an explicit user ID.  A normal authenticated request may only act as
-- auth.uid().  Do not distribute service-role credentials to launcher clients.

-- ---------------------------------------------------------------------------
-- Server management: never trust a user ID supplied by an authenticated client
-- ---------------------------------------------------------------------------
create or replace function public.assign_server_to_node(
    p_server_id uuid,
    p_node_id uuid,
    p_user_id uuid
)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_effective_user_id uuid;
begin
    if v_is_service_role then
        v_effective_user_id := p_user_id;
    else
        v_effective_user_id := auth.uid();
        if v_effective_user_id is null then
            raise exception 'authentication required';
        end if;
        if p_user_id is distinct from v_effective_user_id then
            raise exception 'user context does not match authenticated caller';
        end if;
    end if;

    if v_effective_user_id is null then
        raise exception 'user context is required';
    end if;

    if not exists (
        select 1 from public.server_instances
        where id = p_server_id and user_id = v_effective_user_id
    ) then
        raise exception 'server not found or not owned by user';
    end if;

    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and owner_id = v_effective_user_id
    ) then
        raise exception 'node not found or not owned by user';
    end if;

    update public.server_instances
    set node_id = p_node_id, updated_at = now()
    where id = p_server_id;

    return jsonb_build_object('success', true);
end;
$$;

create or replace function public.create_server_from_template(
    p_template_id uuid,
    p_node_id uuid,
    p_name text,
    p_user_id uuid,
    p_alias text default '',
    p_port integer default 25565
)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_effective_user_id uuid;
    tpl public.hosting_templates;
    new_id uuid;
    computed_address text;
begin
    if v_is_service_role then
        v_effective_user_id := p_user_id;
    else
        v_effective_user_id := auth.uid();
        if v_effective_user_id is null then
            raise exception 'authentication required';
        end if;
        if p_user_id is distinct from v_effective_user_id then
            raise exception 'user context does not match authenticated caller';
        end if;
    end if;

    if v_effective_user_id is null then
        raise exception 'user context is required';
    end if;
    if char_length(btrim(coalesce(p_name, ''))) = 0 or char_length(p_name) > 128 then
        raise exception 'server name must contain 1 to 128 characters';
    end if;
    if p_port is null or p_port < 1 or p_port > 65535 then
        raise exception 'server port must be between 1 and 65535';
    end if;

    -- SECURITY DEFINER bypasses RLS, so reproduce the intended visibility
    -- rule instead of allowing a caller to instantiate a private template.
    select * into tpl
    from public.hosting_templates
    where id = p_template_id
      and (is_public = true or owner_id = v_effective_user_id);
    if tpl.id is null then
        raise exception 'template not found or not available to user';
    end if;

    if not exists (
        select 1 from public.hosting_nodes
        where id = p_node_id and owner_id = v_effective_user_id
    ) then
        raise exception 'node not found or not owned by user';
    end if;

    computed_address := case when coalesce(p_alias, '') = '' then
        lower(replace(left(p_name, 24), ' ', '-'))
    else lower(p_alias) end;

    insert into public.server_instances (
        user_id, node_id, template_id, name, alias, address,
        host, port, minecraft_version, loader, loader_version,
        max_players, memory_mb, auto_restart
    ) values (
        v_effective_user_id, p_node_id, p_template_id, p_name, coalesce(p_alias, ''), computed_address,
        '0.0.0.0', p_port, tpl.minecraft_version, tpl.loader, tpl.loader_version,
        tpl.max_players, tpl.max_memory_mb, tpl.auto_restart
    ) returning id into new_id;

    update public.hosting_templates
    set use_count = use_count + 1
    where id = p_template_id;

    return jsonb_build_object('id', new_id, 'address', computed_address);
end;
$$;

-- ---------------------------------------------------------------------------
-- Telemetry: authenticated callers are always attributed to auth.uid().
-- Runtime events must name a node owned by that caller.  Trusted service-role
-- integrations may attribute a user explicitly, as before.
-- ---------------------------------------------------------------------------
create or replace function public.insert_events(p_events jsonb)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_actor_id uuid := auth.uid();
    v_supplied_user_id uuid;
    v_effective_user_id uuid;
    v_node_id uuid;
    v_source text;
    inserted_count integer := 0;
    item jsonb;
begin
    if not v_is_service_role and v_actor_id is null then
        raise exception 'authentication required';
    end if;
    if coalesce(jsonb_typeof(p_events), '') <> 'array' then
        raise exception 'events must be an array';
    end if;
    if jsonb_array_length(p_events) > 500 then
        raise exception 'events batch exceeds 500 items';
    end if;

    for item in select jsonb_array_elements(p_events)
    loop
        if jsonb_typeof(item) <> 'object' then
            raise exception 'each event must be an object';
        end if;
        v_supplied_user_id := nullif(item ->> 'user_id', '')::uuid;
        if not v_is_service_role and v_supplied_user_id is not null and v_supplied_user_id <> v_actor_id then
            raise exception 'event user_id does not match authenticated caller';
        end if;
        v_effective_user_id := case when v_is_service_role then v_supplied_user_id else v_actor_id end;
        v_node_id := nullif(item ->> 'source_node_id', '')::uuid;
        v_source := lower(coalesce(nullif(item ->> 'source', ''), 'launcher'));
        if v_source not in ('launcher', 'runtime', 'web', 'api', 'edge', 'mobile') then
            raise exception 'invalid event source';
        end if;
        if v_source = 'runtime' then
            if v_node_id is null then
                raise exception 'runtime events require source_node_id';
            end if;
            if not v_is_service_role and not exists (
                select 1 from public.hosting_nodes
                where id = v_node_id and owner_id = v_actor_id
            ) then
                raise exception 'runtime node is not owned by authenticated caller';
            end if;
        elsif v_node_id is not null then
            raise exception 'source_node_id is only valid for runtime events';
        end if;

        insert into public.events (
            source, source_version, source_node_id, user_id, session_id,
            category, event_name, properties, duration_ms, value,
            page, screen, user_agent
        ) values (
            v_source,
            left(coalesce(item ->> 'source_version', ''), 64),
            v_node_id,
            v_effective_user_id,
            left(coalesce(item ->> 'session_id', ''), 256),
            coalesce(item ->> 'category', 'generic'),
            left(coalesce(item ->> 'event_name', ''), 256),
            coalesce(item -> 'properties', '{}'::jsonb),
            (item ->> 'duration_ms')::bigint,
            (item ->> 'value')::numeric,
            left(coalesce(item ->> 'page', ''), 512),
            left(coalesce(item ->> 'screen', ''), 512),
            left(coalesce(item ->> 'user_agent', ''), 512)
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

create or replace function public.insert_metrics(p_metrics jsonb)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_actor_id uuid := auth.uid();
    v_supplied_user_id uuid;
    v_effective_user_id uuid;
    v_node_id uuid;
    v_source text;
    inserted_count integer := 0;
    item jsonb;
begin
    if not v_is_service_role and v_actor_id is null then
        raise exception 'authentication required';
    end if;
    if coalesce(jsonb_typeof(p_metrics), '') <> 'array' then
        raise exception 'metrics must be an array';
    end if;
    if jsonb_array_length(p_metrics) > 500 then
        raise exception 'metrics batch exceeds 500 items';
    end if;

    for item in select jsonb_array_elements(p_metrics)
    loop
        if jsonb_typeof(item) <> 'object' then
            raise exception 'each metric must be an object';
        end if;
        v_supplied_user_id := nullif(item ->> 'user_id', '')::uuid;
        if not v_is_service_role and v_supplied_user_id is not null and v_supplied_user_id <> v_actor_id then
            raise exception 'metric user_id does not match authenticated caller';
        end if;
        v_effective_user_id := case when v_is_service_role then v_supplied_user_id else v_actor_id end;
        v_node_id := nullif(item ->> 'source_node_id', '')::uuid;
        v_source := lower(coalesce(nullif(item ->> 'source', ''), 'launcher'));
        if v_source not in ('launcher', 'runtime', 'web', 'api') then
            raise exception 'invalid metric source';
        end if;
        if v_source = 'runtime' then
            if v_node_id is null then
                raise exception 'runtime metrics require source_node_id';
            end if;
            if not v_is_service_role and not exists (
                select 1 from public.hosting_nodes
                where id = v_node_id and owner_id = v_actor_id
            ) then
                raise exception 'runtime node is not owned by authenticated caller';
            end if;
        elsif v_node_id is not null then
            raise exception 'source_node_id is only valid for runtime metrics';
        end if;

        insert into public.metrics (
            source, source_node_id, user_id,
            metric_name, metric_type, value, unit, tags
        ) values (
            v_source,
            v_node_id,
            v_effective_user_id,
            left(coalesce(item ->> 'metric_name', ''), 256),
            coalesce(item ->> 'metric_type', 'gauge'),
            coalesce((item ->> 'value')::numeric, 0),
            left(coalesce(item ->> 'unit', ''), 64),
            coalesce(item -> 'tags', '{}'::jsonb)
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

create or replace function public.insert_errors(p_errors jsonb)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_actor_id uuid := auth.uid();
    v_supplied_user_id uuid;
    v_effective_user_id uuid;
    v_node_id uuid;
    v_source text;
    inserted_count integer := 0;
    item jsonb;
    fp text;
begin
    if not v_is_service_role and v_actor_id is null then
        raise exception 'authentication required';
    end if;
    if coalesce(jsonb_typeof(p_errors), '') <> 'array' then
        raise exception 'errors must be an array';
    end if;
    if jsonb_array_length(p_errors) > 500 then
        raise exception 'errors batch exceeds 500 items';
    end if;

    for item in select jsonb_array_elements(p_errors)
    loop
        if jsonb_typeof(item) <> 'object' then
            raise exception 'each error must be an object';
        end if;
        v_supplied_user_id := nullif(item ->> 'user_id', '')::uuid;
        if not v_is_service_role and v_supplied_user_id is not null and v_supplied_user_id <> v_actor_id then
            raise exception 'error user_id does not match authenticated caller';
        end if;
        v_effective_user_id := case when v_is_service_role then v_supplied_user_id else v_actor_id end;
        v_node_id := nullif(item ->> 'source_node_id', '')::uuid;
        v_source := lower(coalesce(nullif(item ->> 'source', ''), 'launcher'));
        if v_source not in ('launcher', 'runtime', 'web', 'api', 'edge') then
            raise exception 'invalid error source';
        end if;
        if v_source = 'runtime' then
            if v_node_id is null then
                raise exception 'runtime errors require source_node_id';
            end if;
            if not v_is_service_role and not exists (
                select 1 from public.hosting_nodes
                where id = v_node_id and owner_id = v_actor_id
            ) then
                raise exception 'runtime node is not owned by authenticated caller';
            end if;
        elsif v_node_id is not null then
            raise exception 'source_node_id is only valid for runtime errors';
        end if;

        fp := encode(
            extensions.digest(
                coalesce(item ->> 'error_type', '') || ':' ||
                coalesce(item ->> 'message', '') || ':' ||
                coalesce(item ->> 'component', ''),
                'sha256'
            ), 'hex'
        );

        insert into public.errors (
            source, source_version, source_node_id, user_id,
            error_type, error_code, severity,
            message, stack_trace, component,
            operation, properties, fingerprint
        ) values (
            v_source,
            left(coalesce(item ->> 'source_version', ''), 64),
            v_node_id,
            v_effective_user_id,
            left(coalesce(item ->> 'error_type', ''), 128),
            left(coalesce(item ->> 'error_code', ''), 128),
            coalesce(item ->> 'severity', 'error'),
            left(coalesce(item ->> 'message', ''), 4000),
            left(coalesce(item ->> 'stack_trace', ''), 16000),
            left(coalesce(item ->> 'component', ''), 128),
            left(coalesce(item ->> 'operation', ''), 128),
            coalesce(item -> 'properties', '{}'::jsonb),
            fp
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

create or replace function public.insert_feature_usage(p_usage jsonb)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_is_service_role boolean := coalesce(auth.jwt() ->> 'role', '') = 'service_role';
    v_actor_id uuid := auth.uid();
    v_supplied_user_id uuid;
    v_effective_user_id uuid;
    inserted_count integer := 0;
    item jsonb;
begin
    if not v_is_service_role and v_actor_id is null then
        raise exception 'authentication required';
    end if;
    if coalesce(jsonb_typeof(p_usage), '') <> 'array' then
        raise exception 'feature usage must be an array';
    end if;
    if jsonb_array_length(p_usage) > 500 then
        raise exception 'feature usage batch exceeds 500 items';
    end if;

    for item in select jsonb_array_elements(p_usage)
    loop
        if jsonb_typeof(item) <> 'object' then
            raise exception 'each feature usage item must be an object';
        end if;
        v_supplied_user_id := nullif(item ->> 'user_id', '')::uuid;
        if not v_is_service_role and v_supplied_user_id is not null and v_supplied_user_id <> v_actor_id then
            raise exception 'feature usage user_id does not match authenticated caller';
        end if;
        v_effective_user_id := case when v_is_service_role then v_supplied_user_id else v_actor_id end;

        insert into public.feature_usage (
            user_id, source,
            feature_name, feature_category,
            usage_count, duration_ms, success, properties
        ) values (
            v_effective_user_id,
            left(coalesce(item ->> 'source', 'launcher'), 64),
            left(coalesce(item ->> 'feature_name', ''), 128),
            left(coalesce(item ->> 'feature_category', ''), 128),
            greatest(1, coalesce((item ->> 'usage_count')::integer, 1)),
            (item ->> 'duration_ms')::bigint,
            coalesce((item ->> 'success')::boolean, true),
            coalesce(item -> 'properties', '{}'::jsonb)
        );
        inserted_count := inserted_count + 1;
    end loop;

    return jsonb_build_object('success', true, 'inserted', inserted_count);
end;
$$;

create or replace function public.record_audit(
    p_action text,
    p_resource_type text,
    p_resource_id text default '',
    p_changes jsonb default '{}'::jsonb,
    p_metadata jsonb default '{}'::jsonb,
    p_source text default 'launcher'
)
returns jsonb language plpgsql security definer set search_path = '' as $$
declare
    v_actor_id uuid := auth.uid();
begin
    if v_actor_id is null then
        raise exception 'authentication required';
    end if;

    insert into public.audit_log (
        actor_id, actor_email, actor_role,
        action, resource_type, resource_id,
        changes, metadata, source
    ) values (
        v_actor_id,
        coalesce((select email from auth.users where id = v_actor_id), ''),
        case when public.is_project_staff() then 'staff' else 'user' end,
        left(coalesce(p_action, ''), 128),
        left(coalesce(p_resource_type, ''), 128),
        left(coalesce(p_resource_id, ''), 256),
        coalesce(p_changes, '{}'::jsonb),
        coalesce(p_metadata, '{}'::jsonb),
        left(coalesce(p_source, 'launcher'), 64)
    );

    return jsonb_build_object('success', true);
end;
$$;

-- Direct client inserts have no reason to bypass the identity and runtime-node
-- checks above.  Reads remain governed by their existing row-scoped policies.
drop policy if exists "users insert own events" on public.events;
drop policy if exists "runtime nodes insert events" on public.events;
drop policy if exists "launcher insert metrics" on public.metrics;
drop policy if exists "users insert own errors" on public.errors;
drop policy if exists "runtime nodes insert errors" on public.errors;
drop policy if exists "users insert own feature usage" on public.feature_usage;
drop policy if exists "authenticated insert audit" on public.audit_log;
drop policy if exists "users insert own audit" on public.audit_log;

-- Re-state effective function access explicitly.  SECURITY DEFINER functions
-- begin executable by PUBLIC in PostgreSQL unless this is revoked.
revoke all on function public.assign_server_to_node(uuid,uuid,uuid) from public;
revoke all on function public.create_server_from_template(uuid,uuid,text,uuid,text,integer) from public;
revoke all on function public.insert_events(jsonb) from public;
revoke all on function public.insert_metrics(jsonb) from public;
revoke all on function public.insert_errors(jsonb) from public;
revoke all on function public.insert_feature_usage(jsonb) from public;
revoke all on function public.record_audit(text,text,text,jsonb,jsonb,text) from public;

grant execute on function public.assign_server_to_node(uuid,uuid,uuid) to authenticated;
grant execute on function public.create_server_from_template(uuid,uuid,text,uuid,text,integer) to authenticated;
grant execute on function public.insert_events(jsonb) to authenticated, service_role;
grant execute on function public.insert_metrics(jsonb) to authenticated, service_role;
grant execute on function public.insert_errors(jsonb) to authenticated, service_role;
grant execute on function public.insert_feature_usage(jsonb) to authenticated, service_role;
grant execute on function public.record_audit(text,text,text,jsonb,jsonb,text) to authenticated;
