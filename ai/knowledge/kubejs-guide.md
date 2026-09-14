# KubeJS Reference for Amalgam AI

KubeJS lets you create mod content via JavaScript.

## Item creation (startup script)

```js
StartupEvents.registry('item', event => {
    event.create('my_mod:my_item')
        .displayName('My Item')
        .tooltip('A custom item')
        .maxStackSize(64)
})
```

## Block creation

```js
StartupEvents.registry('block', event => {
    event.create('my_mod:my_block')
        .displayName('My Block')
        .hardness(3)
        .resistance(6)
        .requiresTool(true)
        .tagBlock('mineable/pickaxe')
})
```

## Recipes

```js
ServerEvents.recipes(event => {
    event.shaped('my_mod:my_item', [
        'DDD',
        'DSD',
        'DDD'
    ], {
        D: 'minecraft:diamond',
        S: 'minecraft:stick'
    })
})
```

## Script locations

All KubeJS scripts go in:
- `kubejs/startup_scripts/` — items, blocks, fluids
- `kubejs/server_scripts/` — recipes, loot, tags
- `kubejs/client_scripts/` — tooltips, JEI info
- `kubejs/assets/` — custom textures and models

## Paths for custom content

- Textures: `kubejs/assets/my_mod/textures/item/my_item.png`
- Models: `kubejs/assets/my_mod/models/item/my_item.json`
- Lang: `kubejs/assets/my_mod/lang/en_us.json`