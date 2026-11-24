# ASTRO CTR

**Live customisation for Crash Team Racing Nitro-Fueled (the PS4 build running in the SacredFire / shadPS4-based PC host).**
Everything is in one in-game menu (**F8**) with instant results, in the spirit of BakkesMod for Rocket League:
colours, kart paint, wheels, maps, items, effects and race tools.

**By ابو عرب | ABU 3RB** · TikTok [@_tmo](https://www.tiktok.com/@_tmo) · Instagram [@7.u8](https://www.instagram.com/7.u8) · Discord `7_ug`

> Unofficial fan project. Not affiliated with Activision, Beenox or Toys for Bob. No game files are included or modified.

[العربية في الأسفل](#العربية)

## Features (all in the F8 menu)

| Page | What it does |
|---|---|
| Home | everything ASTRO changes at a glance, with on/off switches |
| GAME | the race being driven: track, laps, lap times (offline races) |
| Nitro flame | 3 flame colours, smoke, brightness (normal and blue boost) |
| Kart paint | 3 slots × (tint + 6-colour ramp), whole kart / whole slot pickers, colour layouts |
| Underglow | the neon light under the Champion kart |
| Wheel Editor | recolour the wheels on your kart, part by part, with a live preview |
| MAPS | water, sky, snow, ice, lava, boost pads, Roo's Tubes, fog, sun, ambient light, colour grading of the map being raced |
| ITEMS | the colours of the power-ups: crates, potions, shields, Aku / Uka, electricity... |
| Effects | every effect colour loaded in the game, searchable, with a "flash" to find one |
| MISC | a gamepad view drawn on the game's screen with the game's own button art |
| MODS | performance read-out and the in-game optimize (hide shadows, grass, effects...) |
| Presets | save, load, import and export complete setups |
| DLL injector | load your own DLL mods into the game |
| Settings / Diagnostics | menu size, debug log, "restore everything", the layer's state |

RGB (rainbow) works on every colour. Settings are kept in `%APPDATA%\ASTRO-CTR\`.

## Compatibility

* The EU version `CUSA14876` and the USA version `CUSA13795`, both update **1.21**, in the PC host `sacredfire.exe` (Vulkan),
  including the CTR Competitive Hub build. Both 1.21 builds keep every address ASTRO uses at the same place
  (checked with `tools/eboot_port/`). Other versions are not supported.
* Windows 10/11 x64.

## Install and use

1. Extract the release anywhere and keep the files together (`ASTROCTRV2.exe`, `astro_ctr.dll`,
   `VkLayer_ASTRO_ctr_implicit.json`, the `*_catalog.json` files and the `wheels` folder).
2. **Close the game.** Open **`ASTROCTRV2.exe`** and keep it open: a fullscreen terminal with the animated ASTRO art and the
   connection state. (`ASTRO_CTR.exe`, the window version, works the same way and also holds the list of XML patch mods.)
3. Start the game normally. ASTRO shows `CONNECTED - press F8 in the game`.
4. In the game press **F8** (Insert is the host's own menu).

Closing the ASTRO launcher turns the mod off. If the game was already running when you opened ASTRO, restart the game.
DLLs in an `autoload` folder next to the launcher are loaded into the game automatically, once per game start.

## How it works

* While the launcher is open it registers a Vulkan layer for the current Windows user only
  (`HKCU\Software\Khronos\Vulkan\ImplicitLayers`) and removes it when closed. The Vulkan loader loads the layer into the game:
  ASTRO itself is not injected. The layer does nothing unless the process is `sacredfire.exe` **and** the launcher is open.
* The layer draws the menu with Dear ImGui on top of the game's frames and edits the game's memory from inside the process.
  Every target is found by its type or signature and re-validated before each write; restoring a value writes the game's own
  value back. Nothing is written to the game's files.
* The F8 menu is the ASTRO app itself: `app/main.cpp` is built both as `ASTRO_CTR.exe` and into the layer (`ASTRO_IN_GAME`).
* Research notes: [`docs/debug-menu-analysis.md`](docs/debug-menu-analysis.md) (the developers' debug menu left in the game,
  its switches and every item), [`docs/item_weights_game_files.md`](docs/item_weights_game_files.md).

## Build from source

Requirements: Windows x64, CMake ≥ 3.20, Ninja, a C++17 compiler (tested with MSYS2 UCRT64 g++ 15), Vulkan headers
(MSYS2 package `mingw-w64-ucrt-x86_64-vulkan-headers` or the Vulkan SDK). Dear ImGui 1.93.0 is vendored in `third_party/imgui`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/astro_tests.exe
```

Outputs in `build/`: `ASTROCTRV2.exe`, `ASTRO_CTR.exe`, `astro_ctr.dll`, `VkLayer_ASTRO_ctr_implicit.json`.
`run.bat` builds and copies everything to `run\` (MSYS2 in `C:\msys64`).
The catalogs and preview images are made from your own copy of the game with the scripts in `tools/`
(`*_catalog_build.py`, `gamepad_sheet_build.py`); game images are not part of this repository.
`-DASTRO_DEV_TOOLS=ON` also builds `present_test` (a stand-in host: copy it as `sacredfire.exe` to test the menu without the
game) and `ptrscan` (a pointer scanner).

## Project layout

| Path | Contents |
|---|---|
| `src/layer/` | the Vulkan layer: overlay, menu, game-memory features (nitro, paint, wheels, maps, items, effects, optimize) |
| `src/config/`, `src/core/`, `src/common/`, `src/diagnostics/`, `src/game/` | config (JSON), protocols between the app pages and the layer, colours, texture recolouring, log |
| `app/` | the ASTRO app pages (window version, and the F8 menu) |
| `launcher_v2/` | ASTRO CTR V2, the terminal launcher (`astro_art.txt` is its art) |
| `mods_dll/` | DLL mods (101% Story for EU and USA 1.21) |
| `data/` | catalogs the pages and the layer use |
| `tools/` | catalog builders, pointer scanner, eboot analysis scripts (`eboot_port/`, `eboot_debug/`) |
| `tests/` | unit tests; `present_test.cpp` stand-in host |
| `third_party/imgui/` | Dear ImGui 1.93.0 (MIT) |

## Troubleshooting

* The menu does not open: the launcher must be open **before** the game starts; check `%APPDATA%\ASTRO-CTR\astro_ctr.log`.
* Something does not change: open Diagnostics; the log says what was found.
* Report problems with the log file attached.

## Credits

Created by **ABU 3RB** · TikTok [@_tmo](https://www.tiktok.com/@_tmo) · Instagram [@7.u8](https://www.instagram.com/7.u8) · Discord `7_ug`.
If you share or fork ASTRO CTR, please keep these credits.

## License

MIT, see [LICENSE](LICENSE). Dear ImGui is MIT licensed (`third_party/imgui/LICENSE.txt`).

---

## العربية

**من صنع: ابو عرب | ABU 3RB** · تيك توك `_tmo` · انستقرام `7.u8` · ديسكورد `7_ug`

**ASTRO CTR** مود لـ Crash Team Racing Nitro-Fueled، كل شيء فيه داخل قائمة واحدة في اللعبة (زر **F8**) والتغيير يظهر **مباشرة**، على فكرة BakkesMod في Rocket League.

> مشروع من المعجبين، غير رسمي، ولا علاقة له بـ Activision أو Beenox أو Toys for Bob. لا يحتوي على ملفات من اللعبة ولا يعدّل ملفاتها.

**المميزات (كلها في F8):**
* **GAME:** الحلبة واللفات وأوقات اللفات.
* **النيترو، لون السيارة، إضاءة Champion، محرر العجلات.**
* **MAPS:** الماء والسماء والثلج والحمم والضباب والشمس والإضاءة.
* **ITEMS و Effects:** ألوان الأغراض وكل مؤثرات اللعبة.
* **MISC:** رسم اليد على الشاشة. **MODS:** الأداء والتحسين داخل اللعبة.
* **Presets، DLL injector، Settings، Diagnostics.**
* **RGB:** ألوان تدور على أي لون.

**التوافق:** النسخة الأوروبية `CUSA14876` والأمريكية `CUSA13795` إصدار **1.21**، على `sacredfire.exe`، ومنها CTR Competitive Hub.

**طريقة الاستخدام:**
1. فك الضغط وأبقِ الملفات مع بعض.
2. **أغلق اللعبة**، ثم افتح **`ASTROCTRV2.exe`** واتركه مفتوحاً (شاشة كاملة فيها رسمة ASTRO المتحركة وحالة الاتصال).
3. شغّل اللعبة كالعادة، وسيظهر `CONNECTED`.
4. داخل اللعبة اضغط **F8**.

إغلاق اللانشر يوقف المود. إذا كانت اللعبة مفتوحة قبل فتح ASTRO، أعد تشغيل اللعبة.
