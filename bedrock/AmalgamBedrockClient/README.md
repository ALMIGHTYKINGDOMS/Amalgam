# Amalgam Bedrock Client

Supported Bedrock companion for the Amalgam Launcher. This is an add-on, not a
DLL injector: the client uses Behavior Packs, Resource Packs, Script API, and
supported Bedrock UI APIs.

## Current foundation

- Package-generated runtime identity plus bounded, player-scoped local settings and session state.
- Action-form menu opened with `/scriptevent amalgam:menu`.
- Real HUD values for coordinates, dimension, world, player, and session state.
- Action-bar HUD visibility, notifications, diagnostics, and reset actions. Minecraft owns action-bar placement, scale, and opacity.
- Offline-safe behavior: local features remain available without the Amalgam
  backend; online status is never fabricated.
- Multiplayer-safe menu access through `/scriptevent amalgam:menu` or by
  sneaking while using a compass. The menu only changes local settings and
  diagnostics, so it does not require operator permissions or send gameplay
  commands.
- Launcher-ready `.mcaddon` packaging, validation, inventory, and SHA-256 output.

The Bedrock client intentionally hides Java-only features that cannot be
implemented through supported Bedrock APIs. It does not claim fake FPS, TPS,
ping, CPS, CPU, RAM, social, cloud, or server data.

## Online and offline behavior

The client has no backend requirement for local play. HUD, settings,
notifications, diagnostics, and the supported menu continue working offline.

During multiplayer or Realms play, the same local features continue working
when the Amalgam behavior/resource packs are enabled for that world or server.
Bedrock does not provide a supported client-only injection path, so the
launcher cannot force an add-on into an arbitrary server that has not enabled
it. Account, social, cloud, and server metadata remain launcher-owned and are
shown only when real data is available.

## Build

From this directory:

```text
npm run validate
npm run package
```

The package is written to `dist/AmalgamBedrockClient.mcaddon`.
`dist/package-inventory.json` and `dist/package-sha256.txt` are emitted beside
it. The launcher can import the resulting `.mcaddon` through its existing
Bedrock add-on flow.

## Runtime target

The stable client targets Bedrock 1.21.0+ with the pinned `@minecraft/server`
1.13.0 and `@minecraft/server-ui` 1.3.0 APIs. `npm run version -- <version>`
updates the package version, both manifests, their modules/dependencies, and
the runtime metadata together. Change the package channel or API baseline
first, then run the same version command so `npm run validate` can verify the
single release contract before packaging.
