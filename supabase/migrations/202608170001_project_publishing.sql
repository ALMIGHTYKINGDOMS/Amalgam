-- Amalgam first-party project publishing.
-- Apply this migration in the Supabase SQL editor before enabling the creator UI.

create table if not exists public.staff_roles (
    user_id uuid primary key references auth.users(id) on delete cascade,
    role text not null check (role in ('moderator', 'administrator')),
    created_at timestamptz not null default now()
);

create table if not exists public.modpack_metadata (
    id text primary key,
    owner_id uuid not null default auth.uid() references auth.users(id) on delete cascade,
    name text not null default '',
    version text not null default '',
    minecraft_version text not null default '',
    loader text not null default '',
    loader_version text not null default '',
    author text not null default '',
    description text not null default '',
    size_bytes bigint not null default 0,
    sha1 text not null default '',
    downloads integer not null default 0,
    rating numeric not null default 0,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);
alter table public.modpack_metadata enable row level security;
create policy "owners manage modpack metadata" on public.modpack_metadata
for all using (owner_id = auth.uid()) with check (owner_id = auth.uid());

create table if not exists public.projects (
    id uuid primary key default gen_random_uuid(),
    owner_id uuid not null references auth.users(id) on delete cascade,
    slug text not null unique check (slug ~ '^[a-z0-9][a-z0-9-]{2,63}$'),
    name text not null check (char_length(name) between 3 and 100),
    description text not null default '',
    project_type text not null check (project_type in ('mod', 'modpack', 'resourcepack', 'shader', 'datapack', 'addon')),
    license text not null default 'All Rights Reserved',
    tags text[] not null default '{}',
    status text not null default 'draft' check (status in ('draft', 'pending_review', 'approved', 'rejected', 'unpublished')),
    approved_at timestamptz,
    approved_by uuid references auth.users(id),
    approval_reason text,
    current_version_id uuid,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

create table if not exists public.project_versions (
    id uuid primary key default gen_random_uuid(),
    project_id uuid not null references public.projects(id) on delete cascade,
    version text not null check (char_length(version) between 1 and 64),
    changelog text not null default '',
    game_versions text[] not null default '{}',
    loaders text[] not null default '{}',
    artifact_path text not null,
    artifact_sha256 text not null,
    artifact_size bigint not null check (artifact_size > 0),
    status text not null default 'draft' check (status in ('draft', 'published', 'withdrawn')),
    created_by uuid not null references auth.users(id),
    created_at timestamptz not null default now(),
    published_at timestamptz,
    unique(project_id, version)
);

alter table public.projects
    drop constraint if exists projects_current_version_fk;
alter table public.projects
    add constraint projects_current_version_fk
    foreign key (current_version_id) references public.project_versions(id) on delete set null;

create table if not exists public.project_media (
    id uuid primary key default gen_random_uuid(),
    project_id uuid not null references public.projects(id) on delete cascade,
    version_id uuid references public.project_versions(id) on delete cascade,
    kind text not null check (kind in ('icon', 'banner', 'gallery', 'video')),
    storage_path text not null unique,
    mime_type text not null,
    size_bytes bigint not null check (size_bytes > 0),
    sha256 text not null,
    title text not null default '',
    sort_order integer not null default 0,
    created_at timestamptz not null default now()
);

create table if not exists public.moderation_actions (
    id uuid primary key default gen_random_uuid(),
    project_id uuid not null references public.projects(id) on delete cascade,
    version_id uuid references public.project_versions(id) on delete set null,
    staff_id uuid not null references auth.users(id),
    action text not null check (action in ('submitted', 'approved', 'rejected', 'takedown', 'restored')),
    reason text not null default '',
    created_at timestamptz not null default now()
);

create index if not exists projects_status_idx on public.projects(status);
create index if not exists projects_owner_idx on public.projects(owner_id);
create index if not exists project_versions_project_idx on public.project_versions(project_id);
create index if not exists project_media_project_idx on public.project_media(project_id);

insert into storage.buckets (id, name, public)
values ('project-artifacts', 'project-artifacts', false),
       ('project-media', 'project-media', false)
on conflict (id) do nothing;

create policy "project owners upload artifacts"
on storage.objects for insert to authenticated
with check (
    bucket_id = 'project-artifacts' and
    exists (select 1 from public.projects p
            where p.id::text = (storage.foldername(name))[2]
              and p.owner_id = auth.uid())
);

create policy "project owners upload media"
on storage.objects for insert to authenticated
with check (
    bucket_id = 'project-media' and
    exists (select 1 from public.projects p
            where p.id::text = (storage.foldername(name))[2]
              and p.owner_id = auth.uid())
);

create policy "public reads approved media"
on storage.objects for select to public
using (
    bucket_id = 'project-media' and
    exists (select 1 from public.projects p
            where p.id::text = (storage.foldername(name))[2]
              and p.status = 'approved')
);

create or replace function public.is_project_staff()
returns boolean
language sql
stable
security definer
set search_path = public
as $$
    select exists (
        select 1 from public.staff_roles
        where user_id = auth.uid()
    );
$$;

alter table public.staff_roles enable row level security;
alter table public.projects enable row level security;
alter table public.project_versions enable row level security;
alter table public.project_media enable row level security;
alter table public.moderation_actions enable row level security;

create policy "public reads approved projects"
on public.projects for select using (status = 'approved' or owner_id = auth.uid() or public.is_project_staff());

create policy "owners create projects"
on public.projects for insert with check (owner_id = auth.uid());

create policy "owners edit projects"
on public.projects for update using (owner_id = auth.uid() or public.is_project_staff())
with check (owner_id = auth.uid() or public.is_project_staff());

create policy "public reads published versions"
on public.project_versions for select using (
    status = 'published' or created_by = auth.uid() or public.is_project_staff()
);

create policy "owners create versions"
on public.project_versions for insert with check (created_by = auth.uid());

create policy "owners edit versions"
on public.project_versions for update using (created_by = auth.uid() or public.is_project_staff())
with check (created_by = auth.uid() or public.is_project_staff());

create or replace function public.guard_project_workflow_fields()
returns trigger
language plpgsql
as $$
begin
    if auth.uid() = old.owner_id and not public.is_project_staff()
       and current_setting('amalgam.project_workflow', true) <> 'internal' then
        if new.status is distinct from old.status
           or new.approved_at is distinct from old.approved_at
           or new.approved_by is distinct from old.approved_by
           or new.current_version_id is distinct from old.current_version_id then
            raise exception 'project workflow fields are managed by publishing functions';
        end if;
    end if;
    return new;
end;
$$;

create or replace function public.guard_version_workflow_fields()
returns trigger
language plpgsql
as $$
begin
    if auth.uid() = old.created_by and not public.is_project_staff()
       and current_setting('amalgam.project_workflow', true) <> 'internal'
       and (new.status is distinct from old.status or new.published_at is distinct from old.published_at) then
        raise exception 'version workflow fields are managed by publishing functions';
    end if;
    return new;
end;
$$;

drop trigger if exists project_workflow_guard on public.projects;
create trigger project_workflow_guard
before update on public.projects
for each row execute function public.guard_project_workflow_fields();

drop trigger if exists version_workflow_guard on public.project_versions;
create trigger version_workflow_guard
before update on public.project_versions
for each row execute function public.guard_version_workflow_fields();

create policy "public reads project media"
on public.project_media for select using (
    exists (select 1 from public.projects p where p.id = project_id and
            (p.status = 'approved' or p.owner_id = auth.uid() or public.is_project_staff()))
);

create policy "owners create project media"
on public.project_media for insert with check (
    exists (select 1 from public.projects p where p.id = project_id and p.owner_id = auth.uid())
);

create policy "owners manage project media"
on public.project_media for update using (
    exists (select 1 from public.projects p where p.id = project_id and (p.owner_id = auth.uid() or public.is_project_staff()))
);

create policy "staff read moderation actions"
on public.moderation_actions for select using (public.is_project_staff() or staff_id = auth.uid());

create policy "owners record submission actions"
on public.moderation_actions for insert with check (
    staff_id = auth.uid() and
    exists (select 1 from public.projects p where p.id = project_id and p.owner_id = auth.uid())
);

create or replace function public.submit_project_for_review(p_project_id uuid)
returns public.projects
language plpgsql
security invoker
set search_path = public
as $$
declare result public.projects;
begin
    perform set_config('amalgam.project_workflow', 'internal', true);
    update public.projects
    set status = 'pending_review', updated_at = now()
    where id = p_project_id and owner_id = auth.uid() and status in ('draft', 'rejected')
    returning * into result;
    if result.id is null then raise exception 'project is not owned or cannot be submitted'; end if;
    insert into public.moderation_actions(project_id, staff_id, action)
    values (p_project_id, auth.uid(), 'submitted');
    return result;
end;
$$;

create or replace function public.review_project(p_project_id uuid, p_decision text, p_reason text default '')
returns public.projects
language plpgsql
security definer
set search_path = public
as $$
declare result public.projects;
begin
    if not public.is_project_staff() then raise exception 'staff role required'; end if;
    perform set_config('amalgam.project_workflow', 'internal', true);
    if p_decision not in ('approve', 'reject', 'takedown', 'restore') then raise exception 'invalid moderation decision'; end if;
    update public.projects
    set status = case p_decision when 'approve' then 'approved' when 'reject' then 'rejected'
                                  when 'takedown' then 'unpublished' else 'approved' end,
        approved_at = case when p_decision = 'approve' then coalesce(approved_at, now()) else approved_at end,
        approved_by = case when p_decision = 'approve' then coalesce(approved_by, auth.uid()) else approved_by end,
        approval_reason = p_reason, updated_at = now()
    where id = p_project_id
    returning * into result;
    if result.id is null then raise exception 'project not found'; end if;
    insert into public.moderation_actions(project_id, staff_id, action, reason)
    values (p_project_id, auth.uid(),
            case p_decision when 'approve' then 'approved' when 'reject' then 'rejected'
                             when 'takedown' then 'takedown' else 'restored' end, p_reason);
    return result;
end;
$$;

create or replace function public.publish_project_version(p_project_id uuid, p_version_id uuid)
returns public.project_versions
language plpgsql
security invoker
set search_path = public
as $$
declare result public.project_versions;
begin
    perform set_config('amalgam.project_workflow', 'internal', true);
    if not exists (select 1 from public.projects where id = p_project_id and owner_id = auth.uid() and approved_at is not null and status = 'approved') then
        raise exception 'project must be approved before versions can be published';
    end if;
    update public.project_versions
    set status = 'published', published_at = coalesce(published_at, now())
    where id = p_version_id and project_id = p_project_id and created_by = auth.uid()
    returning * into result;
    if result.id is null then raise exception 'version is not owned or does not belong to project'; end if;
    update public.projects set current_version_id = result.id, updated_at = now() where id = p_project_id;
    return result;
end;
$$;
