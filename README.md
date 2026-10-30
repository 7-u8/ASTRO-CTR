# ASTRO CTR

**Live visual customisation for Crash Team Racing Nitro-Fueled (PS4 build running in the SacredFire / shadPS4-based PC host).**
Change your nitro flame, the Champion kart underglow and your kart's paint from an in-game menu (**F8**), with instant results, in the spirit of BakkesMod for Rocket League.

**By ابو عرب | ABU 3RB** · TikTok [@_tmo](https://www.tiktok.com/@_tmo) · Instagram [@7.u8](https://www.instagram.com/7.u8) · Discord `7_ug`

> Unofficial fan project. Not affiliated with Activision, Beenox or Toys for Bob. No game files are included or modified.

[العربية في الأسفل](#العربية)

## Features

| Menu | What it changes | Status |
|---|---|---|
| Nitro Color Studio | nitro flame (3 colours + smoke + brightness), normal and blue boost | works (garage and races) |
| Vehicle Studio → Champion underglow | the neon light under the Champion kart | works |
| Vehicle Studio → My kart paint | your kart's paint: 3 slots × (tint + 6-colour ramp), whole kart / whole slot pickers, colour layouts | works on the customization screen and in races |
| RGB | rainbow animation for any colour above | works |
| Settings | save/load `%APPDATA%\ASTRO-CTR\config.json`, menu size, debug log | works |

**Special paint jobs** (Atomic, Electron, Spectral, Gem, Neon, Rustland) get their look from their own textures and animation. Pick one in the game's Paint Job menu, then recolour it with ASTRO. Choosing them from the ASTRO menu itself is being researched (see *Research notes*).

## Compatibility

* **Only** the EU version `CUSA14876` update **1.21**, running in the PC host `sacredfire.exe` (Vulkan). Some addresses are specific to this build; other versions are not supported and may misbehave.
* Windows 10/11 x64.

## Install and use

1. Download the release zip and extract it anywhere (keep the three files together):
   `ASTRO_CTR_Launcher.exe`, `astro_ctr.dll`, `VkLayer_ASTRO_ctr_implicit.json`.
2. **Close the game.** Run `ASTRO_CTR_Launcher.exe` and keep its window open.
3. Start the game normally. The window prints `game detected` and `OK: menu ready`.
4. In game press **F8** (Insert is the host's own menu).
5. Settings → **Save settings** to keep your colours for next time.

Closing the ASTRO window turns the mod off. If the game was already running when you opened ASTRO, restart the game.

## How it works (no injection)

* While its window is open, the launcher registers a Vulkan layer for the current Windows user only
  (`HKCU\Software\Khronos\Vulkan\ImplicitLayers`) and removes it when closed. The Vulkan loader loads the layer into the game.
  The layer does nothing unless the game process is `sacredfire.exe` **and** the ASTRO window is open.
* The layer draws an ImGui overlay on top of the game's frames and writes colours into the game's memory from inside the process:
  * **Nitro:** the `igVfxColorCurveOperator` curves of `ctr_kart_exhaust_fire_1` (and its black-smoke variant), found by a background search.
  * **Underglow:** the `igVfxColorOperator` of `ctr_kart_shadow_kartchampion`, plus the live decal copies.
  * **Kart paint:** the kart's own resolved material command stream (`0x24 | ptr | 0x1e | 0 | P | RGBA | 0x1f`): the tint, and the 6-colour ramp after it. Every target is re-validated before each write. Writing at stale offsets after a paint-job change once corrupted draw commands, so this check is essential.
* Nothing is written to the game's files. Restoring a colour writes the game's own value back.

## Build from source

Requirements: Windows x64, CMake ≥ 3.20, Ninja, a C++17 compiler (tested with MSYS2 UCRT64 g++ 15), Vulkan headers
(MSYS2 package `mingw-w64-ucrt-x86_64-vulkan-headers` or the Vulkan SDK). Dear ImGui 1.93.0 is vendored in `third_party/imgui`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/astro_tests.exe
```

Outputs in `build/`: `ASTRO_CTR_Launcher.exe`, `astro_ctr.dll`, `VkLayer_ASTRO_ctr_implicit.json`.
`-DASTRO_DEV_TOOLS=ON` also builds `present_test` (a stand-in host: copy it as `sacredfire.exe` to test the overlay without the game)
and `ptrscan` (a pointer scanner for the running game).

## Project layout

| Path | Contents |
|---|---|
| `src/layer/` | Vulkan layer (`layer.cpp`), overlay renderer (`overlay.cpp`), menu (`menu.cpp`), game-memory backend (`nitro_backend.cpp`), paint palette |
| `src/config/`, `src/core/`, `src/common/`, `src/diagnostics/`, `src/game/` | config (JSON), controller, colours, log, backend interface |
| `launcher/` | the ASTRO window (layer registration, game detection) |
| `tests/` | unit tests; `present_test.cpp` stand-in host |
| `tools/ptrscan.cpp` | pointer scanner |
| `third_party/imgui/` | Dear ImGui 1.93.0 (MIT) |

## Research notes

* Kart customization component fields (from the game's reflection data): `_selectedColorScheme` +0x48, `_wantedColorScheme` +0x50,
  `_selectedColorSchemeDirtyCounter` +0x70, `_patchedColorScheme` +0xb0, `_pendingColorScheme` +0xd8. All 177 schemes are loaded,
  including the seasonal ones (`GP3_Color_*` Gem, `GP4_*` Spectral, `GP7_*` Rustland, `GP8_*` Atomic …). Writing `_wantedColorScheme`
  alone does not apply a scheme; the function that starts the load still has to be found.
* The paint copy of the shown kart stayed at the same address on the customization screen and in races; the pointer paths to it
  differ per track, so the mod keeps a kart once found while it validates.

## Troubleshooting

* The menu does not open: make sure the ASTRO window was open **before** the game started; check `%APPDATA%\ASTRO-CTR\astro_ctr.log`.
* A colour does not change: open the Diagnostics tab; the log says what was found.
* Report problems with the log file attached.

## Credits

Created by **ابو عرب | ABU 3RB**

* TikTok: [@_tmo](https://www.tiktok.com/@_tmo)
* Instagram: [@7.u8](https://www.instagram.com/7.u8)
* Discord: `7_ug`

If you share or fork ASTRO CTR, please keep these credits.

## License

MIT, see [LICENSE](LICENSE). Dear ImGui is MIT licensed (`third_party/imgui/LICENSE.txt`).

---

## العربية

**من صنع: ابو عرب | ABU 3RB** · تيك توك `_tmo` · انستقرام `7.u8` · ديسكورد `7_ug`

**ASTRO CTR** مود يغيّر ألوان Crash Team Racing Nitro-Fueled **مباشرة** من قائمة داخل اللعبة (زر **F8**)، على فكرة BakkesMod في Rocket League.

> مشروع من المعجبين، غير رسمي، ولا علاقة له بـ Activision أو Beenox أو Toys for Bob. لا يحتوي على ملفات من اللعبة ولا يعدّل ملفاتها.

**المميزات:**
* **النيترو:** 3 ألوان، والدخان، والسطوع، للبوست العادي والأزرق.
* **إضاءة Champion:** النور الذي تحت سيارة Champion.
* **لون سيارتك:** 3 خانات، في كل خانة لمعة و6 طبقات تدرّج، مع اختيار لون واحد للسيارة كلها أو لخانة كاملة. يعمل في شاشة التخصيص وفي السباق.
* **RGB:** ألوان تدور على كل الألوان، لأي لون من الألوان السابقة.
* **حفظ الإعدادات:** في `%APPDATA%\ASTRO-CTR\config.json`.
* **الطلاءات الخاصة** (Atomic و Electron و Spectral و Gem و Neon و Rustland): اختر الطلاء من قائمة Paint Job في اللعبة، ثم لوّنه من ASTRO. إضافتها مباشرة في قائمة ASTRO ما زالت قيد البحث.

**التوافق:** النسخة الأوروبية `CUSA14876` إصدار **1.21** فقط، على مشغّل الحاسوب `sacredfire.exe`. النسخ الأخرى غير مدعومة.

**طريقة الاستخدام:**
1. فك ضغط الملف، وأبقِ الملفات الثلاثة في مجلد واحد.
2. **أغلق اللعبة**، ثم شغّل `ASTRO_CTR_Launcher.exe` واترك نافذته مفتوحة.
3. شغّل اللعبة كالعادة. ستكتب نافذة ASTRO «game detected» ثم «OK: menu ready».
4. داخل اللعبة اضغط **F8**.
5. من Settings اضغط **Save settings** لتبقى ألوانك في المرة القادمة.

إغلاق نافذة ASTRO يوقف المود. إذا كانت اللعبة مفتوحة قبل فتح ASTRO، أعد تشغيل اللعبة.

**بدون حقن:** نافذة ASTRO تسجّل طبقة Vulkan لمستخدم ويندوز الحالي فقط، طوال ما هي مفتوحة، وتحذف التسجيل عند إغلاقها. والطبقة لا تعمل إلا داخل `sacredfire.exe` وما دامت نافذة ASTRO مفتوحة. ملفات اللعبة لا تُمسّ.
