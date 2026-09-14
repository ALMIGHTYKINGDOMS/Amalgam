-- Staff-only directory backed by auth.users. PostgREST does not expose
-- auth.users as a normal table, so the admin client must use this RPC.
create or replace function public.admin_list_users()
returns setof jsonb
language sql
security definer
set search_path = public, auth
as $$
    select jsonb_build_object(
        'id', u.id,
        'email', u.email,
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

revoke all on function public.admin_list_users() from public;
grant execute on function public.admin_list_users() to authenticated;
