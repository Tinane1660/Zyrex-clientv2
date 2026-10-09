#include "IL2CPPResolver.h"
#include "EntityScanner.h"
#include <fstream>
#include <sstream>

bool IL2CPPResolver::Initialize() {
    if (isInitialized) return true;

    DebugConsole::Log(LogLevel::Info, "[IL2CPP] Searching for GameAssembly.dll / client.dll...");

    // Try GameAssembly.dll first (standard IL2CPP), then client.dll (Standknife renamed)
    hGameAssembly = GetModuleHandleA("GameAssembly.dll");
    if (!hGameAssembly) {
        hGameAssembly = GetModuleHandleA("client.dll");
    }
    if (!hGameAssembly) {
        hGameAssembly = GetModuleHandleA("gameassembly.dll");
    }

    if (!hGameAssembly) {
        DebugConsole::Log(LogLevel::Error, "[IL2CPP] GameAssembly/client.dll not found!");
        return false;
    }

    char modPath[MAX_PATH];
    GetModuleFileNameA(hGameAssembly, modPath, MAX_PATH);
    DebugConsole::Log(LogLevel::Success, std::string("[IL2CPP] Found module: ") + modPath);

    // Resolve IL2CPP exported API functions
    fn_domain_get              = (il2cpp_domain_get_t)GetProcAddress(hGameAssembly, "il2cpp_domain_get");
    fn_domain_get_assemblies   = (il2cpp_domain_get_assemblies_t)GetProcAddress(hGameAssembly, "il2cpp_domain_get_assemblies");
    fn_assembly_get_image      = (il2cpp_assembly_get_image_t)GetProcAddress(hGameAssembly, "il2cpp_assembly_get_image");
    fn_class_from_name         = (il2cpp_class_from_name_t)GetProcAddress(hGameAssembly, "il2cpp_class_from_name");
    fn_class_get_fields        = (il2cpp_class_get_fields_t)GetProcAddress(hGameAssembly, "il2cpp_class_get_fields");
    fn_class_get_methods       = (il2cpp_class_get_methods_t)GetProcAddress(hGameAssembly, "il2cpp_class_get_methods");
    fn_field_get_name          = (il2cpp_field_get_name_t)GetProcAddress(hGameAssembly, "il2cpp_field_get_name");
    fn_field_get_offset        = (il2cpp_field_get_offset_t)GetProcAddress(hGameAssembly, "il2cpp_field_get_offset");
    fn_method_get_name         = (il2cpp_method_get_name_t)GetProcAddress(hGameAssembly, "il2cpp_method_get_name");
    fn_class_get_name          = (il2cpp_class_get_name_t)GetProcAddress(hGameAssembly, "il2cpp_class_get_name");
    fn_class_get_namespace     = (il2cpp_class_get_namespace_t)GetProcAddress(hGameAssembly, "il2cpp_class_get_namespace");
    fn_class_get_parent        = (il2cpp_class_get_parent_t)GetProcAddress(hGameAssembly, "il2cpp_class_get_parent");
    fn_image_get_class         = (il2cpp_image_get_class_t)GetProcAddress(hGameAssembly, "il2cpp_image_get_class");
    fn_image_get_class_count   = (il2cpp_image_get_class_count_t)GetProcAddress(hGameAssembly, "il2cpp_image_get_class_count");
    fn_image_get_name          = (il2cpp_image_get_name_t)GetProcAddress(hGameAssembly, "il2cpp_image_get_name");
    fn_field_get_flags         = (il2cpp_field_get_flags_t)GetProcAddress(hGameAssembly, "il2cpp_field_get_flags");

    if (!fn_domain_get || !fn_domain_get_assemblies || !fn_assembly_get_image) {
        DebugConsole::Log(LogLevel::Error, "[IL2CPP] Failed to resolve core IL2CPP API functions! (ByteGuard may be blocking exports)");
        return false;
    }

    DebugConsole::Log(LogLevel::Success, "[IL2CPP] Core API functions resolved!");

    char buf[256];
    snprintf(buf, sizeof(buf), "[IL2CPP] il2cpp_domain_get: 0x%p", fn_domain_get);
    DebugConsole::Log(LogLevel::Info, buf);
    snprintf(buf, sizeof(buf), "[IL2CPP] il2cpp_class_from_name: 0x%p", fn_class_from_name);
    DebugConsole::Log(LogLevel::Info, buf);

    isInitialized = true;
    return true;
}

void* IL2CPPResolver::FindAssemblyImage(const char* assemblyName) {
    if (!fn_domain_get || !fn_domain_get_assemblies || !fn_assembly_get_image) return nullptr;

    void* domain = fn_domain_get();
    if (!domain) return nullptr;

    size_t numAssemblies = 0;
    void** assemblies = fn_domain_get_assemblies(domain, &numAssemblies);
    if (!assemblies) return nullptr;

    for (size_t i = 0; i < numAssemblies; i++) {
        void* image = fn_assembly_get_image(assemblies[i]);
        if (!image || !fn_image_get_name) continue;

        const char* imgName = fn_image_get_name(image);
        if (imgName && strstr(imgName, assemblyName)) {
            return image;
        }
    }
    return nullptr;
}

void* IL2CPPResolver::FindClassInImage(void* image, const char* namespaze, const char* className) {
    if (!image) return nullptr;

    // Try direct lookup first
    if (fn_class_from_name) {
        void* klass = fn_class_from_name(image, namespaze, className);
        if (klass) return klass;
    }

    // Fallback: enumerate all classes
    if (fn_image_get_class && fn_image_get_class_count && fn_class_get_name) {
        size_t count = fn_image_get_class_count(image);
        for (size_t i = 0; i < count; i++) {
            void* klass = fn_image_get_class(image, i);
            if (!klass) continue;

            const char* name = fn_class_get_name(klass);
            if (name && strcmp(name, className) == 0) {
                if (fn_class_get_namespace) {
                    const char* ns = fn_class_get_namespace(klass);
                    if (namespaze[0] == '\0' || (ns && strcmp(ns, namespaze) == 0)) {
                        return klass;
                    }
                } else {
                    return klass;
                }
            }
        }
    }

    return nullptr;
}

bool IL2CPPResolver::ResolveOffsets() {
    if (!isInitialized) return false;
    if (offsets.resolved) return true;

    DebugConsole::Log(LogLevel::Info, "[IL2CPP] Resolving game class offsets...");

    // Find Assembly-CSharp image
    void* csharpImage = FindAssemblyImage("Assembly-CSharp");
    if (!csharpImage) {
        DebugConsole::Log(LogLevel::Warning, "[IL2CPP] Assembly-CSharp not found. Trying alternative names...");
        csharpImage = FindAssemblyImage("Assembly");
        if (!csharpImage) {
            DebugConsole::Log(LogLevel::Error, "[IL2CPP] Could not find game assembly image!");
            return false;
        }
    }

    DebugConsole::Log(LogLevel::Success, "[IL2CPP] Found game assembly image!");

    // Search for player-related classes with common Unity FPS naming patterns
    const char* playerClassNames[] = {
        "PlayerController", "Player", "NetworkPlayer", "PlayerEntity",
        "CharacterController", "FPSPlayer", "PlayerMovement",
        "PlayerHealth", "PlayerStats", "BasePlayer", "PhotonPlayer",
        "PlayerManager", "GamePlayer", nullptr
    };

    for (int i = 0; playerClassNames[i] != nullptr; i++) {
        void* klass = FindClassInImage(csharpImage, "", playerClassNames[i]);
        if (klass) {
            char logBuf[256];
            snprintf(logBuf, sizeof(logBuf), "[IL2CPP] Found class: %s", playerClassNames[i]);
            DebugConsole::Log(LogLevel::Success, logBuf);
            DumpClassFields(klass);

            if (!offsets.playerClass) {
                offsets.playerClass = klass;
            }
        }
    }

    offsets.resolved = true;
    DebugConsole::Log(LogLevel::Success, "[IL2CPP] Offset resolution complete!");
    return true;
}

void IL2CPPResolver::DumpClassFields(void* klass) {
    if (!klass || !fn_class_get_fields || !fn_field_get_name || !fn_field_get_offset) return;

    const char* className = fn_class_get_name ? fn_class_get_name(klass) : "Unknown";

    void* iter = nullptr;
    void* field = nullptr;

    while ((field = fn_class_get_fields(klass, &iter)) != nullptr) {
        const char* fieldName = fn_field_get_name(field);
        size_t fieldOffset = fn_field_get_offset(field);

        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "  [Field] %s::%s -> Offset: 0x%llX",
            className, fieldName ? fieldName : "null",
            static_cast<unsigned long long>(fieldOffset));
        DebugConsole::Log(LogLevel::Info, logBuf);

        // Auto-detect common field names for player data
        if (fieldName) {
            std::string fn(fieldName);
            // Lowercase comparison
            for (auto& c : fn) c = static_cast<char>(tolower(c));

            if (fn.find("health") != std::string::npos || fn.find("hp") != std::string::npos) {
                offsets.playerHealth = fieldOffset;
                DebugConsole::Log(LogLevel::Success, "    -> Auto-mapped as PLAYER HEALTH offset!");
            }
            if (fn.find("team") != std::string::npos || fn.find("side") != std::string::npos) {
                offsets.playerTeam = fieldOffset;
                DebugConsole::Log(LogLevel::Success, "    -> Auto-mapped as PLAYER TEAM offset!");
            }
            if (fn.find("alive") != std::string::npos || fn.find("dead") != std::string::npos || fn.find("isdead") != std::string::npos) {
                offsets.playerIsAlive = fieldOffset;
                DebugConsole::Log(LogLevel::Success, "    -> Auto-mapped as PLAYER ALIVE offset!");
            }
            if (fn.find("position") != std::string::npos || fn.find("pos") != std::string::npos) {
                offsets.playerPosition = fieldOffset;
                DebugConsole::Log(LogLevel::Success, "    -> Auto-mapped as PLAYER POSITION offset!");
            }
            if (fn.find("nickname") != std::string::npos || fn.find("username") != std::string::npos || fn.find("playername") != std::string::npos) {
                offsets.playerName = fieldOffset;
                DebugConsole::Log(LogLevel::Success, "    -> Auto-mapped as PLAYER NAME offset!");
            }
        }
    }

    // Dump methods too
    if (fn_class_get_methods && fn_method_get_name) {
        void* mIter = nullptr;
        void* method = nullptr;
        while ((method = fn_class_get_methods(klass, &mIter)) != nullptr) {
            const char* methodName = fn_method_get_name(method);
            char logBuf[256];
            snprintf(logBuf, sizeof(logBuf), "  [Method] %s::%s -> 0x%p",
                className, methodName ? methodName : "null", method);
            DebugConsole::Log(LogLevel::Info, logBuf);
        }
    }
}

void IL2CPPResolver::DumpAllClasses(const std::string& outputFile) {
    if (!isInitialized || !fn_domain_get || !fn_domain_get_assemblies) {
        DebugConsole::Log(LogLevel::Error, "[IL2CPP] Cannot dump: resolver not initialized!");
        return;
    }

    std::ofstream outFile(outputFile);
    outFile << "// gandon.cc Standknife IL2CPP Class Dump\n";
    outFile << "// Generated with UnityCrashHandler64 bypass\n\n";

    void* domain = fn_domain_get();
    if (!domain) {
        DebugConsole::Log(LogLevel::Error, "[IL2CPP] il2cpp_domain_get() returned NULL!");
        return;
    }

    size_t numAssemblies = 0;
    void** assemblies = fn_domain_get_assemblies(domain, &numAssemblies);

    char buf[256];
    snprintf(buf, sizeof(buf), "[IL2CPP] Found %llu assemblies in domain", static_cast<unsigned long long>(numAssemblies));
    DebugConsole::Log(LogLevel::Info, buf);

    int totalClasses = 0;

    for (size_t a = 0; a < numAssemblies; a++) {
        void* image = fn_assembly_get_image(assemblies[a]);
        if (!image) continue;

        const char* imgName = fn_image_get_name ? fn_image_get_name(image) : "Unknown";
        outFile << "// ============================================\n";
        outFile << "// Assembly: " << imgName << "\n";
        outFile << "// ============================================\n\n";

        if (!fn_image_get_class || !fn_image_get_class_count) continue;

        size_t classCount = fn_image_get_class_count(image);
        for (size_t c = 0; c < classCount; c++) {
            void* klass = fn_image_get_class(image, c);
            if (!klass) continue;

            const char* className = fn_class_get_name ? fn_class_get_name(klass) : "???";
            const char* classNs = fn_class_get_namespace ? fn_class_get_namespace(klass) : "";

            outFile << "class " << (classNs[0] ? classNs : "") << (classNs[0] ? "." : "") << className << " {\n";

            // Dump fields
            if (fn_class_get_fields && fn_field_get_name && fn_field_get_offset) {
                void* iter = nullptr;
                void* field = nullptr;
                while ((field = fn_class_get_fields(klass, &iter)) != nullptr) {
                    const char* fieldName = fn_field_get_name(field);
                    size_t fieldOff = fn_field_get_offset(field);
                    outFile << "    [0x" << std::hex << fieldOff << "] " << (fieldName ? fieldName : "null") << "\n";
                }
            }

            // Dump methods
            if (fn_class_get_methods && fn_method_get_name) {
                void* mIter = nullptr;
                void* method = nullptr;
                while ((method = fn_class_get_methods(klass, &mIter)) != nullptr) {
                    const char* mName = fn_method_get_name(method);
                    outFile << "    [Method] " << (mName ? mName : "null") << " -> 0x" << std::hex << reinterpret_cast<uintptr_t>(method) << "\n";
                }
            }

            outFile << "}\n\n";
            totalClasses++;
        }
    }

    outFile.close();

    snprintf(buf, sizeof(buf), "[IL2CPP] Dumped %d classes to '%s'", totalClasses, outputFile.c_str());
    DebugConsole::Log(LogLevel::Success, buf);
}

std::vector<PlayerData> IL2CPPResolver::GetPlayers() {
    // Use EntityScanner for now (returns mock data for testing)
    return EntityScanner::Get().ScanPlayers();
}

bool IL2CPPResolver::GetCameraMatrix(float outMatrix[16]) {
    // Use EntityScanner for matrix (returns mock matrix for testing)
    return EntityScanner::Get().GetViewMatrix(outMatrix);
}
