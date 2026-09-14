-- Social system tables for Amalgam.
-- Friends, messaging, parties, presence, and public profiles.

-- ---------------------------------------------------------------------------
-- Friendships (bidirectional, one row per pair)
-- ---------------------------------------------------------------------------
create table if not exists public.friendships (
    id uuid primary key default gen_random_uuid(),
    user_id_a uuid not null references auth.users(id) on delete cascade,
    user_id_b uuid not null references auth.users(id) on delete cascade,
    status text not null default 'accepted' check (status in ('accepted', 'blocked')),
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    check (user_id_a < user_id_b)  -- canonical ordering: smaller UUID first
);

alter table public.friendships enable row level security;
create policy "users see own friendships"
on public.friendships for select to authenticated
using (user_id_a = auth.uid() or user_id_b = auth.uid());

create policy "system manages friendships"
on public.friendships for all to authenticated
using (true) with check (true);

create unique index if not exists friendships_pair_idx
    on public.friendships(user_id_a, user_id_b);
create index if not exists friendships_user_b_idx on public.friendships(user_id_b);

-- ---------------------------------------------------------------------------
-- Friend requests (pending invitations)
-- ---------------------------------------------------------------------------
create table if not exists public.friend_requests (
    id uuid primary key default gen_random_uuid(),
    sender_id uuid not null references auth.users(id) on delete cascade,
    receiver_id uuid not null references auth.users(id) on delete cascade,
    status text not null default 'pending' check (status in ('pending', 'accepted', 'rejected')),
    message text not null default '',
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    check (sender_id <> receiver_id)
);

alter table public.friend_requests enable row level security;
create policy "participants see own friend requests"
on public.friend_requests for select to authenticated
using (sender_id = auth.uid() or receiver_id = auth.uid());

create policy "senders create friend requests"
on public.friend_requests for insert to authenticated
with check (sender_id = auth.uid());

create policy "participants update friend requests"
on public.friend_requests for update to authenticated
using (sender_id = auth.uid() or receiver_id = auth.uid())
with check (sender_id = auth.uid() or receiver_id = auth.uid());

create index if not exists friend_requests_receiver_idx
    on public.friend_requests(receiver_id, status, created_at desc);
create index if not exists friend_requests_sender_idx
    on public.friend_requests(sender_id, status);

-- ---------------------------------------------------------------------------
-- Public profiles (user-searchable)
-- ---------------------------------------------------------------------------
create table if not exists public.public_profiles (
    user_id uuid primary key references auth.users(id) on delete cascade,
    display_name text not null default '',
    avatar_url text not null default '',
    bio text not null default '',
    minecraft_username text not null default '',
    is_public boolean not null default true,
    friend_count integer not null default 0,
    play_time_seconds bigint not null default 0,
    last_seen_at timestamptz,
    updated_at timestamptz not null default now()
);

alter table public.public_profiles enable row level security;
create policy "public profiles are readable"
on public.public_profiles for select to authenticated
using (is_public = true or user_id = auth.uid());

create policy "owners manage own public profile"
on public.public_profiles for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

-- Staff can read all profiles for admin directory
create policy "staff read all public profiles"
on public.public_profiles for select to authenticated
using (public.is_project_staff());

create index if not exists public_profiles_search_idx
    on public.public_profiles using gin(to_tsvector('simple', display_name || ' ' || minecraft_username));
create index if not exists public_profiles_last_seen_idx on public.public_profiles(last_seen_at desc);

-- ---------------------------------------------------------------------------
-- Conversations (DM threads)
-- ---------------------------------------------------------------------------
create table if not exists public.conversations (
    id uuid primary key default gen_random_uuid(),
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now(),
    last_message_content text not null default '',
    last_message_sender_id uuid references auth.users(id) on delete set null,
    last_message_at timestamptz
);

alter table public.conversations enable row level security;

-- ---------------------------------------------------------------------------
-- Conversation participants
-- ---------------------------------------------------------------------------
create table if not exists public.conversation_participants (
    conversation_id uuid not null references public.conversations(id) on delete cascade,
    user_id uuid not null references auth.users(id) on delete cascade,
    joined_at timestamptz not null default now(),
    last_read_at timestamptz,
    primary key (conversation_id, user_id)
);

alter table public.conversation_participants enable row level security;

-- Participants can see their conversations
create policy "participants see own conversations"
on public.conversation_participants for select to authenticated
using (user_id = auth.uid());

-- RLS for conversations: visible if you're a participant
create policy "participants see conversations"
on public.conversations for select to authenticated
using (
    exists (
        select 1 from public.conversation_participants
        where conversation_participants.conversation_id = conversations.id
          and conversation_participants.user_id = auth.uid()
    )
);

create policy "authenticated create conversations"
on public.conversations for insert to authenticated
with check (true);

create policy "participants update conversations"
on public.conversations for update to authenticated
using (
    exists (
        select 1 from public.conversation_participants
        where conversation_participants.conversation_id = conversations.id
          and conversation_participants.user_id = auth.uid()
    )
);

create policy "authenticated add conversation participants"
on public.conversation_participants for insert to authenticated
with check (true);

-- ---------------------------------------------------------------------------
-- Messages
-- ---------------------------------------------------------------------------
create table if not exists public.messages (
    id uuid primary key default gen_random_uuid(),
    conversation_id uuid not null references public.conversations(id) on delete cascade,
    sender_id uuid not null references auth.users(id) on delete cascade,
    content text not null default '',
    is_read boolean not null default false,
    created_at timestamptz not null default now()
);

alter table public.messages enable row level security;

create policy "participants see messages"
on public.messages for select to authenticated
using (
    exists (
        select 1 from public.conversation_participants
        where conversation_participants.conversation_id = messages.conversation_id
          and conversation_participants.user_id = auth.uid()
    )
);

create policy "participants send messages"
on public.messages for insert to authenticated
with check (
    sender_id = auth.uid() and
    exists (
        select 1 from public.conversation_participants
        where conversation_participants.conversation_id = messages.conversation_id
          and conversation_participants.user_id = auth.uid()
    )
);

create policy "sender update own messages"
on public.messages for update to authenticated
using (sender_id = auth.uid())
with check (sender_id = auth.uid());

create index if not exists messages_conversation_idx on public.messages(conversation_id, created_at);
create index if not exists messages_sender_idx on public.messages(sender_id);

-- ---------------------------------------------------------------------------
-- Parties
-- ---------------------------------------------------------------------------
create table if not exists public.parties (
    id uuid primary key default gen_random_uuid(),
    owner_id uuid not null references auth.users(id) on delete cascade,
    name text not null default '',
    is_public boolean not null default true,
    invite_code text not null default '',
    max_members integer not null default 10,
    created_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.parties enable row level security;

-- Party members can see the party
create policy "members see parties"
on public.parties for select to authenticated
using (
    is_public = true or
    owner_id = auth.uid() or
    exists (
        select 1 from public.party_members
        where party_members.party_id = parties.id
          and party_members.user_id = auth.uid()
    )
);

create policy "owners manage parties"
on public.parties for all to authenticated
using (owner_id = auth.uid()) with check (owner_id = auth.uid());

create policy "authenticated create parties"
on public.parties for insert to authenticated
with check (owner_id = auth.uid());

create unique index if not exists parties_invite_code_idx on public.parties(invite_code) where invite_code <> '';

-- ---------------------------------------------------------------------------
-- Party members
-- ---------------------------------------------------------------------------
create table if not exists public.party_members (
    party_id uuid not null references public.parties(id) on delete cascade,
    user_id uuid not null references auth.users(id) on delete cascade,
    role text not null default 'member' check (role in ('owner', 'admin', 'member')),
    joined_at timestamptz not null default now(),
    primary key (party_id, user_id)
);

alter table public.party_members enable row level security;

create policy "members see party members"
on public.party_members for select to authenticated
using (
    exists (
        select 1 from public.party_members pm
        where pm.party_id = party_members.party_id
          and pm.user_id = auth.uid()
    )
);

create policy "members join parties"
on public.party_members for insert to authenticated
with check (user_id = auth.uid());

create policy "members leave parties"
on public.party_members for delete to authenticated
using (user_id = auth.uid());

create policy "owners manage members"
on public.party_members for all to authenticated
using (
    exists (
        select 1 from public.parties
        where parties.id = party_members.party_id
          and parties.owner_id = auth.uid()
    )
);

-- ---------------------------------------------------------------------------
-- User presence
-- ---------------------------------------------------------------------------
create table if not exists public.user_presence (
    user_id uuid primary key references auth.users(id) on delete cascade,
    status text not null default 'offline' check (status in (
        'online', 'offline', 'in_game', 'away'
    )),
    status_message text not null default '',
    current_server_id text not null default '',
    last_seen_at timestamptz not null default now(),
    updated_at timestamptz not null default now()
);

alter table public.user_presence enable row level security;
create policy "users manage own presence"
on public.user_presence for all to authenticated
using (user_id = auth.uid()) with check (user_id = auth.uid());

create policy "users see friend presence"
on public.user_presence for select to authenticated
using (
    user_id = auth.uid() or
    exists (
        select 1 from public.friendships
        where (user_id_a = auth.uid() and user_id_b = user_presence.user_id)
           or (user_id_b = auth.uid() and user_id_a = user_presence.user_id)
    )
);

-- ---------------------------------------------------------------------------
-- Account activity (for realtime subscription)
-- ---------------------------------------------------------------------------
create table if not exists public.account_activity (
    id bigint generated always as identity primary key,
    user_id uuid not null references auth.users(id) on delete cascade,
    activity_type text not null default '',
    metadata jsonb not null default '{}'::jsonb,
    created_at timestamptz not null default now()
);

alter table public.account_activity enable row level security;
create policy "users see own activity"
on public.account_activity for select to authenticated
using (user_id = auth.uid());

create policy "system insert activity"
on public.account_activity for insert to authenticated
with check (true);

create index if not exists account_activity_user_idx on public.account_activity(user_id, created_at desc);
