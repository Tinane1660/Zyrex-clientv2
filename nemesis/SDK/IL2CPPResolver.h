#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../Console/DebugConsole.h"

// IL2CPP Runtime API Function Typedefs
typedef void* (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void* domain, size_t* size);
typedef void* (*il2cpp_assembly_get_image_t)(void* assembly);
typedef void* (*il2cpp_class_from_name_t)(void* image, const char* namespaze, const char* name);
typedef void* (*il2cpp_class_get_fields_t)(void* klass, void** iter);
typedef void* (*il2cpp_class_get_methods_t)(void* klass, void** iter);
typedef const char* (*il2cpp_field_get_name_t)(void* field);
typedef size_t (*il2cpp_field_get_offset_t)(void* field);
typedef const char* (*il2cpp_method_get_name_t)(void* method);
typedef const char* (*il2cpp_class_get_name_t)(void* klass);
typedef const char* (*il2cpp_class_get_namespace_t)(void* klass);
typedef void* (*il2cpp_class_get_parent_t)(void* klass);
typedef void* (*il2cpp_image_get_class_t)(void* image, size_t index);
typedef size_t (*il2cpp_image_get_class_count_t)(void* image);
typedef const char* (*il2cpp_image_get_name_t)(void* image);
typedef void* (*il2cpp_type_get_object_t)(void* type);
typedef void* (*il2cpp_class_get_type_t)(void* klass);
typedef int (*il2cpp_field_get_flags_t)(void* field);

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct PlayerData {
    Vec3 position;
    Vec3 headPosition;
    float health;
    int team;
    bool isAlive;
    bool isLocalPlayer;
    char name[64];
};

struct IL2CPPOffsets {
    // Dynamically resolved offsets
    size_t playerHealth      = 0;
    size_t playerTeam        = 0;
    size_t playerPosition    = 0;
    size_t playerIsAlive     = 0;
    size_t playerName        = 0;
    size_t transformPosition = 0;

    // Class pointers
    void* playerClass        = nullptr;
    void* cameraClass        = nullptr;
    void* transformClass     = nullptr;

    bool resolved = false;
};

class IL2CPPResolver {
public:
    static IL2CPPResolver& Get() {
        static IL2CPPResolver instance;
        return instance;
    }

    bool Initialize();
    bool ResolveOffsets();
    void DumpAllClasses(const std::string& outputFile);
    void DumpClassFields(void* klass);

    // Player scanning
    std::vector<PlayerData> GetPlayers();
    bool GetCameraMatrix(float outMatrix[16]);

    IL2CPPOffsets offsets;
    bool isInitialized = false;

private:
    IL2CPPResolver() = default;
    HMODULE hGameAssembly = nullptr;

    // IL2CPP API function pointers
    il2cpp_domain_get_t                fn_domain_get = nullptr;
    il2cpp_domain_get_assemblies_t     fn_domain_get_assemblies = nullptr;
    il2cpp_assembly_get_image_t        fn_assembly_get_image = nullptr;
    il2cpp_class_from_name_t           fn_class_from_name = nullptr;
    il2cpp_class_get_fields_t          fn_class_get_fields = nullptr;
    il2cpp_class_get_methods_t         fn_class_get_methods = nullptr;
    il2cpp_field_get_name_t            fn_field_get_name = nullptr;
    il2cpp_field_get_offset_t          fn_field_get_offset = nullptr;
    il2cpp_method_get_name_t           fn_method_get_name = nullptr;
    il2cpp_class_get_name_t            fn_class_get_name = nullptr;
    il2cpp_class_get_namespace_t       fn_class_get_namespace = nullptr;
    il2cpp_class_get_parent_t          fn_class_get_parent = nullptr;
    il2cpp_image_get_class_t           fn_image_get_class = nullptr;
    il2cpp_image_get_class_count_t     fn_image_get_class_count = nullptr;
    il2cpp_image_get_name_t            fn_image_get_name = nullptr;
    il2cpp_field_get_flags_t           fn_field_get_flags = nullptr;

    void* FindAssemblyImage(const char* assemblyName);
    void* FindClassInImage(void* image, const char* namespaze, const char* className);
};
