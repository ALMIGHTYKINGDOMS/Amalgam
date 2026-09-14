-- 202608220002 — Release security hardening.
--
-- These tables are intentionally backend/RPC-only: they have RLS enabled and
-- no direct Data API policies. Explicitly remove the default API privileges
-- from anonymous and authenticated roles so a later policy cannot accidentally
-- expose them without an intentional grant. SECURITY DEFINER RPCs remain the
-- supported authenticated API surface; service_role retains maintenance access.

revoke all on table
    public."Amalgam Launcher Storage",
    public.billing_events,
    public.billing_memberships,
    public.billing_preorders,
    public.essentials_invites,
    public.essentials_manifest_mods,
    public.essentials_manifests,
    public.essentials_session_bans,
    public.essentials_session_members,
    public.essentials_sessions,
    public.hosting_audit_events,
    public.hosting_automation_runs,
    public.hosting_automations,
    public.hosting_catalog_artifacts,
    public.hosting_content_items,
    public.hosting_nodes,
    public.hosting_server_console_lines,
    public.hosting_server_content,
    public.hosting_server_operations,
    public.hosting_server_requests,
    public.hosting_server_snapshots,
    public.hosting_servers,
    public.hosting_templates,
    public.staff_roles
from public, anon, authenticated;

grant all on table
    public."Amalgam Launcher Storage",
    public.billing_events,
    public.billing_memberships,
    public.billing_preorders,
    public.essentials_invites,
    public.essentials_manifest_mods,
    public.essentials_manifests,
    public.essentials_session_bans,
    public.essentials_session_members,
    public.essentials_sessions,
    public.hosting_audit_events,
    public.hosting_automation_runs,
    public.hosting_automations,
    public.hosting_catalog_artifacts,
    public.hosting_content_items,
    public.hosting_nodes,
    public.hosting_server_console_lines,
    public.hosting_server_content,
    public.hosting_server_operations,
    public.hosting_server_requests,
    public.hosting_server_snapshots,
    public.hosting_servers,
    public.hosting_templates,
    public.staff_roles
to service_role;

-- These trigger/helper functions are not public endpoints. Pin their search
-- paths and restrict execution to roles used by the authenticated launcher and
-- trusted backend. The functions are intentionally not SECURITY DEFINER.
alter function public.essentials_alias_valid(text)
    set search_path = pg_catalog, public, pg_temp;
alter function public.guard_project_workflow_fields()
    set search_path = pg_catalog, public, pg_temp;
alter function public.guard_version_workflow_fields()
    set search_path = pg_catalog, public, pg_temp;
alter function public.set_billing_updated_at()
    set search_path = pg_catalog, public, pg_temp;
alter function public.set_hosting_updated_at()
    set search_path = pg_catalog, public, pg_temp;

revoke all on function public.essentials_alias_valid(text) from public, anon;
revoke all on function public.guard_project_workflow_fields() from public, anon;
revoke all on function public.guard_version_workflow_fields() from public, anon;
revoke all on function public.set_billing_updated_at() from public, anon;
revoke all on function public.set_hosting_updated_at() from public, anon;

grant execute on function public.essentials_alias_valid(text) to authenticated, service_role;
grant execute on function public.guard_project_workflow_fields() to authenticated, service_role;
grant execute on function public.guard_version_workflow_fields() to authenticated, service_role;
grant execute on function public.set_billing_updated_at() to authenticated, service_role;
grant execute on function public.set_hosting_updated_at() to authenticated, service_role;
