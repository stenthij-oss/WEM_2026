# WEM_2026 — Weltraum, Erde, Mensch

Unreal Engine **5.8** project. C++ module `WEM_2026` plus World Partition levels.

## Getting started

This repo uses **Git LFS** for Unreal binary assets (`.uasset`, `.umap`).
Install it once before cloning, or the assets arrive as text pointer files:

```bash
git lfs install
git clone https://github.com/<owner>/WEM_2026.git
```

Then right-click `WEM_2026.uproject` → *Generate Visual Studio project files*, and
open the generated solution (or just double-click the `.uproject`).

## Layout

| Path | Contents |
| --- | --- |
| `Source/WEM_2026/` | C++ module — `RoomManager` is the entry point |
| `Content/` | Levels (`WEM.umap`, `NewMap.umap`), blueprints, placeholders |
| `Content/__ExternalActors__/` | World Partition actor data — one file per actor |
| `Config/` | Project + engine `.ini` settings |
| `.mcp.json` | MCP server config for editor tooling |

## Not in version control

`Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/` and the generated
`.sln`/`.slnx` files are ignored — Unreal regenerates all of them on build.
