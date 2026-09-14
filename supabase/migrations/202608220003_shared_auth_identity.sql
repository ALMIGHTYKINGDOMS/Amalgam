-- Shared identity bootstrap for the launcher and website.
--
-- Both clients must authenticate against the same Supabase Auth project. This
-- trigger creates the shared social/account profile from Auth metadata so a
-- signup from either client has the same user-owned identity.

create or replace function public.bootstrap_shared_user_profile()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
    insert into public.public_profiles (
        user_id,
        display_name,
        avatar_url,
        minecraft_username
    ) values (
        new.id,
        left(coalesce(
            new.raw_user_meta_data->>'display_name',
            new.raw_user_meta_data->>'username',
            split_part(coalesce(new.email, ''), '@', 1),
            ''
        ), 64),
        left(coalesce(new.raw_user_meta_data->>'avatar_url', ''), 2048),
        left(coalesce(new.raw_user_meta_data->>'minecraft_username', ''), 64)
    )
    on conflict (user_id) do nothing;

    return new;
end;
$$;

revoke all on function public.bootstrap_shared_user_profile() from public, anon, authenticated;

drop trigger if exists on_auth_user_created_bootstrap_profile on auth.users;
create trigger on_auth_user_created_bootstrap_profile
    after insert on auth.users
    for each row execute function public.bootstrap_shared_user_profile();

-- Backfill only missing rows; existing profile data is never overwritten.
insert into public.public_profiles (user_id, display_name, avatar_url, minecraft_username)
select
    u.id,
    left(coalesce(
        u.raw_user_meta_data->>'display_name',
        u.raw_user_meta_data->>'username',
        split_part(coalesce(u.email, ''), '@', 1),
        ''
    ), 64),
    left(coalesce(u.raw_user_meta_data->>'avatar_url', ''), 2048),
    left(coalesce(u.raw_user_meta_data->>'minecraft_username', ''), 64)
from auth.users u
where not exists (
    select 1 from public.public_profiles p where p.user_id = u.id
);
