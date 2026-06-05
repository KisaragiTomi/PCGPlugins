# MCPUnreal Editor Plugin

UE 5.7 C++ editor plugin that exposes editor internals via a local HTTP API for the `mcp-unreal` Go MCP server.

## What This Does

The plugin starts an HTTP server on `127.0.0.1:8090` when the editor loads. The Go `mcp-unreal` binary sends HTTP requests to this server to perform operations that the built-in Remote Control API cannot handle — actor management, Blueprint editing, output log access, viewport capture, and more.

## Installation

### Option 1: Copy to Project Plugins

```bash
cp -r plugin/ /path/to/YourProject/Plugins/MCPUnreal/
```

### Option 2: Copy to Engine Plugins

```bash
cp -r plugin/ "/Users/Shared/Epic Games/UE_5.7/Engine/Plugins/MCPUnreal/"
```

Then restart the editor. The plugin loads automatically on editor startup.

## Verify Installation

After the editor starts, check the output log for:

```
LogMCPUnreal: MCPUnreal plugin starting (version 0.1.0)
LogMCPUnreal: MCPUnreal HTTP server started on 127.0.0.1:8090 (routes: 37)
```

Or test the status endpoint:

```bash
curl -X POST http://localhost:8090/api/status
```

Expected response:

```json
{
  "name": "MCPUnreal",
  "version": "0.1.0",
  "ue_version": "5.7.0-...",
  "port": 8090,
  "project": "YourProject",
  "capabilities": ["status", "actors", "blueprints", "anim_blueprints", "editor", "assets"]
}
```

## Configuration

The HTTP server port can be changed via console variable:

```
mcp.Port 9090
```

Or in your project's `DefaultEngine.ini`:

```ini
[ConsoleVariables]
mcp.Port=9090
```

Remember to update the `PLUGIN_PORT` environment variable for the Go server to match.

## Security

- The HTTP server binds to **127.0.0.1 only** — it is not accessible from other machines.
- All route handlers validate input JSON before acting on it.
- No authentication is required (localhost-only design).
- See `SECURITY.md` in the repository root for the full security model.

## Available Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | POST/GET | Plugin health, version, capabilities |
| `/api/actors/list` | POST | List level actors with class/name/tag filters |
| `/api/actors/spawn` | POST | Spawn actor by class name with transform |
| `/api/actors/delete` | POST | Delete actors by path or name |
| `/api/blueprints/list` | POST | List all Blueprint assets |
| `/api/blueprints/inspect` | POST | Get Blueprint variables, functions, event graphs |
| `/api/blueprints/get_graph` | POST | Serialize a graph's nodes, pins, and connections |
| `/api/blueprints/modify` | POST | Create BP, add/remove vars/funcs, add/delete nodes, connect/disconnect pins |
| `/api/anim_blueprints/query` | POST | List state machines, inspect states and transitions |
| `/api/anim_blueprints/modify` | POST | Rename/create/delete state machines and states |
| `/api/assets/info` | POST | Asset metadata, class, package flags, tags |
| `/api/assets/dependencies` | POST | Get asset package dependencies |
| `/api/assets/referencers` | POST | Get asset reverse dependencies |
| `/api/editor/output_log` | POST | Read output log entries with category/verbosity filter |
| `/api/editor/capture_viewport` | POST | Screenshot the active viewport (base64 or file). `include_ui=true` for composited capture with Slate/UMG overlays (requires PIE) |
| `/api/editor/execute_script` | POST | Run Python script in editor |
| `/api/editor/console_command` | POST | Execute a console command |
| `/api/editor/pie_control` | POST | Start/stop/status for Play In Editor sessions |
| `/api/editor/player_control` | POST | Control player pawn (teleport, rotation, view target) and editor viewport camera |
| `/api/editor/live_compile` | POST | Trigger Live Coding (hot reload) compilation |
| `/api/mesh/procedural` | POST | ProceduralMesh operations (Phase 8) |
| `/api/mesh/realtime` | POST | RealtimeMesh operations (Phase 8) |
| `/api/pcg/generate` | POST | Trigger PCG graph generation on a component |
| `/api/pcg/cleanup` | POST | Clean up PCG-generated actors/components |
| `/api/pcg/set_graph` | POST | Assign a PCG graph to a component |
| `/api/gas/grant_ability` | POST | Grant a gameplay ability to an actor |
| `/api/gas/apply_effect` | POST | Apply a gameplay effect to a target |
| `/api/gas/get_attributes` | POST | Query attribute values from an ability system |
| `/api/niagara/spawn` | POST | Spawn a Niagara system at a location |
| `/api/niagara/set_parameter` | POST | Set a parameter on a Niagara component |
| `/api/niagara/control` | POST | Activate, deactivate, or reset a Niagara component |

## Building from Source

The plugin is built as part of your UE project. No separate build step is needed — just place it in `Plugins/` and rebuild.

If you need to build the project from the command line:

```bash
# macOS
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development \
  -project="/path/to/YourProject.uproject"
```

## Module Dependencies

- `Core`, `CoreUObject`, `Engine` — UE fundamentals
- `HTTPServer` — Embedded HTTP server (`FHttpServerModule`)
- `Json`, `JsonUtilities` — JSON parsing and serialization
- `Slate`, `SlateCore` — Editor UI access (viewport capture)
- `UnrealEd` — Editor subsystems and utilities
- `AssetRegistry` — Asset metadata and dependency queries
- `BlueprintGraph`, `KismetCompiler`, `Kismet` — Blueprint graph manipulation and compilation
- `AnimGraph` — Animation Blueprint state machine access
- `ImageCore` — Viewport pixel capture and PNG encoding
