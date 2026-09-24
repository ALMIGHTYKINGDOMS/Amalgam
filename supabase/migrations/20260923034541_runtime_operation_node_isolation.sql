-- Runtime operation routing isolation.
--
-- A server operation is executable only by the node currently assigned to its
-- server instance.  Earlier migrations allowed a valid node to claim any
-- pending row and overwrite its node_id.  This migration keeps the existing
-- RPC signatures while making that routing relationship immutable for active
-- work and derived from server_instances for pending work.

-- Bring queued legacy rows into the new invariant before the enforcing
-- triggers are installed. Pending work can safely follow its server's current
-- assignment. Active work that is already assigned to a different (or null)
-- node must fail closed rather than be silently moved between workers.
update public.server_operations as o
set node_id = i.node_id
from public.server_instances as i
where i.id = o.server_id
  and o.status = 'pending'
  and o.node_id is distinct from i.node_id;

update public.server_operations as o
set status = 'failed',
    error_message = case
      when coalesce(o.error_message, '') = '' then
        'Operation routing no longer matches the assigned server node; retry the operation.'
      else
        left(o.error_message || ' | operation routing no longer matches the assigned server node', 4000)
    end,
    completed_at = coalesce(o.completed_at, now())
from public.server_instances as i
where i.id = o.server_id
  and o.status in ('claimed', 'running')
  and o.node_id is distinct from i.node_id;

-- Index the exact agent polling predicate. This is intentionally partial so
-- terminal history does not increase queue-scan cost.
create index if not exists server_operations_pending_node_created_idx
    on public.server_operations (node_id, created_at)
    where status = 'pending';

-- The operation's node is server-derived, not a client-selected routing hint.
-- The trigger also prevents an active operation from changing either its
-- server or node identity.
create or replace function public.bind_server_operation_node()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
    v_server_node_id uuid;
begin
    select i.node_id
      into v_server_node_id
      from public.server_instances as i
     where i.id = new.server_id
       -- An operation insert/update and a server reassignment must serialize.
       -- FOR SHARE conflicts with the NO KEY UPDATE lock used by the
       -- reassignment, so a new pending row cannot be left with the old node.
     for share;

    if v_server_node_id is null then
        raise exception 'server instance is not assigned to a runtime node';
    end if;

    if tg_op = 'UPDATE'
       and old.status in ('claimed', 'running')
       and (
           new.server_id is distinct from old.server_id
           or new.node_id is distinct from old.node_id
       ) then
        raise exception 'cannot reroute an active server operation';
    end if;

    new.node_id := v_server_node_id;
    return new;
end;
$$;

drop trigger if exists trg_bind_server_operation_node on public.server_operations;
create trigger trg_bind_server_operation_node
before insert or update of server_id, node_id on public.server_operations
for each row execute function public.bind_server_operation_node();

-- A reassignment may re-route pending work, but cannot race an already claimed
-- or running operation. This runs AFTER the server row changes so the
-- operation-binding trigger observes the new node while it updates pending
-- rows. Raising from an AFTER trigger still rolls the reassignment back.
-- Keeping the pending rows in sync lets agents use an exact node_id filter
-- without depending on nullable legacy routing data.
create or replace function public.rebind_pending_operations_for_server_node()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
    if new.node_id is distinct from old.node_id then
        if exists (
            select 1
              from public.server_operations as o
             where o.server_id = new.id
               and o.status in ('claimed', 'running')
        ) then
            raise exception 'cannot reassign a server while it has an active operation';
        end if;

        update public.server_operations
           set node_id = new.node_id
         where server_id = new.id
           and status = 'pending';
    end if;
    return new;
end;
$$;

drop trigger if exists trg_rebind_pending_operations_for_server_node on public.server_instances;
create trigger trg_rebind_pending_operations_for_server_node
after update of node_id on public.server_instances
for each row execute function public.rebind_pending_operations_for_server_node();

-- Claiming, starting, and completing all prove the same three-way invariant:
-- valid node credentials, operation.node_id = caller node, and
-- server_instances.node_id = caller node. The UPDATE predicate is atomic, so
-- two nodes racing for one operation cannot both obtain a claim.
create or replace function public.claim_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
    v_operation public.server_operations;
    v_server_node_id uuid;
begin
    if not exists (
        select 1
          from public.hosting_nodes as n
         where n.id = p_node_id
           and n.hashed_secret = pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex')
    ) then
        raise exception 'invalid node credentials';
    end if;

    -- Lock the assigned server before claiming its operation.  This serializes
    -- a claim with server_instances.node_id reassignment: once a claim wins,
    -- the reassignment trigger sees an active operation and aborts rather than
    -- stranding it on the old node.
    select i.node_id
      into v_server_node_id
      from public.server_operations as o
      join public.server_instances as i on i.id = o.server_id
     where o.id = p_operation_id
       and o.status = 'pending'
       and o.node_id = p_node_id
       and i.node_id = p_node_id
     for update of i;

    if v_server_node_id is null then
        raise exception 'operation is not pending for this node and assigned server';
    end if;

    update public.server_operations as o
       set status = 'claimed',
           claimed_at = now()
      from public.server_instances as i
     where o.id = p_operation_id
       and o.status = 'pending'
       and o.node_id = p_node_id
       and i.id = o.server_id
       and i.node_id = p_node_id
    returning o.* into v_operation;

    if v_operation.id is null then
        raise exception 'operation was claimed concurrently';
    end if;

    return pg_catalog.to_jsonb(v_operation);
end;
$$;

create or replace function public.start_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
    v_operation public.server_operations;
begin
    if not exists (
        select 1
          from public.hosting_nodes as n
         where n.id = p_node_id
           and n.hashed_secret = pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex')
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations as o
       set status = 'running',
           started_at = now()
      from public.server_instances as i
     where o.id = p_operation_id
       and o.status = 'claimed'
       and o.node_id = p_node_id
       and i.id = o.server_id
       and i.node_id = p_node_id
    returning o.* into v_operation;

    if v_operation.id is null then
        raise exception 'operation is not claimed for this node and assigned server';
    end if;

    return pg_catalog.jsonb_build_object('success', true);
end;
$$;

create or replace function public.complete_operation(
    p_operation_id uuid,
    p_node_id uuid,
    p_node_secret text,
    p_success boolean default true,
    p_result jsonb default '{}'::jsonb,
    p_error_message text default ''
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
    v_operation public.server_operations;
begin
    if not exists (
        select 1
          from public.hosting_nodes as n
         where n.id = p_node_id
           and n.hashed_secret = pg_catalog.encode(extensions.digest(p_node_secret, 'sha256'), 'hex')
    ) then
        raise exception 'invalid node credentials';
    end if;

    update public.server_operations as o
       set status = case when p_success then 'completed' else 'failed' end,
           result = coalesce(p_result, '{}'::jsonb),
           error_message = left(coalesce(p_error_message, ''), 4000),
           completed_at = now()
      from public.server_instances as i
     where o.id = p_operation_id
       and o.status in ('claimed', 'running')
       and o.node_id = p_node_id
       and i.id = o.server_id
       and i.node_id = p_node_id
    returning o.* into v_operation;

    if v_operation.id is null then
        raise exception 'operation is not active for this node and assigned server';
    end if;

    return pg_catalog.jsonb_build_object('success', true);
end;
$$;

-- Trigger functions are internal integrity mechanisms, while the three RPCs
-- retain the deployed public signatures and intended caller roles.
revoke all on function public.bind_server_operation_node() from public;
revoke all on function public.rebind_pending_operations_for_server_node() from public;
revoke all on function public.claim_operation(uuid, uuid, text) from public;
revoke all on function public.start_operation(uuid, uuid, text) from public;
revoke all on function public.complete_operation(uuid, uuid, text, boolean, jsonb, text) from public;
grant execute on function public.claim_operation(uuid, uuid, text) to authenticated, service_role;
grant execute on function public.start_operation(uuid, uuid, text) to authenticated, service_role;
grant execute on function public.complete_operation(uuid, uuid, text, boolean, jsonb, text) to authenticated, service_role;
