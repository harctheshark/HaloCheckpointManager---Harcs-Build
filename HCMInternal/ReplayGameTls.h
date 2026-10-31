#pragma once
#include "pch.h"
#include "GameState.h"

// Small game-thread readers shared by the replay recorder and player.
namespace ReplayGameTls
{
    // The calling thread's TLS block of a module that declares static TLS (its TLS directory's AddressOfIndex), or 0.
    inline uintptr_t moduleTlsBlockOfThisThread(const wchar_t* moduleName)
    {
        __try
        {
            auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(moduleName));
            if (!base) return 0;
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (!dir.VirtualAddress) return 0;
            auto tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
            if (!tls->AddressOfIndex) return 0;
            const DWORD index = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
            auto slots = reinterpret_cast<uintptr_t*>(__readgsqword(0x58));   // TEB.ThreadLocalStoragePointer (x64)
            return slots ? slots[index] : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // the i16 at *(structSlot) + rateOffset, or 0
    inline int readRateAt(uintptr_t structSlot, uint32_t rateOffset)
    {
        __try
        {
            const uintptr_t gtg = *reinterpret_cast<const uintptr_t*>(structSlot);
            return gtg ? *reinterpret_cast<const int16_t*>(gtg + rateOffset) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // The game's current simulation tick rate (game time globals; the table MasterTickrate.cpp verified for MCC 1.3528),
    // or 0 when unknown. Call on the GAME thread for the TLS games (Halo 2's is a module global).
    //   Halo 2: *(halo2.dll+0x15FE008) i16 @+2;  Halo 3 / ODST: TLS slot 0xC8 @+4;  Halo 4: TLS 0x90 @+6;  Reach: TLS 0xA0 @+6
    inline int simTickRate(GameState game)
    {
        int rate = 0;
        switch (game.operator GameState::Value())
        {
        case GameState::Value::Halo2:
            if (const auto base = (uintptr_t)GetModuleHandleW(L"halo2.dll")) rate = readRateAt(base + 0x15FE008, 2);
            break;
        case GameState::Value::Halo3:
        case GameState::Value::Halo3ODST:
        case GameState::Value::Halo4:
        case GameState::Value::HaloReach:
        {
            const GameState::Value g = game.operator GameState::Value();
            const uint32_t slot = g == GameState::Value::Halo4 ? 0x90 : g == GameState::Value::HaloReach ? 0xA0 : 0xC8;
            const uint32_t at = (g == GameState::Value::Halo4 || g == GameState::Value::HaloReach) ? 6 : 4;
            if (const uintptr_t tls = moduleTlsBlockOfThisThread(game.toModuleName().c_str())) rate = readRateAt(tls + slot, at);
            break;
        }
        default: break;
        }
        return (rate >= 10 && rate <= 240) ? rate : 0;
    }
}
