# Microsoft sign-in setup

Amalgam uses Microsoft's device-code sign-in. The launcher never asks for a
player's password; Microsoft handles that in the browser.

Before distributing a build with Java Edition login enabled, the publisher must
register an application they control:

1. In the Microsoft Entra admin center, create an **App registration** named
   Amalgam Launcher.
2. Choose **Accounts in any organizational directory and personal Microsoft
   accounts**. Personal Microsoft accounts cover Xbox and Minecraft ownership.
   A personal-accounts-only registration is also suitable if the launcher will
   never support work or school accounts.
3. Copy the **Application (client) ID** from the app Overview page. It is a
   public GUID, not a secret.
4. In the app registration, open **Authentication**, add a **Mobile and desktop
   applications** platform, enable device-code/public-client authentication,
   and allow personal Microsoft accounts. Do not create a client secret.
5. In Amalgam, open **Settings > Advanced**, paste the value into
   **Microsoft sign-in (publisher setup)**, and save.
6. Use **Connect Microsoft account**. The launcher opens the Microsoft
   device-code page and then exchanges the approved Microsoft token for the
   Minecraft profile.

Do not paste a client secret into Amalgam. Device-code desktop apps use only
their public client ID, and player tokens remain encrypted with Windows account
protection on the local PC.

Microsoft's current registration guide:
https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app

The device-code flow requires the app's Application (client) ID:
https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code
