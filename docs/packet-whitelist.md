# Packet Whitelist

Hard design rule: **no custom packets**. Every Amalgam action is expressed through
packets Minecraft already uses, sent through the normal client→server path, and
applied **only at the start of the game tick** (`START_CLIENT_TICK`).

## What Amalgam sends

| Controller | Action | Vanilla packet / call | Notes |
|---|---|---|---|
| NoFall | `MOVE_ON_GROUND` (3) | `PlayerMoveC2SPacket.OnGroundOnly(true, false)` | One per tick while enabled. `(onGround=true, horizontalCollision=false)` constructor in 1.21.11 yarn. |
| Fly (velocity) | `SET_VELOCITY` (1) | local `p.setVelocity(x,y,z)` + `p.setOnGround(false)` | Client-side velocity; the follow-up movement packet is whatever the game already sends. |
| Killaura | `ATTACK` (4) | `client.interactionManager.attackEntity(player, target)` + `player.swingHand(MAIN_HAND)` | Target resolved by entity id from the snapshot hostile scan. |
| Autotool | `SWAP_SLOT` (5) | `player.getInventory().setSelectedSlot(i)` | Picks the best `getMiningSpeedMultiplier` slot for the block under the crosshair. |
| Slot select | `SELECT_SLOT` (6) | `player.getInventory().setSelectedSlot(i)` | Direct slot request (0–8). |
| Freecam | `SET_POS` (2) | client-side camera only | `CameraMixin` overrides `setPos`/`setRotation`; never a packet. |

## What Amalgam does NOT send

- No custom/unknown packet ids.
- No server-side teleport packets for freecam.
- No fabricated entity/player packets.
- No packets outside the game tick (all actions drain during tick-start).

## Why tick-start only

- Actions are applied at a deterministic point in the client loop, before the game
  computes and sends its own movement state, so the server sees consistent input.
- `TickHandler` guards the drain with `try/finally` around
  `nativeEnterTick`/`nativeExitTick` so the native side always observes a balanced tick.

## Compatibility

These packets exist across the supported range (1.12 → 26.x), though constructors and
yarn names differ by version. The 1.21.11 bridge pins the exact mappings via Loom:
`PlayerMoveC2SPacket.OnGroundOnly(boolean,boolean)`, `ClientPlayerEntity.setVelocity`,
`setOnGround`, `getInventory().setSelectedSlot`, `getMiningSpeedMultiplier`,
`interactionManager.attackEntity`, `Camera.setPos/setRotation` (`@Shadow`).