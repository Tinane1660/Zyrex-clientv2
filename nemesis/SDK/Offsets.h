#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>

// ============================================================
//  Standknife byte patch offsets (client.dll / engine.dll)
//  Taken from a live process dump, verified byte-wise at runtime.
//
//  The dumped byte string always starts AT the listed RVA, e.g.
//    anti_flash | client.dll + 0x1C58C48 | 0F 2F 46 54 0F 87 CD 01 00 00 48 8B
//  means comiss xmm0,[rsi+0x54] sits at +0x00 and the conditional jump we
//  actually patch (0F 87 = ja) starts at +0x04. Every entry therefore carries
//  the delta from the dumped RVA to the first patched byte.
// ============================================================

namespace Offsets {
    inline const char* CLIENT_DLL = "client.dll";
    inline const char* ENGINE_DLL = "engine.dll";

    // client.dll
    inline uintptr_t no_recoil    = 0x23A2676;   // 8B 46 34 89 46 10
    inline uintptr_t fire_rate    = 0x1E45486;   // 48 89 87 E0 01 00 00
    inline uintptr_t no_spread    = 0x1E4552C;   // F3 0F 10 97 B0 01 00 00
    inline uintptr_t inf_ammo     = 0x1E453B3;   // 48 89 87 04 01 00 00
    inline uintptr_t anti_flash   = 0x1C58C48;   // 0F 2F 46 54 | 0F 87 CD 01 00 00
    inline uintptr_t anti_smoke   = 0x261E77A;   // F3 0F 10 40 30
    inline uintptr_t anti_molotov = 0x1B6C630;   // 8B 58 5C

    // engine.dll
    inline uintptr_t fullbright   = 0x65A18C;    // 8B 56 10 | 85 D2

    // offset from the dumped RVA to the first byte we overwrite
    inline uintptr_t anti_flash_delta = 0x4;
    inline uintptr_t fullbright_delta = 0x3;
}

namespace BytePatches {
    // Inf Ammo: mov [rdi+0x104], rax -> nop
    const std::vector<uint8_t> InfAmmo_Original = { 0x48, 0x89, 0x87, 0x04, 0x01, 0x00, 0x00 };
    const std::vector<uint8_t> InfAmmo_Patch    = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // No Spread: movss xmm2, [rdi+0x1B0] -> nop
    const std::vector<uint8_t> NoSpread_Original = { 0xF3, 0x0F, 0x10, 0x97, 0xB0, 0x01, 0x00, 0x00 };
    const std::vector<uint8_t> NoSpread_Patch    = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // No Recoil: keep the load, drop the store back into [rsi+0x10]
    const std::vector<uint8_t> NoRecoil_Original = { 0x8B, 0x46, 0x34, 0x89, 0x46, 0x10 };
    const std::vector<uint8_t> NoRecoil_Patch    = { 0x8B, 0x46, 0x34, 0x90, 0x90, 0x90 };

    // Fire Rate: mov [rdi+0x1E0], rax -> nop
    const std::vector<uint8_t> FireRate_Original = { 0x48, 0x89, 0x87, 0xE0, 0x01, 0x00, 0x00 };
    const std::vector<uint8_t> FireRate_Patch    = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // Anti Flash: ja rel32 -> nop + jmp rel32 (always skip the flash draw).
    // nop(1) + E9(1) + rel32(4) lands on exactly the same target as the 6 byte ja.
    const std::vector<uint8_t> AntiFlash_Original = { 0x0F, 0x87, 0xCD, 0x01, 0x00, 0x00 };
    const std::vector<uint8_t> AntiFlash_Patch    = { 0x90, 0xE9, 0xCD, 0x01, 0x00, 0x00 };

    // Anti Smoke: movss xmm0, [rax+0x30] -> xorps xmm0, xmm0 (alpha = 0.0f) + 2x nop
    const std::vector<uint8_t> AntiSmoke_Original = { 0xF3, 0x0F, 0x10, 0x40, 0x30 };
    const std::vector<uint8_t> AntiSmoke_Patch    = { 0x0F, 0x57, 0xC0, 0x90, 0x90 };

    // Anti Molotov: mov ebx, [rax+0x5C]; xor r8d, r8d -> xor ebx, ebx; xor r8d, r8d; nop (6 bytes)
    const std::vector<uint8_t> AntiMolotov_Original = { 0x8B, 0x58, 0x5C, 0x45, 0x31, 0xC0 };
    const std::vector<uint8_t> AntiMolotov_Patch    = { 0x31, 0xDB, 0x45, 0x31, 0xC0, 0x90 };

    // Fullbright: test edx, edx -> xor edx, edx
    const std::vector<uint8_t> Fullbright_Original  = { 0x85, 0xD2 };
    const std::vector<uint8_t> Fullbright_Patch     = { 0x31, 0xD2 };
}

// Long, unique signatures used as a fallback when the static RVA no longer
// holds the expected bytes (game update). patternDelta = distance from the
// pattern start to the first patched byte.
namespace Signatures {
    struct Entry {
        const char* pattern;
        int         patternDelta;
    };

    inline const Entry NoRecoil    = { "8B 46 34 89 46 10 33 C0 49 89 06 41", 0 };
    inline const Entry FireRate    = { "48 89 87 E0 01 00 00 48 8B 87 00 02", 0 };
    inline const Entry NoSpread    = { "F3 0F 10 97 B0 01 00 00 0F 57 F6 80", 0 };
    inline const Entry InfAmmo     = { "48 89 87 04 01 00 00 48 8B 0D", 0 };
    inline const Entry AntiFlash   = { "0F 2F 46 54 0F 87 CD 01 00 00 48 8B", 4 };
    inline const Entry AntiSmoke   = { "F3 0F 10 40 30 F2 0F 5C B4 24 00 01", 0 };
    inline const Entry AntiMolotov = { "8B 58 5C 45 33 C0 49 8B D4 E8", 0 };
    inline const Entry Fullbright  = { "8B 56 10 85 D2 44 89 47 04 41 B8 01", 3 };
}
