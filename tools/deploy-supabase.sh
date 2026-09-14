#!/usr/bin/env bash
# Amalgam Supabase deployment helper.
# This script never hides migration/function failures.
#
# Usage:
#   bash tools/deploy-supabase.sh                  # verify, push migrations, deploy functions
#   bash tools/deploy-supabase.sh verify           # local static verification only
#   bash tools/deploy-supabase.sh migrations       # verify + push migrations
#   bash tools/deploy-supabase.sh functions        # verify + deploy functions
#
# Prerequisites for deployment:
#   Supabase CLI installed and authenticated.
#   The project is linked or SUPABASE_DB_PASSWORD is available to `supabase db push`.

set -euo pipefail

PROJECT_REF="${SUPABASE_PROJECT_REF:-nnrrmvaxnoknthpwvttt}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERIFY_SCRIPT="$ROOT_DIR/tools/verify-supabase.mjs"

usage() {
  sed -n '2,18p' "$0"
}

require_cli() {
  if ! command -v supabase >/dev/null 2>&1; then
    echo "ERROR: Supabase CLI is not installed." >&2
    echo "Install it from https://supabase.com/docs/guides/cli" >&2
    exit 127
  fi
}

verify_local() {
  node "$VERIFY_SCRIPT"
}

push_migrations() {
  require_cli
  echo "Applying all linked migrations for project $PROJECT_REF..."
  # db push applies every unapplied migration in lexical order. Do not loop,
  # truncate output, or ignore errors: doing so can report a false success.
  supabase db push --linked
  echo "Migrations applied successfully."
}

deploy_functions() {
  require_cli
  local function_dir function_name
  while IFS= read -r function_dir; do
    function_name="$(basename "$function_dir")"
    echo "Deploying Edge Function: $function_name"
    # JWT behavior comes from supabase/config.toml. The Whop webhook is the
    # only intentionally public function; all other functions verify JWTs.
    supabase functions deploy "$function_name" --project-ref "$PROJECT_REF"
  done < <(find "$ROOT_DIR/supabase/functions" -mindepth 1 -maxdepth 1 -type d ! -name '_*' | sort)
}

mode="${1:-all}"
cd "$ROOT_DIR"

case "$mode" in
  verify)
    verify_local
    ;;
  migrations)
    verify_local
    push_migrations
    ;;
  functions)
    verify_local
    deploy_functions
    ;;
  all)
    verify_local
    push_migrations
    deploy_functions
    ;;
  -h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
