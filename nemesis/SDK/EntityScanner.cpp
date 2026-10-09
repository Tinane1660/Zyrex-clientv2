#include "EntityScanner.h"
#include "RuntimeScanner.h"
#include "../Console/DebugConsole.h"
#include "../SDK/Offsets.h"
#include <cstring>
#include <chrono>

// Player structure offsets (Based on gandon.cc Standknife)
namespace PlayerOffsets {
    constexpr size_t m_vecOrigin = 0x170;      // Position (Vec3) - May need adjustment
    constexpr size_t m_iHealth = 0x140;        // Health (int) - May need adjustment  
    constexpr size_t m_iTeamNum = 0x3C8;       // Team (int) - May need adjustment
    constexpr size_t m_lifeState = 0x27;       // Life state (byte, 0 = alive)
    constexpr size_t m_szPlayerName = 0x650;   // Name (char[64])
    constexpr size_t m_bDormant = 0xED;        // Dormant flag (byte)
    
    // Head position calculation: add ~72 units to Z axis for standing player
    constexpr float HEAD_OFFSET = 72.0f;
}

// Helper function with SEH protection for reading entity data
static bool ReadEntityData(uintptr_t entityAddr, PlayerData& outPlayer, int index) {
    if (entityAddr == 0 || entityAddr < 0x10000) return false;

    __try {
        // Read position
        float* pPos = reinterpret_cast<float*>(entityAddr + PlayerOffsets::m_vecOrigin);
        if (pPos && !IsBadReadPtr(pPos, sizeof(float) * 3)) {
            outPlayer.position.x = pPos[0];
            outPlayer.position.y = pPos[1];
            outPlayer.position.z = pPos[2];
            outPlayer.headPosition = Vec3(pPos[0], pPos[1], pPos[2] + PlayerOffsets::HEAD_OFFSET);
        } else {
            return false;
        }

        // Read health
        int* pHealth = reinterpret_cast<int*>(entityAddr + PlayerOffsets::m_iHealth);
        if (pHealth && !IsBadReadPtr(pHealth, sizeof(int))) {
            outPlayer.health = static_cast<float>(*pHealth);
        } else {
            outPlayer.health = 100.0f;
        }

        // Read team
        int* pTeam = reinterpret_cast<int*>(entityAddr + PlayerOffsets::m_iTeamNum);
        if (pTeam && !IsBadReadPtr(pTeam, sizeof(int))) {
            outPlayer.team = *pTeam;
        } else {
            outPlayer.team = 0;
        }

        // Read alive state
        byte* pLifeState = reinterpret_cast<byte*>(entityAddr + PlayerOffsets::m_lifeState);
        if (pLifeState && !IsBadReadPtr(pLifeState, sizeof(byte))) {
            outPlayer.isAlive = (*pLifeState == 0);
        } else {
            outPlayer.isAlive = false;
        }

        // Read name
        char* pName = reinterpret_cast<char*>(entityAddr + PlayerOffsets::m_szPlayerName);
        if (pName && !IsBadReadPtr(pName, 4)) {
            strncpy_s(outPlayer.name, sizeof(outPlayer.name), pName, _TRUNCATE);
        } else {
            snprintf(outPlayer.name, sizeof(outPlayer.name), "Player_%d", index);
        }

        outPlayer.isLocalPlayer = false;

        return outPlayer.isAlive && outPlayer.health > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EntityScanner::Initialize(uintptr_t clientBase) {
    if (m_initialized) return true;

    m_clientBase = clientBase;
    DebugConsole::Log(LogLevel::Info, "[EntityScanner] Initializing entity scanner...");

    if (FindEntityList()) {
        DebugConsole::Log(LogLevel::Success, "[EntityScanner] EntityList found successfully!");
    } else {
        DebugConsole::Log(LogLevel::Warning, "[EntityScanner] EntityList not found - will try runtime scanning");
        
        // Пробуем runtime сканирование
        uintptr_t foundEntityList = 0;
        if (RuntimeScanner::Get().ScanForEntityList(m_clientBase, 0x7A2C000, foundEntityList)) {
            m_entityListPtr = foundEntityList;
            DebugConsole::Log(LogLevel::Success, "[EntityScanner] EntityList found via runtime scan!");
        }
    }

    if (FindViewMatrix()) {
        DebugConsole::Log(LogLevel::Success, "[EntityScanner] ViewMatrix found successfully!");
    } else {
        DebugConsole::Log(LogLevel::Warning, "[EntityScanner] ViewMatrix not found - trying runtime scan");
        
        // Пробуем runtime сканирование ViewMatrix
        uintptr_t foundViewMatrix = 0;
        if (RuntimeScanner::Get().ScanForViewMatrix(m_clientBase, 0x7A2C000, foundViewMatrix)) {
            m_viewMatrixPtr = foundViewMatrix;
            DebugConsole::Log(LogLevel::Success, "[EntityScanner] ViewMatrix found via runtime scan!");
        } else {
            DebugConsole::Log(LogLevel::Info, "[EntityScanner] Using mock ViewMatrix for testing");
        }
    }

    m_initialized = true;
    return true;
}

bool EntityScanner::FindEntityList() {
    if (m_clientBase == 0) return false;

    char buf[256];
    
    // Попытка 1: Ищем через паттерн обращения к EntityList
    DebugConsole::Log(LogLevel::Info, "[EntityScanner] Searching for EntityList via patterns...");
    
    // Паттерн 1: mov rcx, [client.dll + EntityList]
    uintptr_t pattern1 = PatternScanner::FindPattern("client.dll", "48 8B 0D ? ? ? ? 8B 81");
    if (pattern1) {
        std::vector<uint8_t> buffer;
        if (Memory::SafeReadBytes(pattern1 + 3, sizeof(int32_t), buffer) && buffer.size() >= sizeof(int32_t)) {
            int32_t rva = 0;
            memcpy(&rva, buffer.data(), sizeof(int32_t));
            uintptr_t foundAddr = pattern1 + 7 + rva;
            
            snprintf(buf, sizeof(buf), "[EntityScanner] EntityList found via pattern 1 at: 0x%llX (Offset: +0x%llX)", 
                static_cast<unsigned long long>(foundAddr),
                static_cast<unsigned long long>(foundAddr - m_clientBase));
            DebugConsole::Log(LogLevel::Success, buf);
            
            m_entityListPtr = foundAddr;
            return true;
        }
    }
    
    // Паттерн 2: mov rax, [client.dll + EntityList]
    uintptr_t pattern2 = PatternScanner::FindPattern("client.dll", "48 8B 05 ? ? ? ? 48 85 C0");
    if (pattern2) {
        std::vector<uint8_t> buffer;
        if (Memory::SafeReadBytes(pattern2 + 3, sizeof(int32_t), buffer) && buffer.size() >= sizeof(int32_t)) {
            int32_t rva = 0;
            memcpy(&rva, buffer.data(), sizeof(int32_t));
            uintptr_t foundAddr = pattern2 + 7 + rva;
            
            snprintf(buf, sizeof(buf), "[EntityScanner] EntityList found via pattern 2 at: 0x%llX (Offset: +0x%llX)", 
                static_cast<unsigned long long>(foundAddr),
                static_cast<unsigned long long>(foundAddr - m_clientBase));
            DebugConsole::Log(LogLevel::Success, buf);
            
            m_entityListPtr = foundAddr;
            return true;
        }
    }
    
    // Попытка 2: Тестируем оффсеты из дампа
    DebugConsole::Log(LogLevel::Info, "[EntityScanner] Testing static offsets from dump...");
    
    uintptr_t entityList_v1 = m_clientBase + 0x7157868;
    uintptr_t entityList_v2 = m_clientBase + 0x71598A0;
    
    // Тестируем v1
    std::vector<uint8_t> testBuffer1;
    if (Memory::SafeReadBytes(entityList_v1, 32, testBuffer1) && testBuffer1.size() >= 32) {
        snprintf(buf, sizeof(buf), "[EntityScanner] EntityList_v1 (0x%llX) first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
            static_cast<unsigned long long>(entityList_v1),
            testBuffer1[0], testBuffer1[1], testBuffer1[2], testBuffer1[3],
            testBuffer1[4], testBuffer1[5], testBuffer1[6], testBuffer1[7]);
        DebugConsole::Log(LogLevel::Info, buf);
    }
    
    // Тестируем v2
    std::vector<uint8_t> testBuffer2;
    if (Memory::SafeReadBytes(entityList_v2, 32, testBuffer2) && testBuffer2.size() >= 32) {
        snprintf(buf, sizeof(buf), "[EntityScanner] EntityList_v2 (0x%llX) first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
            static_cast<unsigned long long>(entityList_v2),
            testBuffer2[0], testBuffer2[1], testBuffer2[2], testBuffer2[3],
            testBuffer2[4], testBuffer2[5], testBuffer2[6], testBuffer2[7]);
        DebugConsole::Log(LogLevel::Info, buf);
    }
    
    // Используем v2 по умолчанию
    m_entityListPtr = entityList_v2;
    
    DebugConsole::Log(LogLevel::Warning, "[EntityScanner] Using EntityList_v2, but offsets may be incorrect. Use ReClass.NET to find proper structure!");

    return true;
}

bool EntityScanner::FindViewMatrix() {
    // Попытаемся найти ViewMatrix через паттерн
    // ViewMatrix обычно находится в статической секции client.dll
    // Ищем паттерн: характерные значения матрицы (например, перспективные коэффициенты)
    
    // Для начала пробуем статический оффсет, если он есть
    if (Offsets::view_matrix != 0) {
        m_viewMatrixPtr = m_clientBase + Offsets::view_matrix;
        
        char buf[256];
        snprintf(buf, sizeof(buf), "[EntityScanner] ViewMatrix at: 0x%llX (static offset)",
            static_cast<unsigned long long>(m_viewMatrixPtr));
        DebugConsole::Log(LogLevel::Info, buf);
        return true;
    }

    // Если нет статического оффсета, ищем через патерн
    // Паттерн для ViewMatrix: обычно это 4x4 матрица с характерными значениями
    // Попробуем найти через сигнатуру обращения к матрице
    uintptr_t found = PatternScanner::FindPattern("client.dll", "48 8D 0D ? ? ? ? 48 8B 01 FF 50");
    
    if (found) {
        // Извлекаем RVA из инструкции LEA
        int32_t rva = 0;
        std::vector<uint8_t> buffer;
        if (Memory::SafeReadBytes(found + 3, sizeof(int32_t), buffer) && buffer.size() >= sizeof(int32_t)) {
            memcpy(&rva, buffer.data(), sizeof(int32_t));
            m_viewMatrixPtr = found + 7 + rva; // LEA инструкция: 3 байта опкод + 4 байта RVA + текущий IP
            
            char buf[256];
            snprintf(buf, sizeof(buf), "[EntityScanner] ViewMatrix found via pattern at: 0x%llX",
                static_cast<unsigned long long>(m_viewMatrixPtr));
            DebugConsole::Log(LogLevel::Success, buf);
            return true;
        }
    }

    DebugConsole::Log(LogLevel::Warning, "[EntityScanner] ViewMatrix not found - using mock matrix");
    m_viewMatrixPtr = 0;
    return false;
}

std::vector<PlayerData> EntityScanner::ScanPlayers() {
    // ОТКЛЮЧЕНО: Старый код больше не используется, используем UnityESP
    return std::vector<PlayerData>();
}

std::vector<PlayerData> EntityScanner::GetMockPlayers() {
    static bool loggedOnce = false;
    
    if (!loggedOnce) {
        DebugConsole::Log(LogLevel::Info, "[EntityScanner] Using mock player data for ESP testing (offsets need fixing)");
        loggedOnce = true;
    }
    
    std::vector<PlayerData> players;

    // Мок 1 - Близкий враг
    PlayerData mock1;
    mock1.position = Vec3(100.0f, 200.0f, 50.0f);
    mock1.headPosition = Vec3(100.0f, 200.0f, 122.0f);
    mock1.health = 85.0f;
    mock1.team = 2;
    mock1.isAlive = true;
    mock1.isLocalPlayer = false;
    strncpy_s(mock1.name, sizeof(mock1.name), "Enemy_Close", _TRUNCATE);
    players.push_back(mock1);

    // Мок 2 - Средняя дистанция
    PlayerData mock2;
    mock2.position = Vec3(-150.0f, 300.0f, 50.0f);
    mock2.headPosition = Vec3(-150.0f, 300.0f, 122.0f);
    mock2.health = 50.0f;
    mock2.team = 2;
    mock2.isAlive = true;
    mock2.isLocalPlayer = false;
    strncpy_s(mock2.name, sizeof(mock2.name), "Enemy_Mid", _TRUNCATE);
    players.push_back(mock2);

    // Мок 3 - Далёкий враг
    PlayerData mock3;
    mock3.position = Vec3(250.0f, -400.0f, 50.0f);
    mock3.headPosition = Vec3(250.0f, -400.0f, 122.0f);
    mock3.health = 25.0f;
    mock3.team = 2;
    mock3.isAlive = true;
    mock3.isLocalPlayer = false;
    strncpy_s(mock3.name, sizeof(mock3.name), "Enemy_Far", _TRUNCATE);
    players.push_back(mock3);

    return players;
}

bool EntityScanner::GetViewMatrix(float outMatrix[16]) {
    static auto lastScanTime = std::chrono::steady_clock::now();
    static bool needRescan = true;
    
    // Если ViewMatrix не найдена или нулевая - пробуем пересканировать каждые 5 секунд
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastScanTime).count();
    
    if ((m_viewMatrixPtr == 0 || needRescan) && elapsed >= 5) {
        DebugConsole::Log(LogLevel::Info, "[EntityScanner] Rescanning for ViewMatrix...");
        
        uintptr_t foundViewMatrix = 0;
        if (RuntimeScanner::Get().ScanForViewMatrix(m_clientBase, 0x7A2C000, foundViewMatrix)) {
            m_viewMatrixPtr = foundViewMatrix;
            needRescan = false;
            DebugConsole::Log(LogLevel::Success, "[EntityScanner] ViewMatrix updated via rescan!");
        }
        
        lastScanTime = now;
    }
    
    if (m_viewMatrixPtr == 0) {
        // Возвращаем простую тестовую матрицу
        memset(outMatrix, 0, sizeof(float) * 16);
        
        // Identity матрица для 2D режима
        outMatrix[0] = 1.0f;
        outMatrix[5] = 1.0f;
        outMatrix[10] = 1.0f;
        outMatrix[15] = 1.0f;
        
        return false;
    }

    // Читаем реальную матрицу
    std::vector<uint8_t> buffer;
    if (Memory::SafeReadBytes(m_viewMatrixPtr, sizeof(float) * 16, buffer)) {
        if (buffer.size() >= sizeof(float) * 16) {
            memcpy(outMatrix, buffer.data(), sizeof(float) * 16);
            
            // Проверяем что матрица не нулевая
            bool allZero = true;
            for (int i = 0; i < 16; i++) {
                if (std::abs(outMatrix[i]) > 0.001f) {
                    allZero = false;
                    break;
                }
            }
            
            if (allZero) {
                needRescan = true; // Матрица нулевая, нужен ресканирование
                return false;
            }
            
            return true;
        }
    }

    needRescan = true;
    return false;
}
