-- Authenticated SDP/ICE signaling only. Game bytes never enter Supabase.
create table if not exists public.essentials_signals (
    id uuid primary key default gen_random_uuid(),
    session_id text not null references public.essentials_sessions(id) on delete cascade,
    sender_id uuid not null references auth.users(id) on delete cascade,
    recipient_id uuid not null references auth.users(id) on delete cascade,
    kind text not null check (kind in ('hello', 'offer', 'answer', 'candidate', 'close')),
    payload text not null,
    created_at timestamptz not null default now()
);

create index if not exists essentials_signals_recipient_idx
    on public.essentials_signals(session_id, recipient_id, created_at);

alter table public.essentials_signals enable row level security;

create policy "session members send signals"
on public.essentials_signals for insert to authenticated
with check (
    sender_id = auth.uid() and exists (
        select 1 from public.essentials_session_members m
        where m.session_id = essentials_signals.session_id
          and m.user_id = auth.uid() and m.left_at is null
    )
);

create policy "signal recipients read signals"
on public.essentials_signals for select to authenticated
using (recipient_id = auth.uid() or sender_id = auth.uid());

create policy "signal participants delete signals"
on public.essentials_signals for delete to authenticated
using (recipient_id = auth.uid() or sender_id = auth.uid());

-- Replace the original manifest writer with one that persists the complete
-- mod list used by the TCP bridge compatibility check.
drop function if exists public.upsert_session_manifest(
    text, text, text, text, text, text, jsonb, jsonb);

create or replace function public.upsert_session_manifest(
    session_id text, profile_id text default '', profile_name text default '',
    minecraft_version text default '', loader text default '', loader_version text default '',
    required_resource_packs jsonb default '[]'::jsonb,
    required_configs jsonb default '{}'::jsonb,
    mods jsonb default '[]'::jsonb)
returns jsonb language plpgsql security definer set search_path = public as $$
begin
    if not exists (
        select 1 from essentials_sessions
        where id = upsert_session_manifest.session_id and host_user_id = auth.uid()
    ) then
        raise exception 'host session not found';
    end if;
    insert into essentials_manifests(session_id, host_user_id, profile_id, profile_name,
        minecraft_version, loader, loader_version, required_resource_packs, required_configs)
    values (session_id, auth.uid(), profile_id, profile_name, minecraft_version, loader,
        loader_version, required_resource_packs, required_configs)
    on conflict (session_id) do update set profile_id = excluded.profile_id,
        profile_name = excluded.profile_name, minecraft_version = excluded.minecraft_version,
        loader = excluded.loader, loader_version = excluded.loader_version,
        required_resource_packs = excluded.required_resource_packs,
        required_configs = excluded.required_configs, updated_at = now();
    delete from essentials_manifest_mods where essentials_manifest_mods.session_id = upsert_session_manifest.session_id;
    insert into essentials_manifest_mods(session_id, name, mod_id, version, hash, source, enabled)
    select upsert_session_manifest.session_id, item->>'name', coalesce(item->>'id', ''), coalesce(item->>'version', ''),
        coalesce(item->>'hash', ''), coalesce(item->>'source', ''), coalesce((item->>'enabled')::boolean, true)
    from jsonb_array_elements(mods) item;
    return jsonb_build_object('success', true, 'session_id', session_id);
end $$;
