#include "stealth.hpp"
#include <intrin.h>
#include <cstddef>

namespace stealth {

#pragma pack(push, 8)
    typedef struct _MY_PEB_LDR_DATA {
        ULONG Length;
        BOOLEAN Initialized;
        HANDLE SsHandle;
        LIST_ENTRY InLoadOrderModuleList;
        LIST_ENTRY InMemoryOrderModuleList;
        LIST_ENTRY InInitializationOrderModuleList;
        PVOID EntryInProgress;
    } MY_PEB_LDR_DATA;

    typedef struct _MY_US {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    } MY_US;

    // x64: смещения первых трёх LINK_ENTRY стабильны на Win7..Win11
    typedef struct _MY_LDR_ENTRY {
        LIST_ENTRY InLoadOrderLinks;            // +0x00
        LIST_ENTRY InMemoryOrderLinks;          // +0x10
        LIST_ENTRY InInitializationOrderLinks;  // +0x20
        PVOID DllBase;                          // +0x30
        PVOID EntryPoint;                       // +0x38
        ULONG SizeOfImage;                      // +0x40
        MY_US FullDllName;
        MY_US BaseDllName;
        ULONG Flags;                            // +0x68
        USHORT ObsoleteLoadCount;               // +0x6C
        USHORT TlsIndex;                        // +0x6E
        LIST_ENTRY HashLinks;                   // +0x70
    } MY_LDR_ENTRY;
#pragma pack(pop)

    static void UnlinkEntry(LIST_ENTRY* entry) {
        __try {
            entry->Blink->Flink = entry->Flink;
            entry->Flink->Blink = entry->Blink;
            entry->Flink = entry;
            entry->Blink = entry;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void hide_self(uintptr_t image_base) {
        if (!image_base) return;

        __try {
#ifdef _WIN64
            const uintptr_t peb = __readgsqword(0x60);
#else
            const uintptr_t peb = __readfsdword(0x30);
#endif
            if (!peb) return;

            MY_PEB_LDR_DATA* ldr = *(MY_PEB_LDR_DATA**)(peb + 0x18);
            if (!ldr) return;

            MY_LDR_ENTRY* found = nullptr;
            LIST_ENTRY* lists[3] = {
                &ldr->InLoadOrderModuleList,
                &ldr->InMemoryOrderModuleList,
                &ldr->InInitializationOrderModuleList
            };

            for (LIST_ENTRY* head : lists) {
                for (LIST_ENTRY* it = head->Flink; it && it != head; it = it->Flink) {
                    // каждый список ссылается на своё поле одной и той же записи
                    auto e = (MY_LDR_ENTRY*)((uint8_t*)it -
                        ((head == &ldr->InLoadOrderModuleList) ? offsetof(MY_LDR_ENTRY, InLoadOrderLinks) :
                        (head == &ldr->InMemoryOrderModuleList) ? offsetof(MY_LDR_ENTRY, InMemoryOrderLinks) :
                        offsetof(MY_LDR_ENTRY, InInitializationOrderLinks)));
                    if ((uintptr_t)e->DllBase == image_base) { found = e; break; }
                }
                if (found) break;
            }

            if (!found) return;

            // 1) анлинк из всех трёх списков
            UnlinkEntry(&found->InLoadOrderLinks);
            UnlinkEntry(&found->InMemoryOrderLinks);
            UnlinkEntry(&found->InInitializationOrderLinks);

            // 2) анлинк из хеш-таблицы LdrpHashTable (ломает GetModuleHandle по имени)
            UnlinkEntry(&found->HashLinks);

            // 3) затереть имена/пути в записи
            if (found->FullDllName.Buffer && found->FullDllName.Length)
                SecureZeroMemory(found->FullDllName.Buffer, found->FullDllName.MaximumLength);
            if (found->BaseDllName.Buffer && found->BaseDllName.Length)
                SecureZeroMemory(found->BaseDllName.Buffer, found->BaseDllName.MaximumLength);
            found->FullDllName.Length = 0;
            found->FullDllName.MaximumLength = 0;
            found->BaseDllName.Length = 0;
            found->BaseDllName.MaximumLength = 0;
            found->EntryPoint = nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        // 4) обнулить PE-заголовок в памяти (первая страница: DOS+NT+секции)
        __try {
            DWORD old = 0;
            if (VirtualProtect((void*)image_base, 0x1000, PAGE_READWRITE, &old)) {
                SecureZeroMemory((void*)image_base, 0x1000);
                VirtualProtect((void*)image_base, 0x1000, old, &old);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
