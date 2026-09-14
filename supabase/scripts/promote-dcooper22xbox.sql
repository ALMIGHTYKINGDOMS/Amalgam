-- Run once in the Supabase SQL editor after the account has been created.
-- This grants server-side administrator access to the exact existing account.
insert into public.staff_roles (user_id, role)
select id, 'administrator'
from auth.users
where lower(email) = lower('dcooper22xbox@gmail.com')
on conflict (user_id) do update set role = excluded.role;

select public.staff_roles.user_id, auth.users.email, public.staff_roles.role
from public.staff_roles
join auth.users on auth.users.id = public.staff_roles.user_id
where lower(auth.users.email) = lower('dcooper22xbox@gmail.com');
