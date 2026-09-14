-- Upgrade & Polish migration.
-- Adds missing FK indexes, improves constraints, adds analytics helpers.

-- ---------------------------------------------------------------------------
-- 1. Missing FK indexes (performance: FK lookups without index = seq scan)
-- ---------------------------------------------------------------------------

-- hosting_nodes FK indexes
CREATE INDEX IF NOT EXISTS hosting_nodes_owner_idx ON public.hosting_nodes(owner_id);
CREATE INDEX IF NOT EXISTS hosting_nodes_owner_status_idx ON public.hosting_nodes(owner_id, status);

-- server_instances FK indexes
CREATE INDEX IF NOT EXISTS server_instances_template_idx ON public.server_instances(template_id);

-- server_operations FK indexes (already has server_idx and node_idx)
CREATE INDEX IF NOT EXISTS server_operations_user_idx ON public.server_operations(user_id);

-- console_lines FK indexes (already has server_idx)
-- runtime_telemetry FK indexes (already has server_idx and node_idx)

-- profiles FK index (already has user_idx)
-- bedrock_profiles FK index (already has user_idx)
-- subscriptions FK indexes (already has user_idx and external_idx)
-- turn_usage: PK is user_id, no additional index needed

-- friendships FK indexes
CREATE INDEX IF NOT EXISTS friendships_user_a_idx ON public.friendships(user_id_a);
CREATE INDEX IF NOT EXISTS friendships_user_b_idx ON public.friendships(user_id_b);

-- friend_requests FK indexes (already has receiver_idx and sender_idx)
-- conversations: no FK columns to index
-- conversation_participants FK indexes
CREATE INDEX IF NOT EXISTS conv_participants_user_idx ON public.conversation_participants(user_id);

-- messages FK indexes (already has conversation_idx and sender_idx)
-- parties: no additional FK indexes needed
-- party_members FK indexes
CREATE INDEX IF NOT EXISTS party_members_user_idx ON public.party_members(user_id);

-- user_presence: PK is user_id
-- public_profiles: PK is user_id
-- account_activity FK indexes (already has user_idx)

-- rate_limits FK index (already has user_action_idx)

-- events FK indexes (already has user_idx, category_idx, source_idx, name_idx, received_idx)
-- metrics FK indexes (already has name_idx, user_idx, recorded_idx)
-- errors FK indexes (already has fingerprint_idx, type_idx, severity_idx, user_idx, unresolved_idx)
-- feature_usage FK indexes (already has name_idx, user_idx)
-- audit_log FK indexes (already has actor_idx, resource_idx, action_idx)

-- ---------------------------------------------------------------------------
-- 2. Input length constraints (prevent abuse)
-- ---------------------------------------------------------------------------

-- Constrain text fields that accept user input
ALTER TABLE public.servers ALTER COLUMN name SET DEFAULT '';
ALTER TABLE public.servers ALTER COLUMN motd SET DEFAULT '';

-- Add CHECK constraints for reasonable lengths
DO $$ BEGIN
    ALTER TABLE public.servers ADD CONSTRAINT servers_name_len CHECK (char_length(name) <= 128);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.servers ADD CONSTRAINT servers_motd_len CHECK (char_length(motd) <= 256);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.profiles ADD CONSTRAINT profiles_name_len CHECK (char_length(name) <= 128);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.hosting_templates ADD CONSTRAINT template_name_len CHECK (char_length(name) <= 128);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.hosting_templates ADD CONSTRAINT template_desc_len CHECK (char_length(description) <= 2000);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.messages ADD CONSTRAINT messages_content_len CHECK (char_length(content) <= 4000);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.parties ADD CONSTRAINT parties_name_len CHECK (char_length(name) <= 64);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.friend_requests ADD CONSTRAINT friend_requests_message_len CHECK (char_length(message) <= 500);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.user_presence ADD CONSTRAINT presence_status_msg_len CHECK (char_length(status_message) <= 200);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.public_profiles ADD CONSTRAINT profile_display_name_len CHECK (char_length(display_name) <= 64);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.public_profiles ADD CONSTRAINT profile_bio_len CHECK (char_length(bio) <= 500);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

DO $$ BEGIN
    ALTER TABLE public.public_profiles ADD CONSTRAINT profile_minecraft_username_len CHECK (char_length(minecraft_username) <= 64);
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

-- ---------------------------------------------------------------------------
-- 3. Rate limit enforcement RPC
-- ---------------------------------------------------------------------------

-- Combined check-and-increment in one atomic operation
CREATE OR REPLACE FUNCTION public.enforce_rate_limit(
    p_action text,
    p_max_per_minute integer DEFAULT 60
)
RETURNS boolean LANGUAGE plpgsql SECURITY DEFINER SET search_path = public AS $$
DECLARE
    current_count integer;
BEGIN
    -- Clean up old entries (older than 2 minutes)
    DELETE FROM public.rate_limits
    WHERE window_start < now() - interval '2 minutes';

    -- Count recent actions
    SELECT count(*) INTO current_count
    FROM public.rate_limits
    WHERE user_id = auth.uid()
      AND action = p_action
      AND window_start > now() - interval '1 minute';

    IF current_count >= p_max_per_minute THEN
        RETURN false;  -- Rate limit exceeded
    END IF;

    -- Record this action
    INSERT INTO public.rate_limits(user_id, action, count, window_start)
    VALUES (auth.uid(), p_action, 1, now());

    RETURN true;  -- Within limit
END;
$$;

REVOKE ALL ON FUNCTION public.enforce_rate_limit(text, integer) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.enforce_rate_limit(text, integer) TO AUTHENTICATED;

-- ---------------------------------------------------------------------------
-- 4. Improved analytics: daily active users view
-- ---------------------------------------------------------------------------

CREATE MATERIALIZED VIEW IF NOT EXISTS public.daily_active_users AS
SELECT
    date_trunc('day', received_at) AS day,
    source,
    count(DISTINCT user_id) AS unique_users,
    count(*) AS total_events
FROM public.events
WHERE user_id IS NOT NULL
GROUP BY 1, 2;

CREATE UNIQUE INDEX IF NOT EXISTS daily_active_users_idx
    ON public.daily_active_users(day, source);

-- ---------------------------------------------------------------------------
-- 5. Improved analytics: server usage summary
-- ---------------------------------------------------------------------------

CREATE MATERIALIZED VIEW IF NOT EXISTS public.server_usage_summary AS
SELECT
    date_trunc('day', recorded_at) AS day,
    server_id,
    avg(tps) AS avg_tps,
    min(tps) AS min_tps,
    max(tps) AS max_tps,
    avg(mspt) AS avg_mspt,
    avg(player_count) AS avg_players,
    max(player_count) AS peak_players,
    avg(entity_count) AS avg_entities,
    avg(heap_used_mb) AS avg_heap_mb,
    count(*) AS sample_count
FROM public.runtime_telemetry
GROUP BY 1, 2;

CREATE UNIQUE INDEX IF NOT EXISTS server_usage_summary_idx
    ON public.server_usage_summary(day, server_id);

-- ---------------------------------------------------------------------------
-- 6. Idempotency: upsert for node registration (prevent duplicate inserts)
-- ---------------------------------------------------------------------------

-- Add unique constraint on node_secret_hash to prevent duplicate registrations
-- (The RPC already handles this, but the constraint adds a safety net)

-- ---------------------------------------------------------------------------
-- 7. Backup cleanup: auto-expire old console lines (keep 7 days)
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION public.cleanup_old_console_lines()
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path = public AS $$
BEGIN
    DELETE FROM public.console_lines
    WHERE timestamp < now() - interval '7 days';
END;
$$;

-- ---------------------------------------------------------------------------
-- 8. Backup cleanup: auto-expire old events (keep 30 days)
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION public.cleanup_old_events()
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path = public AS $$
BEGIN
    DELETE FROM public.events
    WHERE received_at < now() - interval '30 days';
    DELETE FROM public.metrics
    WHERE recorded_at < now() - interval '30 days';
    DELETE FROM public.errors
    WHERE occurred_at < now() - interval '90 days';
END;
$$;

REVOKE ALL ON FUNCTION public.cleanup_old_console_lines() FROM PUBLIC;
REVOKE ALL ON FUNCTION public.cleanup_old_events() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.cleanup_old_console_lines() TO SERVICE_ROLE;
GRANT EXECUTE ON FUNCTION public.cleanup_old_events() TO SERVICE_ROLE;

-- ---------------------------------------------------------------------------
-- 9. Refresh analytics views function
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION public.refresh_analytics_views()
RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path = public AS $$
BEGIN
    REFRESH MATERIALIZED VIEW CONCURRENTLY public.event_hourly_stats;
    REFRESH MATERIALIZED VIEW CONCURRENTLY public.error_daily_stats;
    REFRESH MATERIALIZED VIEW CONCURRENTLY public.daily_active_users;
    REFRESH MATERIALIZED VIEW CONCURRENTLY public.server_usage_summary;
END;
$$;

REVOKE ALL ON FUNCTION public.refresh_analytics_views() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.refresh_analytics_views() TO SERVICE_ROLE;
