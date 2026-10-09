#pragma once
#include <Windows.h>
#include <cstdint>

namespace stealth {
    // Прячет модуль из процесса:
    //  1) анлинк из трёх списков PEB->Ldr + хеш-таблицы LdrpHashTable
    //  2) затирание путей/имён в записи LDR
    //  3) обнуление PE-заголовков в памяти (дамп по заголовкам больше не работает)
    // Вызывать один раз, ПОСЛЕ полной инициализации (хуки, импорт, CRT).
    void hide_self(uintptr_t image_base);
}
