# Datapacks & Resource Packs for Amalgam AI

## Datapack structure

A datapack lives in `<world>/datapacks/<packname>/` or `datapacks/` in the
profile root. Structure:

```
pack.mcmeta
data/
  <namespace>/
    recipes/*.json
    loot_tables/**/*.json
    advancements/**/*.json
    tags/**/*.json
    worldgen/**/*.json
    functions/*.mcfunction
```

`pack.mcmeta`:
```json
{
  "pack": {
    "pack_format": 15,
    "description": "My pack"
  }
}
```

## Resource pack structure

```
pack.mcmeta
assets/
  minecraft/
    textures/...
    models/...
    lang/en_us.json
  <namespace>/
    textures/item/...
    textures/block/...
    models/item/...
    models/block/...
    lang/en_us.json
```

## pack_format by version

- 1.18: 8
- 1.19: 9-10
- 1.20: 15
- 1.21: 34-42

## Item model JSON

```json
{
  "parent": "minecraft:item/generated",
  "textures": {
    "layer0": "my_namespace:item/my_item"
  }
}
```

## Recipe JSON

```json
{
  "type": "minecraft:crafting_shaped",
  "pattern": ["DDD", "DSD", "DDD"],
  "key": {
    "D": {"item": "minecraft:diamond"},
    "S": {"item": "minecraft:stick"}
  },
  "result": {"item": "my_namespace:my_item", "count": 1}
}
```

## Loot table JSON

```json
{
  "type": "minecraft:entity",
  "pools": [{
    "rolls": 1,
    "entries": [{
      "type": "minecraft:item",
      "name": "minecraft:diamond"
    }]
  }]
}
```