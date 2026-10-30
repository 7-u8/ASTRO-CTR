#pragma once
// Every paint colour of the game's paint jobs: R, G, B float bits read from the KartColorScheme_*_materials.igz files
// in colorschemes.pak (EU 1.21; the colour after the 0x24/0x1e/0x01 commands of each material). Generated from the
// game files; comment = scheme file#slot.
#include <cstdint>

namespace astro {
static const uint32_t kPaintPalette[][3] = {
    {0x00000000, 0x00000000, 0x00000000},  // 11#1, 11#2
    {0x00000000, 0x3f800000, 0x3ea2a2a4},  // Elite_Iridescent_03#1
    {0x00000000, 0x3f800000, 0x3f60e0e1},  // Adventure_Polar#0
    {0x00000000, 0x3f800000, 0x3f67e7e9},  // KartDoomBuggy#2
    {0x00000000, 0x3f048485, 0x3f078788},  // Elite_Iridescent_04#1
    {0x00000000, 0x3e048487, 0x3f119192},  // 10#0
    {0x00000000, 0x3f068687, 0x3f20a0a1},  // Elite_Iridescent_03#2
    {0x00000000, 0x3f0d8d8e, 0x3f800000},  // Elite_Sapphire#0
    {0x00000000, 0x3f4dcdce, 0x3e828284},  // 02#0
    {0x00000000, 0x3ed0d0d0, 0x3f800000},  // 01#0
    {0x00000000, 0x3ed4d4d6, 0x3f800000},  // Elite_Sapphire#2
    {0x00000000, 0x3f6dedef, 0x3edadadc},  // Elite_Iridescent_04#0
    {0x00000000, 0x3ef6f6f6, 0x3efcfcfb},  // Elite_02#1
    {0x00000000, 0x3efcfcfb, 0x3f50d0d1},  // 00#0
    {0x00000000, 0x3efafafc, 0x3f800000},  // KartLeChaux#2
    {0x3f800000, 0x00000000, 0x3f69e9ea},  // Elite_NitroFueled_Pink#1
    {0x3f800000, 0x3f800000, 0x3f800000},  // KartLegacyPs4Exclusive_Cortex#2, KartLegacyPs4Exclusive_Pura#2
    {0x3f800000, 0x3f800000, 0x3ecacaca},  // KartLegacyPs4Exclusive_Pura#1
    {0x3f800000, 0x3f09898a, 0x00000000},  // Elite_NitroFueled_Orange#1
    {0x3f800000, 0x3f09898a, 0x3d30b0b2},  // 05#2
    {0x3f800000, 0x3f0c8c8c, 0x3e60e0de},  // KartLegacyPs4Exclusive_RipperRoo#0
    {0x3f800000, 0x3e149492, 0x3e149492},  // KartLegacyPs4Exclusive_Cortex#0
    {0x3f800000, 0x3f149495, 0x3e34b4b5},  // 05#0
    {0x3f800000, 0x3f2babad, 0x00000000},  // Adventure_Pura#2
    {0x3f800000, 0x3eacacad, 0x00000000},  // Elite_Iridescent_03#0
    {0x3f800000, 0x3eb2b2b2, 0x3eaeaeb0},  // 08#0
    {0x3f800000, 0x3f33b3b4, 0x00000000},  // Adventure_Cortex#2
    {0x3f800000, 0x3f39b9ba, 0x00000000},  // 04#1, 10#1
    {0x3f800000, 0x3f43c3c4, 0x00000000},  // Elite_03#0
    {0x3f800000, 0x3f48c8c8, 0x3e30b0af},  // KartLegacyPs4Exclusive_Cortex#1
    {0x3f800000, 0x3f4bcbcb, 0x00000000},  // 06#0
    {0x3f800000, 0x3ecccccd, 0x3f72f2f3},  // KartLegacyPs4Exclusive_Coco#0
    {0x3f800000, 0x3f57d7d9, 0x00000000},  // KartLegacyPs4Exclusive#1
    {0x3f800000, 0x3ee4e4e4, 0x00000000},  // Adventure_RipperRoo#0
    {0x3f800000, 0x3f6aeaec, 0x00000000},  // KartLeChaux#0
    {0x3f800000, 0x3f75f5f6, 0x00000000},  // Adventure_PapuPapu#0
    {0x3f800000, 0x3f75f5f6, 0x3e4ccccd},  // KartLegacyPs4Exclusive_PapuPapu#0
    {0x3b808081, 0x3f1b9b9d, 0x00000000},  // KartNitrousOxide#0
    {0x3d808081, 0x3da8a8a9, 0x3ececed0},  // Adventure_Crash#0
    {0x3f038384, 0x3f2eaeb0, 0x3e1c9c9e},  // Adventure_Dingodile#0
    {0x3f068687, 0x3e909093, 0x3f44c4c6},  // Elite_NitroFueled_Purple#2
    {0x3e868687, 0x3e868687, 0x3e868687},  // 11#0
    {0x3f078788, 0x3f0d8d8e, 0x3f1b9b9d},  // KartLeChaux#1, 00#1
    {0x3f078788, 0x3f5dddde, 0x3f6eeeee},  // 02#1
    {0x3e88888a, 0x3e88888a, 0x3e88888a},  // Adventure_NitrosOxide#2
    {0x3e88888a, 0x3e8e8e8f, 0x3e989898},  // 06#2
    {0x3e88888a, 0x3de8e8ea, 0x3f18989a},  // Elite_Iridescent_02#1
    {0x3f0a8a8b, 0x3e2cacb0, 0x3f55d5d6},  // Elite_NitroFueled_Purple#1
    {0x3e8c8c8c, 0x00000000, 0x3f1b9b9d},  // KartNitrousPreOrder#0
    {0x3f0b8b8d, 0x3f31b1b3, 0x3f42c2c3},  // 03#1
    {0x3f0d8d8e, 0x00000000, 0x00000000},  // 09#0
    {0x3f0d8d8e, 0x3ededede, 0x3df0f0f6},  // Adventure_KomodoJoe#0
    {0x3f0f8f8f, 0x00000000, 0x3f800000},  // Adventure_DrNGin#0
    {0x3d909091, 0x3ed2d2d3, 0x3f800000},  // Elite_Iridescent_02#2
    {0x3d909091, 0x3f64e4e6, 0x00000000},  // Adventure_TinyTiger#0
    {0x3f119192, 0x3f62e2e3, 0x3d909091},  // KartBandiBuggy#0
    {0x3e109093, 0x3ebebebe, 0x3f800000},  // Elite_NitroFueled_Blue#1
    {0x3e909093, 0x3ed4d4d6, 0x3f44c4c6},  // Elite_NitroFueled_Blue#2
    {0x3e949495, 0x3f22a2a2, 0x00000000},  // Elite_01#1
    {0x3f169697, 0x3eb8b8b8, 0x00000000},  // Elite_Iridescent_04#2
    {0x3e989898, 0x00000000, 0x00000000},  // Elite_Iridescent_02#0
    {0x3f19999a, 0x3ededede, 0x3e189898},  // Elite_03#1
    {0x3e9a9a9b, 0x3f51d1d3, 0x3f4bcbcb},  // Elite_NitroFueled_Aqua#2
    {0x3f1b9b9d, 0x3f1b9b9d, 0x3f1b9b9d},  // 07#2
    {0x3e1c9c9e, 0x3ed4d4d6, 0x3f2babad},  // 02#2
    {0x3e9c9c9e, 0x3e9c9c9e, 0x3e9c9c9e},  // Adventure_Pinstripe#0
    {0x3f21a1a2, 0x3e969698, 0x3d50d0d0},  // 01#2
    {0x3ea2a2a4, 0x00000000, 0x3f09898a},  // Elite_02#2
    {0x3ea6a6a7, 0x00000000, 0x3f41c1c3},  // KartLegacyPs4Exclusive_NGin#0
    {0x3f27a7a8, 0x3f27a7a8, 0x3f27a7a8},  // 04#2
    {0x3f2babad, 0x3f41c1c3, 0x00000000},  // KartLegacyPs4Exclusive_Dingodile#0
    {0x3f2fafb0, 0x00000000, 0x3f800000},  // Elite_02#0
    {0x3d30b0b2, 0x3f058587, 0x00000000},  // Adventure_NitrosOxide#0
    {0x3eb2b2b2, 0x3f25a5a5, 0x3f800000},  // Elite_Sapphire#1
    {0x3f33b3b4, 0x3f3bbbbb, 0x3f4dcdce},  // KartNitrousOxide#1, KartNitrousPreOrder#1
    {0x3f34b4b5, 0x3f34b4b5, 0x3f34b4b5},  // Adventure_KomodoJoe#1
    {0x3f37b7b8, 0x00000000, 0x00000000},  // 04#0
    {0x3f37b7b8, 0x3f37b7b8, 0x3f37b7b8},  // Adventure_DrNGin#1
    {0x3f38b8b8, 0x3f38b8b8, 0x3f38b8b8},  // Adventure_Pinstripe#1
    {0x3eb8b8b8, 0x3f1b9b9d, 0x3f3dbdbe},  // Basic_01#0
    {0x3eb6b6b9, 0x3f1b9b9d, 0x3f3dbdbe},  // Basic_01#1, Basic_01#2
    {0x3f3cbcbd, 0x3f3cbcbd, 0x3f3cbcbd},  // 09#2
    {0x3f40c0c1, 0x3e909093, 0x3f44c4c6},  // Elite_NitroFueled_Pink#2
    {0x3ec0c0c1, 0x3f31b1b3, 0x3f800000},  // Adventure_Ntropy#0
    {0x3f41c1c3, 0x3f41c1c3, 0x3f41c1c3},  // Adventure_PapuPapu#1
    {0x3f44c4c6, 0x3f47c7c9, 0x3f4dcdce},  // 06#1
    {0x3f44c4c6, 0x3efefefe, 0x3e909093},  // Elite_NitroFueled_Orange#2
    {0x3f46c6c7, 0x3f109091, 0x00000000},  // Elite_03#2
    {0x3f46c6c7, 0x3f46c6c7, 0x3f46c6c7},  // KartLegacyPs4Exclusive_NGin#1
    {0x3f48c8c8, 0x3f48c8c8, 0x3f48c8c8},  // Adventure_RipperRoo#1
    {0x3ecacaca, 0x3de8e8ea, 0x3f66e6e7},  // Adventure_Pura#0
    {0x3f4acacb, 0x3f4acacb, 0x3f4acacb},  // Adventure_Polar#1
    {0x3f4ccccd, 0x3f4ccccd, 0x3f4ccccd},  // Adventure_Ntropy#1
    {0x3ecccccd, 0x3f71f1f1, 0x3f800000},  // KartLegacyPs4Exclusive_Polar#0
    {0x3f4fcfd0, 0x3f56d6d7, 0x3f67e7e9},  // Adventure_Crash#1
    {0x3f51d1d3, 0x3f51d1d3, 0x3f51d1d3},  // Adventure_Dingodile#1
    {0x3f51d1d3, 0x3e70f0f0, 0x3e70f0f0},  // Elite_NitroFueled_Red#2
    {0x3f52d2d3, 0x3f52d2d3, 0x3f52d2d3},  // Adventure_PentaPenguin#0
    {0x3f53d3d4, 0x3f53d3d4, 0x3f53d3d4},  // Adventure_PentaPenguin#1
    {0x3f54d4d6, 0x00000000, 0x3f73f3f4},  // Adventure_Coco#0
    {0x3ed4d4d6, 0x3f800000, 0x3e989898},  // KartLegacyPs4Exclusive_TinyTiger#0
    {0x3f55d5d6, 0x3f55d5d6, 0x3f55d5d6},  // Adventure_FakeCrash#1
    {0x3f56d6d7, 0x3f56d6d7, 0x3f56d6d7},  // Adventure_Crash#2
    {0x3ed8d8d8, 0x3f6dedef, 0x3e909093},  // Elite_01#2
    {0x3f59d9da, 0x3f38b8b8, 0x00000000},  // Elite_NitroFueled_Yellow#1
    {0x3f59d9da, 0x3f59d9da, 0x3f59d9da},  // Adventure_Dingodile#2, Adventure_RipperRoo#2
    {0x3f5bdbdb, 0x00000000, 0x3ca0a09f},  // KartImperium#0
    {0x3f5adadc, 0x3f5cdcdd, 0x3f800000},  // Adventure_Cortex#1
    {0x3edadadc, 0x3f1a9a9b, 0x3f3ebec0},  // Adventure_FakeCrash#0
    {0x3f5dddde, 0x3f68e8e9, 0x3f800000},  // KartDoomBuggy#1
    {0x3f60e0e1, 0x3f60e0e1, 0x3f60e0e1},  // Adventure_Pura#1, Adventure_PapuPapu#2
    {0x3ee0e0e1, 0x3ee0e0e1, 0x3ee0e0e1},  // KartBandiBuggy#2
    {0x3f63e3e4, 0x3f30b0b1, 0x00000000},  // 01#1
    {0x3f63e3e4, 0x3f63e3e4, 0x3f63e3e4},  // Elite_Platine#2
    {0x3f64e4e6, 0x00000000, 0x00000000},  // Adventure_Cortex#0
    {0x3f65e5e6, 0x3f65e5e6, 0x3f65e5e6},  // KartImperium#1
    {0x3f66e6e7, 0x3f66e6e7, 0x3f66e6e7},  // Adventure_NitrosOxide#1
    {0x3f67e7e9, 0x3f67e7e9, 0x3f67e7e9},  // Adventure_PentaPenguin#2, 08#1
    {0x3f68e8e9, 0x3f68e8e9, 0x3f68e8e9},  // KartNitrousOxide#2, KartNitrousPreOrder#2
    {0x3e68e8ea, 0x00000000, 0x3ef2f2f3},  // KartDeadinator#0
    {0x3f69e9ea, 0x3f69e9ea, 0x3f69e9ea},  // Adventure_TinyTiger#1
    {0x3f6dedef, 0x3f72f2f3, 0x3f800000},  // 05#1
    {0x3f6feff0, 0x3ef0f0f0, 0x3f800000},  // 07#0
    {0x3d70f0f1, 0x3eb4b4b5, 0x3f800000},  // KartLegacyPs4Exclusive#0
    {0x3f70f0f1, 0x3f70f0f1, 0x3f70f0f1},  // KartDeadinator#1
    {0x3df0f0f6, 0x3f1e9ea0, 0x3f21a1a2},  // Elite_NitroFueled_Aqua#1
    {0x3e74f4f6, 0x3e74f4f6, 0x3e74f4f6},  // KartImperium#2
    {0x3f75f5f6, 0x3f75f5f6, 0x3f75f5f6},  // KartDoomBuggy#0
    {0x3f77f7f9, 0x3f66e6e7, 0x3cc0c0c0},  // Elite_NitroFueled_Yellow#2
    {0x3f78f8f9, 0x00000000, 0x00000000},  // Elite_NitroFueled_Red#1
    {0x3e7cfcfb, 0x00000000, 0x3f5adadc},  // KartLegacyPs4Exclusive_Pura#0
    {0x3f7bfbfc, 0x3f54d4d6, 0x00000000},  // 03#0
    {0x3efefefe, 0x3f2cacad, 0x3f2aaaab},  // 03#2
};
}  // namespace astro
