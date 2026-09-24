-- Blanket "for all to authenticated using (true)" policies were written for the
-- Edge Functions, but every Edge Function runs as the caller: the work itself is
-- done by SECURITY DEFINER RPCs, which bypass RLS entirely. The blanket policies
-- therefore protect nothing and hand every signed-in user write access to rows
-- the RPCs are supposed to own -- reachable straight through PostgREST with the
-- public anon key and the user's own session:
--
--   subscriptions            any user could insert/update/delete their own (and
--                            anyone else's) subscription row, which is what
--                            entitlements read -> self-granted paid plan.
--   friendships              any user could befriend anyone, or delete another
--                            pair's friendship, without a request being accepted.
--   conversations            arbitrary conversation creation.
--   conversation_participants any user could add themselves to someone else's
--                            conversation and then read its messages.
--   account_activity         activity rows forged for any user_id.
--   audit_log                forged audit records (record_audit() is definer).
--
-- The scoped policies that remain cover every read and write the client and the
-- RPCs actually perform: owners read their own rows, participants manage their
-- own friendships and messages, and the definer RPCs own everything else.
drop policy if exists "backend manages subscriptions" on public.subscriptions;
drop policy if exists "system manages friendships" on public.friendships;
drop policy if exists "authenticated create conversations" on public.conversations;
drop policy if exists "authenticated add conversation participants" on public.conversation_participants;
drop policy if exists "system insert activity" on public.account_activity;
drop policy if exists "authenticated insert audit" on public.audit_log;

-- The staff activity view queries account_activity with no user filter, so the
-- row filter silently reduced "Recent Activity" to the staff member's own rows.
-- Every other collected table has this staff read; account_activity was missing
-- it.
drop policy if exists "staff read all activity" on public.account_activity;
create policy "staff read all activity"
    on public.account_activity for select to authenticated
    using (public.is_project_staff());

-- "users insert own activity" (user_id = auth.uid()) remains the only way a
-- client writes activity, and the client already filters its reads to its own
-- user_id.
