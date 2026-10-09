#include "RuntimeScanner.h"
#include "../Memory/Memory.h"
#include "../Console/DebugConsole.h"
#include <cmath>

bool RuntimeScanner::IsValidPointer(uintptr_t ptr) {
    // Проверяем что адрес выглядит как валидный указатель в user-space
    if (ptr < 0x10000) return false; // NULL или маленький адрес
    if (ptr > 0x7FFFFFFFFFFF) return false; // Слишком большой для user-space
    
    // Проверяем что можем читать по этому адресу
    std::vector<uint8_t> test;
    return Memory::SafeReadBytes(ptr, 8, test);
}

bool RuntimeScanner::LooksLikePlayerStruct(uintptr_t addr) {
    if (!IsValidPointer(addr)) return false;
    
    // Читаем предполагаемые поля игрока
    std::vector<uint8_t> buffer;
    if (!Memory::SafeReadBytes(addr, 0x800, buffer)) return false;
    if (buffer.size() < 0x800) return false;
    
    // Проверяем типичные оффсеты для игрока:
    // Health (0x140): должно быть от 0 до 100-200
    float* healthPtr = reinterpret_cast<float*>(&buffer[0x140]);
    float health = *healthPtr;
    if (health < 0.0f || health > 200.0f) return false;
    if (std::isnan(health) || std::isinf(health)) return false;
    
    // Position (0x170): Vec3 - координаты обычно в пределах +-10000
    float* posPtr = reinterpret_cast<float*>(&buffer[0x170]);
    float posX = posPtr[0];
    float posY = posPtr[1];
    float posZ = posPtr[2];
    
    if (std::isnan(posX) || std::isinf(posX)) return false;
    if (std::isnan(posY) || std::isinf(posY)) return false;
    if (std::isnan(posZ) || std::isinf(posZ)) return false;
    
    if (abs(posX) > 50000.0f || abs(posY) > 50000.0f || abs(posZ) > 50000.0f) return false;
    
    return true;
}

bool RuntimeScanner::LooksLikeEntityList(uintptr_t addr) {
    std::vector<uint8_t> buffer;
    if (!Memory::SafeReadBytes(addr, 64 * 8, buffer)) return false; // Читаем 64 указателя
    if (buffer.size() < 64 * 8) return false;
    
    uintptr_t* pointers = reinterpret_cast<uintptr_t*>(buffer.data());
    
    int validPlayers = 0;
    int totalChecked = 0;
    
    // Проверяем первые 32 слота
    for (int i = 0; i < 32; i++) {
        uintptr_t entityPtr = pointers[i];
        
        if (entityPtr == 0) continue; // NULL - пустой слот
        
        totalChecked++;
        
        if (IsValidPointer(entityPtr) && LooksLikePlayerStruct(entityPtr)) {
            validPlayers++;
        }
    }
    
    // Если нашли хотя бы 1-2 валидных игрока - скорее всего это EntityList
    return (validPlayers >= 1 && totalChecked <= 16);
}

bool RuntimeScanner::ScanForEntityList(uintptr_t clientBase, DWORD clientSize, uintptr_t& outEntityList) {
    DebugConsole::Log(LogLevel::Info, "[RuntimeScanner] Scanning for EntityList in client.dll...");
    
    // Читаем память большими блоками для скорости
    const size_t SCAN_CHUNK = 1024 * 1024; // 1 MB
    std::vector<uint8_t> buffer(SCAN_CHUNK);
    
    int candidatesFound = 0;
    
    for (size_t offset = 0; offset < clientSize && offset + SCAN_CHUNK <= clientSize; offset += SCAN_CHUNK / 2) {
        if (!Memory::SafeReadBytes(clientBase + offset, SCAN_CHUNK, buffer)) continue;
        
        // Сканируем каждый указатель в блоке
        uintptr_t* pointers = reinterpret_cast<uintptr_t*>(buffer.data());
        size_t numPointers = SCAN_CHUNK / sizeof(uintptr_t);
        
        for (size_t i = 0; i < numPointers; i++) {
            uintptr_t ptr = pointers[i];
            
            // Проверяем выглядит ли этот адрес как EntityList
            if (IsValidPointer(ptr) && LooksLikeEntityList(ptr)) {
                uintptr_t candidateAddr = clientBase + offset + (i * sizeof(uintptr_t));
                
                char buf[256];
                snprintf(buf, sizeof(buf), "[RuntimeScanner] FOUND EntityList candidate at RVA: 0x%llX (Addr: 0x%llX)",
                    static_cast<unsigned long long>(candidateAddr - clientBase),
                    static_cast<unsigned long long>(candidateAddr));
                DebugConsole::Log(LogLevel::Success, buf);
                
                outEntityList = candidateAddr;
                candidatesFound++;
                
                if (candidatesFound >= 1) return true; // Берем первый найденный
            }
        }
    }
    
    if (candidatesFound == 0) {
        DebugConsole::Log(LogLevel::Warning, "[RuntimeScanner] EntityList not found - maybe not in match?");
    }
    
    return candidatesFound > 0;
}

bool RuntimeScanner::ScanForLocalPlayer(uintptr_t clientBase, DWORD clientSize, uintptr_t& outLocalPlayer) {
    DebugConsole::Log(LogLevel::Info, "[RuntimeScanner] Scanning for LocalPlayer pointer...");
    
    // LocalPlayer обычно рядом с EntityList
    // Ищем указатели на структуры игроков в статической секции
    
    const size_t SCAN_SIZE = 10 * 1024 * 1024; // Первые 10 MB
    std::vector<uint8_t> buffer;
    
    if (!Memory::SafeReadBytes(clientBase, SCAN_SIZE, buffer)) return false;
    
    uintptr_t* pointers = reinterpret_cast<uintptr_t*>(buffer.data());
    size_t numPointers = buffer.size() / sizeof(uintptr_t);
    
    for (size_t i = 0; i < numPointers; i++) {
        uintptr_t ptr = pointers[i];
        
        if (IsValidPointer(ptr) && LooksLikePlayerStruct(ptr)) {
            uintptr_t candidateAddr = clientBase + (i * sizeof(uintptr_t));
            
            char buf[256];
            snprintf(buf, sizeof(buf), "[RuntimeScanner] FOUND LocalPlayer candidate at RVA: 0x%llX",
                static_cast<unsigned long long>(candidateAddr - clientBase));
            DebugConsole::Log(LogLevel::Success, buf);
            
            outLocalPlayer = candidateAddr;
            return true;
        }
    }
    
    DebugConsole::Log(LogLevel::Warning, "[RuntimeScanner] LocalPlayer not found");
    return false;
}

bool RuntimeScanner::LooksLikeViewMatrix(float* matrix) {
    // ViewMatrix - это 4x4 матрица перспективной проекции
    // Характерные признаки:
    // - Диагональные элементы обычно ненулевые
    // - Элемент [3][3] обычно 0 для перспективной проекции
    // - Значения в разумных пределах
    
    for (int i = 0; i < 16; i++) {
        if (std::isnan(matrix[i]) || std::isinf(matrix[i])) return false;
        if (abs(matrix[i]) > 10000.0f) return false; // Слишком большие значения
    }
    
    // Проверяем что хотя бы несколько диагональных элементов ненулевые
    int nonZeroDiag = 0;
    for (int i = 0; i < 4; i++) {
        if (abs(matrix[i * 4 + i]) > 0.001f) nonZeroDiag++;
    }
    
    if (nonZeroDiag < 2) return false;
    
    // Дополнительная проверка: ViewMatrix для перспективной проекции
    // обычно имеет [3][3] = 0 или близкое к 0, и [3][2] = -1 или близкое
    // Но эти проверки не обязательны, главное - матрица не пустая и не битая
    
    return true;
}

bool RuntimeScanner::ScanForViewMatrix(uintptr_t clientBase, DWORD clientSize, uintptr_t& outViewMatrix) {
    DebugConsole::Log(LogLevel::Info, "[RuntimeScanner] Scanning for ViewMatrix...");
    
    const size_t SCAN_SIZE = std::min<size_t>(clientSize, 50 * 1024 * 1024); // Макс 50 MB
    std::vector<uint8_t> buffer;
    
    if (!Memory::SafeReadBytes(clientBase, SCAN_SIZE, buffer)) {
        DebugConsole::Log(LogLevel::Error, "[RuntimeScanner] Failed to read client.dll memory!");
        return false;
    }
    
    int candidatesFound = 0;
    
    // ViewMatrix - это 16 флоатов (64 байта)
    for (size_t offset = 0; offset < buffer.size() - 64; offset += 16) {
        float* matrix = reinterpret_cast<float*>(&buffer[offset]);
        
        // Объявляем все переменные заранее чтобы избежать ошибок с goto/continue
        bool allZero = true;
        int validCount = 0;
        int nonZeroDiag = 0;
        float scaleSum = 0.0f;
        
        // Базовая валидация
        for (int i = 0; i < 16; i++) {
            if (std::isnan(matrix[i]) || std::isinf(matrix[i])) continue;
            if (abs(matrix[i]) > 10000.0f) continue;
            if (abs(matrix[i]) > 0.001f) {
                allZero = false;
                validCount++;
            }
        }
        
        if (allZero || validCount < 4) continue;
        
        // Проверяем что это похоже на ViewMatrix:
        // 1. Хотя бы 2 диагональных элемента ненулевые
        for (int i = 0; i < 4; i++) {
            if (abs(matrix[i * 4 + i]) > 0.1f) nonZeroDiag++;
        }
        if (nonZeroDiag < 2) continue;
        
        // 2. [3][3] (индекс 15) должен быть близок к 0 для перспективной проекции
        if (abs(matrix[15]) > 1.0f) continue;
        
        // 3. Проверяем что есть rotation/scale компоненты
        for (int i = 0; i < 9; i++) {
            scaleSum += abs(matrix[i]);
        }
        if (scaleSum < 0.5f || scaleSum > 100.0f) continue;
        
        // Нашли кандидата!
        uintptr_t candidateAddr = clientBase + offset;
        
        char buf[512];
        snprintf(buf, sizeof(buf), "[RuntimeScanner] ViewMatrix candidate #%d at RVA: 0x%llX",
            candidatesFound + 1,
            static_cast<unsigned long long>(candidateAddr - clientBase));
        DebugConsole::Log(LogLevel::Success, buf);
        
        // Логируем матрицу для анализа
        snprintf(buf, sizeof(buf), "  [0-3 ]: %.3f, %.3f, %.3f, %.3f",
            matrix[0], matrix[1], matrix[2], matrix[3]);
        DebugConsole::Log(LogLevel::Info, buf);
        
        snprintf(buf, sizeof(buf), "  [4-7 ]: %.3f, %.3f, %.3f, %.3f",
            matrix[4], matrix[5], matrix[6], matrix[7]);
        DebugConsole::Log(LogLevel::Info, buf);
        
        snprintf(buf, sizeof(buf), "  [8-11]: %.3f, %.3f, %.3f, %.3f",
            matrix[8], matrix[9], matrix[10], matrix[11]);
        DebugConsole::Log(LogLevel::Info, buf);
        
        snprintf(buf, sizeof(buf), "  [12-15]: %.3f, %.3f, %.3f, %.3f",
            matrix[12], matrix[13], matrix[14], matrix[15]);
        DebugConsole::Log(LogLevel::Info, buf);
        
        if (candidatesFound == 0) {
            outViewMatrix = candidateAddr; // Берем первый найденный
        }
        
        candidatesFound++;
        if (candidatesFound >= 5) {
            DebugConsole::Log(LogLevel::Info, "[RuntimeScanner] Found 5 candidates, stopping scan. Using first one.");
            return true;
        }
    }
    
    if (candidatesFound > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[RuntimeScanner] Found %d ViewMatrix candidates total", candidatesFound);
        DebugConsole::Log(LogLevel::Success, buf);
        return true;
    }
    
    DebugConsole::Log(LogLevel::Error, "[RuntimeScanner] ViewMatrix not found!");
    return false;
}
