-- 202608220001 — Auth execution hardening.
--
-- These SECURITY DEFINER RPCs enforce their own ownership/staff checks, but
-- must not be callable by anonymous clients. Keep the public API surface
-- limited to authenticated launcher users and trusted service operations.

revoke all on function public.create_session(text, uuid, text, integer, integer, text, text, text, text, text, text) from public, anon;
revoke all on function public.get_session_manifest(text, text) from public, anon;
revoke all on function public.is_essentials_alias_available(text) from public, anon;
revoke all on function public.is_project_staff() from public, anon;
revoke all on function public.is_server_alias_available(text) from public, anon;
revoke all on function public.join_session(text, text) from public, anon;
revoke all on function public.leave_session(text) from public, anon;
revoke all on function public.resolve_essentials_address(text) from public, anon;
revoke all on function public.review_project(uuid, text, text) from public, anon;
revoke all on function public.start_session(text) from public, anon;
revoke all on function public.stop_session(text) from public, anon;
revoke all on function public.update_session(text, text, integer, integer, integer) from public, anon;
revoke all on function public.upsert_session_manifest(text, text, text, text, text, text, jsonb, jsonb, jsonb) from public, anon;

grant execute on function public.create_session(text, uuid, text, integer, integer, text, text, text, text, text, text) to authenticated, service_role;
grant execute on function public.get_session_manifest(text, text) to authenticated, service_role;
grant execute on function public.is_essentials_alias_available(text) to authenticated, service_role;
grant execute on function public.is_project_staff() to authenticated, service_role;
grant execute on function public.is_server_alias_available(text) to authenticated, service_role;
grant execute on function public.join_session(text, text) to authenticated, service_role;
grant execute on function public.leave_session(text) to authenticated, service_role;
grant execute on function public.resolve_essentials_address(text) to authenticated, service_role;
grant execute on function public.review_project(uuid, text, text) to authenticated, service_role;
grant execute on function public.start_session(text) to authenticated, service_role;
grant execute on function public.stop_session(text) to authenticated, service_role;
grant execute on function public.update_session(text, text, integer, integer, integer) to authenticated, service_role;
grant execute on function public.upsert_session_manifest(text, text, text, text, text, text, jsonb, jsonb, jsonb) to authenticated, service_role;
