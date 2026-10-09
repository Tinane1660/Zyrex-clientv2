#include "pch.h"
#include "Features/Features.h"
#include "Features/VehFeatures.h"
#include "Features/ESP.h"
#include "Hooks/DX11Hook.h"
#include "Console/DebugConsole.h"
#include "Stealth/stealth.hpp"
#include <thread>

static bool g_Running = true;

DWORD WINAPI MainThread(LPVOID lpParam) {
    HMODULE hModule = static_cast<HMODULE>(lpParam);

    DebugConsole::Initialize();
    DebugConsole::Log(LogLevel::Info, "Main thread started. Waiting for game modules...");

    // Wait for essential game modules to be fully mapped into process space
    int waitLoops = 0;
    while (!GetModuleHandleA("client.dll") && !GetModuleHandleA("engine.dll") && !GetModuleHandleA("UnityPlayer.dll")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (++waitLoops > 200) break; // max 20 seconds timeout
    }

    // Brief stabilization delay so the game engine finishes entrypoint startup
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Resolve and verify every byte patch site
    FeaturesManager::Get().Initialize();

    // Initialize VEH hooks (Anti-Aim, Chams, Air Jump, StopAllPlayers, etc.)
    VehFeatures::InitializeAll();

    // Install ESP memory hooks (Local & Enemy player coordinates in engine.dll)
    ESP::Initialize();

    // Install the swapchain hook so the menu and ESP render inside the game
    DebugConsole::Log(LogLevel::Info, "Installing DX11 SwapChain Present hook...");
    if (DX11Hook::Initialize()) {
        DebugConsole::Log(LogLevel::Success, "Present hook installed.");
    } else {
        DebugConsole::Log(LogLevel::Error, "Failed to install the Present hook!");
    }

    // Activate Stealth Bypass (Unlink from PEB, wipe LdrpHashTable and zero PE headers)
    stealth::hide_self(reinterpret_cast<uintptr_t>(hModule));
    DebugConsole::Log(LogLevel::Success, "Stealth protection applied: Module unlinked from PEB & headers wiped.");

    DebugConsole::Log(LogLevel::Success, "Ready. Press Menu key to toggle GUI, END to unload cheat.");

    bool insertPressed = false;
    bool endPressed = false;

    // Hotkey listener loop (VK_END for emergency unload; VK_INSERT is handled cleanly in WndProc)
    while (g_Running) {
        // VK_END (0x23) -> Emergency Safety Unload (Restore Memory & Exit DLL)
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            if (!endPressed) {
                DebugConsole::Log(LogLevel::Warning, "VK_END Pressed -> Emergency Safety Unload triggered!");
                endPressed = true;
                g_Running = false;
                break;
            }
        } else {
            endPressed = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Restore memory patches & cleanup completely
    DX11Hook::bShowMenu = false;
    ESP::Shutdown();
    FeaturesManager::Get().UnloadAll();
    DX11Hook::Shutdown();

    DebugConsole::Log(LogLevel::Success, "DLL Unloaded safely. Closing Debug Console in 400ms...");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    DebugConsole::Shutdown();

    // Final thread drain delay before unmapping code from process space
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, hModule, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
