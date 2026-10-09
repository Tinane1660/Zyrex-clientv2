#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// Runtime memory scanner для поиска EntityList во время игры
class RuntimeScanner {
public:
    static RuntimeScanner& Get() {
        static RuntimeScanner instance;
        return instance;
    }

    // Сканирует память в поисках EntityList с реальными игроками
    bool ScanForEntityList(uintptr_t clientBase, DWORD clientSize, uintptr_t& outEntityList);
    
    // Сканирует память в поисках LocalPlayer
    bool ScanForLocalPlayer(uintptr_t clientBase, DWORD clientSize, uintptr_t& outLocalPlayer);
    
    // Сканирует память в поисках ViewMatrix (4x4 матрица флоатов)
    bool ScanForViewMatrix(uintptr_t clientBase, DWORD clientSize, uintptr_t& outViewMatrix);

private:
    RuntimeScanner() = default;
    
    // Проверяет, похож ли адрес на валидный указатель
    bool IsValidPointer(uintptr_t ptr);
    
    // Проверяет, похож ли массив на EntityList (массив указателей на игроков)
    bool LooksLikeEntityList(uintptr_t addr);
    
    // Проверяет, похожа ли структура на игрока
    bool LooksLikePlayerStruct(uintptr_t addr);
    
    // Проверяет, похож ли массив флоатов на ViewMatrix
    bool LooksLikeViewMatrix(float* matrix);
};
