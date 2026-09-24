-- 202609240001 — Restrict privileged RPG credit RPCs.
--
-- The launcher may create an editor job for the signed-in user, but credit
-- grants are backend-only.  Trigger helpers are not public RPC endpoints.
-- Keep the public API least-privilege and bind user-supplied identity to the
-- authenticated Supabase session.

create or replace function public.create_rpg_survival_editor_job(
  p_user_id uuid,
  p_kind text,
  p_title text,
  p_prompt text,
  p_credit_cost integer,
  p_reference_key text,
  p_payload jsonb default '{}'::jsonb
)
returns table(job_id uuid, balance integer)
language plpgsql
security definer
set search_path = pg_catalog, public, auth, pg_temp
as $$
declare
  v_server_id uuid;
  v_wallet_id uuid;
  v_balance integer;
  v_job_id uuid;
begin
  if auth.uid() is null or p_user_id is distinct from auth.uid() then
    raise exception 'user identity does not match authenticated session';
  end if;

  if p_kind not in ('armor', 'item', 'boss', 'other') then
    raise exception 'Unsupported editor job kind';
  end if;
  if p_credit_cost is null or p_credit_cost <= 0 then
    raise exception 'Credit cost must be positive';
  end if;

  select id into v_server_id
  from public.network_servers
  where slug = 'rpg-survival' and status <> 'retired'
  limit 1;

  if v_server_id is null then
    raise exception 'RPG Survival server is not registered';
  end if;

  insert into public.network_credit_wallets (user_id, server_id)
  values (p_user_id, v_server_id)
  on conflict (user_id, server_id) do nothing;

  select id, network_credit_wallets.balance
    into v_wallet_id, v_balance
  from public.network_credit_wallets
  where user_id = p_user_id and server_id = v_server_id
  for update;

  if v_balance < p_credit_cost then
    raise exception 'Not enough RPG Survival credits';
  end if;

  if exists (
    select 1 from public.rpg_survival_editor_jobs
    where runtime_job_key = p_reference_key
  ) then
    select id into v_job_id
    from public.rpg_survival_editor_jobs
    where runtime_job_key = p_reference_key
    limit 1;
    return query select v_job_id, v_balance;
    return;
  end if;

  v_balance := v_balance - p_credit_cost;
  update public.network_credit_wallets
  set balance = v_balance, updated_at = now()
  where id = v_wallet_id;

  insert into public.network_credit_ledger
    (wallet_id, user_id, server_id, direction, amount, balance_after,
     reference_key, source, metadata)
  values
    (v_wallet_id, p_user_id, v_server_id, 'spend', p_credit_cost,
     v_balance, p_reference_key, 'rpg_survival_editor',
     jsonb_build_object('kind', p_kind));

  insert into public.rpg_survival_editor_jobs
    (user_id, server_id, kind, title, prompt, payload, credit_cost,
     runtime_job_key)
  values
    (p_user_id, v_server_id, p_kind, p_title, p_prompt,
     coalesce(p_payload, '{}'::jsonb), p_credit_cost, p_reference_key)
  returning id into v_job_id;

  return query select v_job_id, v_balance;
end;
$$;

-- A caller can spend only their own credits.  Credit grants are backend-only.
revoke all on function public.create_rpg_survival_editor_job(
  uuid, text, text, text, integer, text, jsonb
) from public, anon;
grant execute on function public.create_rpg_survival_editor_job(
  uuid, text, text, text, integer, text, jsonb
) to authenticated;

revoke all on function public.grant_network_credits(
  uuid, text, integer, text, text, jsonb
) from public, anon, authenticated;
grant execute on function public.grant_network_credits(
  uuid, text, integer, text, text, jsonb
) to service_role;

revoke all on function public.default_rpg_survival_plugin_route() from
  public, anon, authenticated;
grant execute on function public.default_rpg_survival_plugin_route() to
  service_role;
