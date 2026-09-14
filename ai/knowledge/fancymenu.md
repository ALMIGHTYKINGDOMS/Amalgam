# FancyMenu Reference for Amalgam AI

FancyMenu is a mod that lets you fully customize Minecraft's main menu.

## Config location

FancyMenu configs live in:
```
config/fancymenu/custom_menus/
```

Each menu is a JSON file named by screen class, e.g. `title_screen.json`
for the main menu.

## Common structure

```json
{
  "menu_id": "title_screen",
  "fancy_menus": {
    "my_custom_menu": {
      "menu": {
        "layout": {
          "background": {
            "image": "my_pack:textures/gui/menu/background.png"
          }
        },
        "elements": [
          {
            "type": "text",
            "text": "Welcome",
            "position": {"x": 50, "y": 50},
            "size": {"width": 200, "height": 40}
          },
          {
            "type": "button",
            "action": "play",
            "label": "Play",
            "position": {"x": 100, "y": 200}
          }
        ]
      }
    }
  }
}
```

## Asset paths

Backgrounds and buttons referenced by FancyMenu go in:
```
kubejs/assets/<namespace>/textures/gui/fancymenu/
```
or a resource pack:
```
assets/<namespace>/textures/gui/fancymenu/
```

## AI best practices

- Always detect the installed FancyMenu version before generating
- Preserve existing user menu configs unless explicitly asked to replace
- Verify element positions don't overlap or clip
- Test with Live Vision after launching Minecraft