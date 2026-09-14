create table if not exists public.blocked_users (
    user_id uuid not null references auth.users(id) on delete cascade,
    blocked_user_id uuid not null references auth.users(id) on delete cascade,
    created_at timestamptz not null default now(),
    primary key (user_id, blocked_user_id),
    check (user_id <> blocked_user_id)
);

alter table public.blocked_users enable row level security;
create policy "users view own blocks" on public.blocked_users
    for select to authenticated using (user_id = auth.uid());

create or replace function public.block_user(p_blocked_user_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    if not exists (select 1 from auth.users where id = p_blocked_user_id) then
        raise exception 'user not found';
    end if;
    insert into public.blocked_users(user_id, blocked_user_id)
    values (auth.uid(), p_blocked_user_id)
    on conflict do nothing;
    return jsonb_build_object('success', true, 'blocked_user_id', p_blocked_user_id);
end;
$$;

create or replace function public.unblock_user(p_blocked_user_id uuid)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if auth.uid() is null then raise exception 'authentication required'; end if;
    delete from public.blocked_users b
     where b.user_id = auth.uid() and b.blocked_user_id = p_blocked_user_id;
    return jsonb_build_object('success', true, 'blocked_user_id', p_blocked_user_id);
end;
$$;

create or replace function public.get_blocked_users()
returns jsonb language sql security definer set search_path = public as $$
    select coalesce(jsonb_agg(to_jsonb(blocked_user_id) order by created_at), '[]'::jsonb)
      from public.blocked_users
     where user_id = auth.uid();
$$;

revoke all on function public.block_user(uuid) from public;
revoke all on function public.unblock_user(uuid) from public;
revoke all on function public.get_blocked_users() from public;
grant execute on function public.block_user(uuid) to authenticated;
grant execute on function public.unblock_user(uuid) to authenticated;
grant execute on function public.get_blocked_users() to authenticated;
