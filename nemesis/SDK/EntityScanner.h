#pragma once
#include "IL2CPPResolver.h"
#include "../Memory/Memory.h"
#include "../Memory/PatternScanner.h"
#include <vector>

// Simple entity scanner using pattern scanning and pointer chains
class EntityScanner {
public:
    static EntityScanner& Get() {
        static EntityScanner instance;
        return instance;
    }

    bool Initialize(uintptr_t clientBase);
    std::vector<PlayerData> ScanPlayers();
    bool GetViewMatrix(float outMatrix[16]);

private:
    EntityScanner() = default;

    uintptr_t m_clientBase = 0;
    uintptr_t m_entityListPtr = 0;
    uintptr_t m_viewMatrixPtr = 0;
    bool m_initialized = false;

    // Scan for entity list using common patterns
    bool FindEntityList();
    bool FindViewMatrix();
    
    // Fallback mock data for testing
    std::vector<PlayerData> GetMockPlayers();
};
