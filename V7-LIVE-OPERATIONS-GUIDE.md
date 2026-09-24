# AMALGAM V7 — LIVE OPERATIONS DEPLOYMENT GUIDE

**Version:** 1.0.0  
**Status:** OPERATIONAL
**Date:** August 23, 2026

---

## OVERVIEW

V7 makes everything live and real. This guide covers:

1. Supabase backend deployment
2. Microsoft Azure authentication setup
3. Launcher configuration
4. Live verification testing
5. Beta tester onboarding

---

## PREREQUISITES

### Required Accounts
- [ ] Supabase account (https://supabase.com)
- [ ] Microsoft Azure account (https://azure.microsoft.com)
- [ ] Minecraft Java Edition account (for testing)

### Required Tools
- [ ] Supabase CLI (`npm install -g supabase`)
- [ ] Azure CLI (`winget install Microsoft.AzureCLI`)
- [ ] Node.js 18+ (for Edge Functions)

---

## PHASE 1: SUPABASE BACKEND DEPLOYMENT

### 1.1 Link to Existing Project

The project is already linked to Supabase project `nnrrmvaxnoknthpwvttt`.

```bash
# Login to Supabase
supabase login

# Link to project (if not already linked)
supabase link --project-ref nnrrmvaxnoknthpwvttt
```

### 1.2 Deploy Migrations

There are 21 migration files in `supabase/migrations/`:

```bash
# Deploy all migrations
supabase db push

# Or deploy individually (in order)
supabase migration up
```

**Migration Order:**
1. `202608170001_project_publishing.sql`
2. `202608170002_essentials_control_plane.sql`
3. `202608170003_essentials_signaling.sql`
4. `202608170004_essentials_addresses.sql`
5. `202608170005_server_addresses.sql`
6. `202608180001_essentials_rpc_functions.sql`
7. `202608180002_remove_plaintext_join_tokens.sql`
8. `202608180003_beta_feedback.sql`
9. `202608180004_social_blocks.sql`
10. `202608180005_admin_user_directory.sql`
11. (11 more migrations...)

### 1.3 Deploy Edge Functions

There are 25 Edge Functions in `supabase/functions/`:

```bash
# Deploy all functions
supabase functions deploy

# Or deploy individually
supabase functions deploy get-turn-credentials
supabase functions deploy curseforge-catalog
supabase functions deploy get_friends
supabase functions deploy send_friend_request
supabase functions deploy accept_friend_request
# ... etc
```

### 1.4 Verify Deployment

```bash
# Check migration status
supabase migration list

# Check function status
supabase functions list

# Test a function
curl -X POST https://nnrrmvaxnoknthpwvttt.supabase.co/functions/v1/curseforge-catalog \
  -H "Authorization: Bearer YOUR_ANON_KEY" \
  -H "Content-Type: application/json" \
  -d '{"query": "sodium", "gameId": 6}'
```

---

## PHASE 2: MICROSOFT AZURE AUTHENTICATION

### 2.1 Create Azure AD App Registration

1. Go to https://portal.azure.com
2. Navigate to **Azure Active Directory** → **App registrations**
3. Click **New registration**
4. Configure:
   - **Name:** Amalgam Launcher
   - **Supported account types:** Accounts in any organizational directory and personal Microsoft accounts
   - **Redirect URI:** `http://localhost:8080` (for desktop app)
5. Click **Register**
6. Copy the **Application (client) ID** (36-character UUID)

### 2.2 Configure Authentication

1. In the app registration, go to **Authentication**
2. Add a platform:
   - **Mobile and desktop applications**
   - **Custom redirect URIs:** `http://localhost:8080`
3. Enable **Allow public client flows**

### 2.3 Configure API Permissions

1. Go to **API permissions**
2. Add permissions:
   - **Xbox Live** → `XboxLive.signin`
   - **Xbox Live** → `XboxLive.xboxliveaccess`
3. Click **Grant admin consent** (if you have admin rights)

### 2.4 Create Client Secret (Optional)

For production, you may need a client secret:
1. Go to **Certificates & secrets**
2. Click **New client secret**
3. Copy the secret value

**Note:** For desktop apps, public client flows are preferred.

---

## PHASE 3: LAUNCHER CONFIGURATION

### 3.1 Create launcher.json

Copy the template and fill in your credentials:

```bash
cp dist/amalgam-1.0.0/launcher.json.template launcher.json
```

### 3.2 Fill in Credentials

Edit `launcher.json`:

```json
{
    "base_dir": "instances",
    "assets_dir": "assets",
    "java_cache_dir": "runtimes/java",
    "bridges_dir": "bridges",
    "loader": "auto",
    "performance_profile": "auto",
    "microsoft_client_id": "YOUR_AZURE_CLIENT_ID",
    "supabase_url": "https://nnrrmvaxnoknthpwvttt.supabase.co",
    "supabase_anon_key": "YOUR_SUPABASE_ANON_KEY",
    "website_url": "https://amalgam-mc.com/",
    "api_url": "",
    "username": "",
    "ai_providers": []
}
```

### 3.3 Get the Launcher's Public Supabase Values

1. Go to https://supabase.com/dashboard
2. Select your project
3. Go to **Settings** → **API**
4. Copy:
   - **Project URL** (for `supabase_url`)
   - **public anon / publishable client key** (for `supabase_anon_key`)

**⚠️ SECURITY BOUNDARY:** `launcher.json` must not contain `supabase_service_key`, a `service_role` key, a secret key, or any other privileged Supabase credential — not even as an empty compatibility field. Privileged keys belong only in server-side secret management, such as the Supabase Edge Function environment, and must be accessed only by trusted server-side code.

---

## PHASE 4: LIVE VERIFICATION TESTING

### 4.1 Test Supabase Connection

```bash
# Test from launcher
./cpp/build-vs/amalgam_launcher.exe --doctor

# Expected output:
# READY   Native bridge    amalgam.dll found
# READY   Java bridges     19/19 supported bridge jars staged
# READY   Java runtime     1 system Java detected
# ACTION  Microsoft account  Connect a Microsoft account to launch Java Edition
# READY   Modrinth          Public catalog; run a live check to verify access
# READY   CurseForge        Provider key or backend proxy is not configured; Modrinth remains available
# READY   Minecraft Bedrock Windows app and addon data folder detected
# READY   Amalgam Bedrock Client  Bundled add-on package is ready to import
# READY   Release integrity Component manifest and release hashes are present
```

### 4.2 Test Microsoft Authentication

1. Launch the launcher
2. Click **Sign In** or **Connect Microsoft Account**
3. Complete the Microsoft login flow
4. Verify:
   - Account email displayed
   - Gamertag displayed
   - Xbox profile picture displayed
   - Session persists after restart

### 4.3 Test Supabase RPCs

Test the following operations:

```bash
# Test friend operations
# (Requires authenticated session)

# Test session operations
# (Requires authenticated session)

# Test entitlements
# (Requires authenticated session)
```

### 4.4 Test Real Minecraft Launch

1. Create a Vanilla profile
2. Click **Play**
3. Verify:
   - Managed Java downloads (if needed)
   - Minecraft files download
   - Game launches
   - Main menu appears
   - Game exits cleanly

---

## PHASE 5: BETA TESTER ONBOARDING

### 5.1 Prepare Distribution Package

The release artifacts are ready:

- **Installer:** `dist/installer/AmalgamLauncher-1.0.0-Setup.exe` (76 MB)
- **Archive:** `dist/amalgam-1.0.0.zip` (81 MB)
- **SHA-256:** `c87780211bd55cfa36b93cbbf11054628a4b2782cc2e9ba3c2cc35c7bad4cb70`

### 5.2 Create Beta Tester Guide

Include in distribution:

1. **Installation instructions**
2. **First-run walkthrough**
3. **Microsoft account setup**
4. **Profile creation**
5. **Content discovery**
6. **Essentials (friends/sessions)**
7. **Local servers**
8. **Bug reporting**

### 5.3 Distribute to Testers

Options:
- **Direct download:** Share the installer via secure link
- **Private GitHub release:** Upload to private repo releases
- **Discord/Slack:** Share in beta tester channel

### 5.4 Collect Feedback

Establish feedback channels:
- **Discord:** #beta-feedback channel
- **GitHub Issues:** Private repo
- **Email:** beta@amalgam-mc.com

---

## PHASE 6: MONITORING & SUPPORT

### 6.1 Supabase Dashboard

Monitor:
- **Authentication:** Login attempts, active sessions
- **Database:** Query performance, storage usage
- **Edge Functions:** Invocation counts, error rates
- **Realtime:** Active connections

### 6.2 Error Tracking

The launcher reports errors via:
- **Feedback RPC:** `submit_feedback` function
- **Diagnostics:** Built-in diagnostic tools
- **Logs:** Local log files

### 6.3 Support Channels

- **Discord:** #support channel
- **Email:** support@amalgam-mc.com
- **In-app:** Report Bug button

---

## TROUBLESHOOTING

### Common Issues

**1. "No Microsoft account connected"**
- Ensure `microsoft_client_id` is set in `launcher.json`
- Verify Azure AD app registration is configured correctly
- Check redirect URI matches (`http://localhost:8080`)

**2. "Supabase connection failed"**
- Verify `supabase_url` and `supabase_anon_key` are correct
- Check Supabase project is active
- Test connection with curl

**3. "Edge Function error"**
- Check function is deployed: `supabase functions list`
- Check function logs: `supabase functions logs FUNCTION_NAME`
- Verify JWT is valid

**4. "Minecraft launch failed"**
- Verify Java is installed or managed Java is available
- Check Minecraft files are downloaded
- Review launch logs

---

## SECURITY CHECKLIST

Before distributing to beta testers:

- [ ] No `supabase_service_key` property or other privileged Supabase credential in `launcher.json`, the client binary, installer, or release package
- [ ] Privileged Supabase keys are configured only in server-side secret management / Supabase Edge Function environments
- [ ] No Azure client secret in client binary
- [ ] No API keys in logs or error reports
- [ ] RLS policies enforced on all tables
- [ ] Edge Functions verify JWTs
- [ ] Rate limiting enabled
- [ ] Input validation on all RPCs

---

## NEXT STEPS

After V7 deployment:

1. **Monitor beta tester feedback**
2. **Fix reported issues**
3. **Iterate on UI/UX**
4. **Expand beta tester pool**
5. **Prepare public release**

---

**Document Version:** 1.0  
**Last Updated:** August 23, 2026  
**Author:** Amalgam Engineering Team
