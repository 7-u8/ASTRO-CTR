# The developers' debug menu in CTR Nitro-Fueled 1.21: full analysis

Game: USA `CUSA13795` 1.21 (`out\eboot.elf`), static analysis, 2026-11-23. The EU 1.21 eboot keeps every data
address at the same place (proven for the ASTRO addresses), so the globals below very probably hold for EU too (code
addresses there shift by up to 0x40). Live address = eboot address + `0x800000000`.

**Not verified in the running game yet.** Everything below comes from the shipped code. Whether the menu can be
opened and drawn in the retail build has to be tested live.

Tools: `tools/eboot_debug/` (`lib.py` loads the ELF; `vsc.py` pairs script names with native functions;
`readers.py` lists who reads a global; `menu.py` sweeps the code for every menu item). Run with `py -3` (capstone).
REA was used on the PC host; its Ghidra on Windows refuses ELF files, so the eboot was read with these scripts.

## 1. What it is

The game ships Alchemy's developer tool system, **igWorkflowDebugMenu**, drawn with **Dear ImGui 1.47 WIP**, which is
linked into the eboot complete with its demo window ("ImGui Demo", "About ImGui") and the Metrics window
("ImGui Metrics", "1.47 WIP"). A render pass class `CImGuiRenderPass` draws it.

- Menu root: `igWorkflowDebugMenu`, enums `DebugMenuDisplayMode` {`kDebugMenuNothing`, `kDebugMenuFull`},
  `DebugMenuEvent` {`kDebugMenuEventOpen`, `kDebugMenuEventClose`, `kDebugMenuEventUpdate`},
  `DebugMenuRoot` {`kDebugMenuRootWorkflow`, `kDebugMenuRootOptions`, `kDebugMenuRootHelp`}.
- Sections are added by callbacks (`igWorkflowDebugMenuOptionsCallbackList`): names `addDebugMenuAIOptions`,
  `addDebugMenuCombatOptions`, `addDebugMenuDrawOptions`, `addDebugMenuEntityOptions`, `addDebugMenuMapOptions`,
  `addDebugMenuNet*Options` (Replay, Simulation, Marketplace, Leaderboard, Unlockables...), `addDebugMenuPerformanceOptions`,
  `addDebugMenuQAToolOptions`, `addDebugMenuUIOptions`, `addDebugMenuGrandPrix*`, `addDebugMenuAudio*`, `addDebugMenuMarketingOptions`.
- Workflows (tools in the Workflow root): `CWorkflowFreeCam`, `CWorkflowToolbarPauseGame`, `CWorkflowToolbarGameSpeed`,
  `CWorkFlowInspectKart`, `CWorkflowInspectRace`, `CWorkflowInspectAi`, `CWorkflowInspectCamera`, `CWorkflowInspectCombat`,
  `CWorkflowInspectAnimation`, `CWorkflowInspectJump`, `CWorkflowInspectLighting`, `CWorkflowInspectHavok`,
  `CWorkflowInspectNetwork`, `CWorkflowInspectLevelStreaming`, `CWorkflowInspectGui`, `CWorkFlowInspectAudio`,
  `CWorkflowBoltCreation`, `CWorkflowBoltSelectionMode`, `CWorkflowBoltVisualization`, `CWorkflowSelectionIsolation`.

### The menu API (eboot code)
| Address | Wraps | What it adds |
|---|---|---|
| `0x38c8a0` | `0x3c11f0` | on/off item: `(menu, "label", bool*)` |
| `0x38c880`, `0x38c8d0` | `0x3c8de0` | button |
| `0x38c8b0`, `0x38c8c0` | `0x3c1a70`, `0x3c24d0` | choice (radio) |
| `0x38c920` | `0x3bc950` | float slider (`"%.3f"`) |
| `0x38c950` | `0x3b4880` | int slider (`"%.0f"`) |
| `0x38d4d0` | `0x3b4a60` | sub-menu (tree node) |
| `0x38bdf0` | | text line (printf format) |

## 2. The switches (global bytes)

Found from the game's own script functions (`*_VSC`, names table `0x25fac90`, registered at `0x184b849`):

| Switch | Eboot address | Live address | Script setter | Readers |
|---|---|---|---|---|
| Allow debug menu (`CUIManager::_allowDebugMenu`, static) | `0x2ac0dbb` | `0x802ac0dbb` | `setAllowDebugMenu_VSC` `0x10bfae0` | snapshot `0x102e5e0` |
| Allow free camera | `0x2ab3ff7` | `0x802ab3ff7` | `setAllowFreeCamMode_VSC` `0x10bfb50` | game manager `0x9eee00`, `0x181a34a` |
| Allow NoClip | `0x2ab3ffa` | `0x802ab3ffa` | `setAllowNoClipMode_VSC` `0x10bfb10` | `0x9eee00`, `0x181a34a` |
| Unlock paths | `0x2ab3ffb` | `0x802ab3ffb` | (read: `isUnlockPaths_VSC`) | `0x9eee00`, `0x181a34a` |
| Unlock time trials | `0x2ab3ffc` | `0x802ab3ffc` | (read: `isUnlockTimeTrials_VSC`) | same |
| Override gems collected (int) | `0x2ab4000` | `0x802ab4000` | (read: `getOverrideGemsCollected_VSC`) | same |
| God mode | `0x2ab9c78` | `0x802ab9c78` | `setGodMode_VSC` `0x10bfa80` | `0x186a998`, `0xbbe5e0` |
| Hero undying | `0x2ab9c79` | `0x802ab9c79` | `setHeroUndying_VSC` | `0x186a998`, `0xd50a20`, `0xd50b50` |
| Touch of death | `0x2ab9c7a` | `0x802ab9c7a` | `setTouchOfDeath_VSC` | `0x186a998` |
| Enemies undying | `0x2ab9c7b` | `0x802ab9c7b` | `setEnemiesUndying_VSC` | `0x186a998`, `0xd50a20`, `0xd50b50` |
| Friendly fire | `0x2aa4d21` | `0x802aa4d21` | `setFriendlyFire_VSC` | `0x17b8b7c`, `0xccb330` |
| Split-screen prototype | game manager `+0x141` | (manager from `0x94efa0`) | `setSplitscreenPrototype_VSC` | |
| Draw HUD (`_showHud`) | `0x2ac0db8` | `0x802ac0db8` | menu item "Draw HUD" | |
| Show UI (`_showUI`) / last HUD / disable fade / debug messages | `0x2ac0db9` / `dba` / `dbc` / `dbd` | +0x800000000 | | |

`0x102e5e0` copies every switch into a backup block (`0x2ab72d8`..`0x2ab72e2`, flag `0x2ab72d8` = 1) the first
time a script changes one; `0x102e700`/`0x102e830` keep a counter at `[*0x2ada848]+0x60`. This looks like the
"cheats used" bookkeeping (achievements off while a cheat is on: string "You have activated a Cheat Code...").

The game manager class (field table `0x25e2840`, 90 fields) also holds: `_debugMenu`, `_debugPaused`,
`_noClipActive`, `_freecamActive`, `_freecamVisibilityLocked`, `_observerActive`, `_pauseForcedInFreecam`,
`_pendingFrameStepWanted`, `_isDebugLoadMap`, `_isReplayingMap`, `_drawTriggers`, `_drawSplines`,
`_drawTraversalPaths`, `_drawGroundDetection`, `_killZ`, `_gameMode`, `_levelName`, `_allowSplitscreen`.
The actor class (table `0x25af890`) holds `_timeScaleList`, `_allowTimeScaling`, `_freezeFrameTimeScale`,
`_inFreezeFrame`, `_usedNoClip`, `_debugMove`, and the four `_debug*DamageScale` floats.

## 3. Every item of the menu (from the code)

`[bool]` on/off, `[button]`, `[choice]`, `[float]`/`[int]` slider, `[submenu]`, `[text]` read-out, `[other]` another
ImGui widget. `-> 0x...` = the global the item edits (eboot address). The builder is the function that adds the
section.

## builder 0x9fdad0 (12)
  [bool] Show Time Elapsed
  [bool] Loop Shot
  [bool] Kill VFX At Loop End
  [bool] Draw Actor Names -> 0x2ab64d0
  [bool] Draw Actor Bolt Names -> 0x2ab64d1
  [button] Stop
  [button] Reset in-out range
  [button] Set in-out range to 36s-39s
  [button] Play configured range
  [float] In point (sec)
  [float] Out point (sec)
  [float] Seeking accuracy

## builder 0x9fe390 (2)
  [float] XP Multiplier -> "SpeedLevelingMultiplier"
  [bool] KidTest Mode

## builder 0x9fec30 (24)
  [bool] God Mode [HEROES] -> 0x2ab9c78
  [bool] Hero Undying -> 0x2ab9c79
  [bool] Touch of Death -> 0x2ab9c7a
  [bool] Enemies Undying -> 0x2ab9c7b
  [bool] Show Custom Parts -> 0x2ab9c7c
  [bool] Enable Entity Debug Update -> 0x2ab9c5b
  [bool] Show Static Count -> 0x2ab9c59
  [bool] Show Collectible Counts -> 0x2ab9c5a
  [bool] Show Entity Scale -> 0x2ab9c5c
  [bool] Show Entity Names -> 0x2ab9c5d
  [bool] Show Entity Teams -> 0x2ab9c5e
  [bool] Show Entity Axis -> 0x2ab9c5f
  [bool] Show Entity Positions -> 0x2ab9c60
  [bool] Show Entity Rotation -> 0x2ab9c61
  [bool] Show Entity Model Names -> 0x2ab9c62
  [bool] Show Entity Extents -> 0x2ab9c63
  [bool] Show Entity Bolts -> 0x2ab9c64
  [bool] Show Entity Stats -> 0x2ab9c58
  [bool] Show Most Expensive Entities -> 0x2ab9c69
  [bool] Dump Entity Cost Stats -> 0x2ab9c6c
  [bool] Show Actor Stats -> 0x2ab9c68
  [bool] Show Actor Count -> 0x2ab9c67
  [bool] Show Actor Muting -> 0x2ab9c6b
  [bool] Show Rigid Body Extents -> 0x2ab9c66

## builder 0x9fee70 (2)
  [bool] Physics Properties -> 0x2ab9c6e
  [bool] Health Display -> 0x2ab9c6f

## builder 0x9feed0 (2)
  [bool] Display Movement Speed -> 0x2ab9c77
  [bool] Infinite Lives -> "HasInfiniteLives"

## builder 0xa056b0 (1)
  [bool] Draw HUD -> 0x2ac0db8

## builder 0xa85d40 (2)
  [bool] Debug Havok
  [bool] Debug Havok AutoCapture

## builder 0xa933c0 (2)
  [float] Animation Speed
  [bool] Debug Lock Frame Rate

## builder 0xa938a0 (4)
  [button] Print Loaded Banks
  [button] Play All Character Localized VO
  [button] Play All Vehicle Localized VO
  [button] Stop Localized VO Playback Test

## builder 0xa93960 (11)
  [button] Dump Selected Pools
  [button] Fragmentation Report
  [button] Dump All Pools
  [button] Dump Vram Report
  [button] Dump String Pool Stats
  [button] Dump Global Chunk
  [button] Report Vfx Pool Sizes
  [button] Report Vfx Usage
  [button] Report Havok Usage
  [button] Report Object Handle Manager Usage
  [button] Print Memory Report

## builder 0xa9d1c0 (8)
  [bool] Lobby Status
  [bool] Session Status
  [bool] Extra Connection Infos
  [bool] Player Relative Platform Positions
  [bool] Base Game Stats
  [bool] Crash1 Level Stats
  [bool] Crash2 Level Stats
  [bool] Crash3 Level Stats

## builder 0xa9d620 (13)
  [choice] None
  [choice] Best
  [choice] Mean
  [choice] Worst
  [choice] Intermittent
  [choice] Custom
  [bool] Cable Connected
  [bool] Publisher File Changed
  [bool] Cell Network
  [float] Bandwidth (KBits/sec)
  [float] Mean one-way Latency (ms)
  [float] One-way Jitter (ms)
  [float] Packet Loss %%

## builder 0xb15bb0 (1)
  [bool] Show Mix Point Ranges

## builder 0xb48b30 (37)
  [bool] Defocus
  [bool] Motion Blur
  [bool] Bloom
  [bool] Override Defocus Planes
  [float] DoF Start
  [float] DoF End
  [float] HDR NIT
  [float] HDR HUD NIT
  [float] HDR HUD POW
  [bool] hdr curve
  [int] Max Fur Shells
  [choice] FXAA
  [choice] SMAA
  [bool] Override Clip Planes
  [float] Near Clip
  [float] Far Clip
  [bool] Show VFX
  [bool] Show Button Prompts
  [bool] Draw Lights
  [choice] None
  [choice] Point
  [choice] Box
  [choice] None
  [choice] Is World
  [choice] None
  [choice] Normals
  [choice] Albedo
  [choice] Albedo Clipping
  [choice] Gloss
  [choice] Metal
  [choice] Emissive
  [choice] Backscatter
  [choice] None
  [choice] Single Player
  [choice] Two Player Split-Screen
  [choice] Three Player Split-Screen
  [choice] Four Player Split-Screen

## builder 0xb53200 (3)
  [bool] Show Actor Mute Info
  [bool] Show Ambient Ranges
  [bool] Show Listener Position

## builder 0xb82110 (4)
  [submenu] Animations
  [submenu] Behavior Events
  [submenu] Animations
  [submenu] Behavior Events

## builder 0xb82430 (5)
  [other] Animation
  [bool] Loop
  [button] Preview
  [button] Stop
  [text] No animations

## builder 0xb827b0 (4)
  [text] Audio Information
  [text] ----
  [text] Last Input: %0.2f
  [text] Current Mode: %s

## builder 0xb83ea0 (3)
  [text] Jump Height
  [text] Max: %.2f
  [text] Min: %.2f

## builder 0xb83fd0 (3)
  [text] Movement Speed
  [text] Max: %.2f
  [text] Min: %.2f

## builder 0xb84130 (9)
  [submenu] Jump Plot
  [bool] Plot Jump
  [int] Track Jump Count
  [bool] Reset Plot
  [text] Height (crates)
  [text] Distance (crates)
  [text] Speed
  [text] Position
  [text] Time

## builder 0xb84520 (7)
  [submenu] Movement Speed
  [text] Actual
  [text] Desired
  [text] Animated
  [text] In/sec
  [text] In/Frame
  [text] Speed Stat: %.2f

## builder 0xb847e0 (10)
  [submenu] Behavior
  [text] Clip Name
  [text] Time
  [text] Layer
  [text] Weight%
  [text] Disabled
  [text] %0.1f
  [text] %d
  [text] %3d%%
  [text] %s

## builder 0xb84cf0 (5)
  [submenu] Jump Info
  [text] Jump Height: %.2f
  [text] Point of Interest
  [text] Position
  [text] Distance

## builder 0xb86100 (28)
  [text] Stick
  [text] Velocity
  [text] Forward Speed
  [text] 2D Speed (XY)
  [text] Touching wall: %s -> "YES"
  [submenu] Wheel Visuals
  [other] Debug Collision Material Handle Name
  [text] Ground Collision Material: %s
  [text] Visual Distance From Ground: %.2f
  [submenu] Surfaces
  [text] Diffuse Result:
  [text] Diffuse Intensity: %.3f
  [text] Gloss Result:
  [text] Gloss Intensity: %.3f
  [text] Emissive Result:
  [text] Emissive Intensity: %.3f
  [text] Wetness: %.3f
  [text] Current Accumulation: %.3f
  [text] Is Active: %s -> "true"
  [text] Boost Ratio
  [text] Air Time
  [text] Ground Normal
  [text] %2.1f degrees
  [text] Ground Collision Material
  [text] - Boost Data -
  [text] Speed: %.0f inches/s
  [text] Spent time: %.2f s
  [text] Remaining time: %.2f s

## builder 0xb98920 (1)
  [float] VFX Budget Reporting Fraction

## builder 0xb9c3c0 (1)
  [bool] FreeCam

## builder 0xb9c440 (1)
  [text] Game Speed

## builder 0xb9c4b0 (2)
  [button] Pause
  [button] Next Frame

## builder 0xba8790 (5)
  [button] Save Now
  [button] Load Now
  [button] Delete Save
  [button] Show Title Screen
  [button] Show EULA Screen

## builder 0xc0a220 (3)
  [bool] Show Vulnerable Regions -> 0x2ab5112
  [bool] Show Player Damage Per Second -> 0x2ab5110
  [bool] Ignore crits for DPS Display -> 0x2ab5111

## builder 0xc0ad50 (2)
  [bool] Show Difficulty Calculations -> 0x2ab7ed8
  [button] Reset Dynamic Difficulty For Zone

## builder 0xc9e6e0 (1)
  [bool] Display AI Character Names

## builder 0xca45b0 (1)
  [bool] Display NavMesh

## builder 0xcbcea0 (4)
  [float] DebugCam Rotation Speed
  [float] DebugCam Movement Speed
  [float] DebugCam FOV Speed
  [float] DebugCam Zoom Speed

## builder 0xd325d0 (1)
  [button] Send Gravy Message

## builder 0xeafa60 (1)
  [bool] Draw Racer LOD -> 0x2ae35b1

## builder 0x10783a0 (4)
  [button] Start
  [button] CompleteMap
  [button] Complete All
  [button] Reset All

## builder 0x1079490 (1)
  [bool] Reset On Debug Load

## builder 0x10794d0 (3)
  [button] Complete All Maps
  [button] Restart in ModeR
  [bool] Draw transitions

## 4. What is still unknown (to settle in game)

1. **Opening.** No code reads `_allowDebugMenu` except the backup routine and the reflection table, so the open
   path probably goes through the workflow system (`DebugMenuEvent` / `kDebugMenuEventOpen`) and the debug input
   (`_allowDebugInputs`, field table `0x25d2198`). The button combination is not found yet.
2. **Drawing.** `CImGuiRenderPass` exists (`0x18f1b8c` registers it). Whether the retail render pipeline includes it
   is unknown. If not, ASTRO can draw the same items with its own ImGui (it already has one) and call the game's
   builders, or edit the globals directly.
3. **Direct switches work without the menu.** The globals in section 2 are read by gameplay code, so writing them
   from ASTRO should act at once (god mode, free camera, unlocks). Test offline only.

## 5. A safe live test (offline, from ASTRO)

Write 1 to `0x802ab3ff7` (allow free cam) and `0x802ab3ffa` (allow NoClip), then check `_freecamActive` /
`_noClipActive` and the "Toggle FreeCam" / `ToggleNoClip` input actions. Write 1 to `0x802ac0dbb` and look for
the menu. Restore 0 afterwards. Never in online or ranked play.
