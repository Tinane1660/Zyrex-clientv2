#pragma once
#include <cstdint>
#include <cmath>

// Shared math / entity types used by the ESP, aimbot and the auto-calibration engine.

struct Vec3 {
    float x, y, z;
    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s)       const { return Vec3(x * s, y * s, z * s); }

    float LengthSqr() const { return x * x + y * y + z * z; }
    float Length()    const { return std::sqrt(LengthSqr()); }
};

inline float Vec3Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Vec3Normalize(const Vec3& v) {
    float len = v.Length();
    if (len < 1e-6f) return Vec3();
    return Vec3(v.x / len, v.y / len, v.z / len);
}

struct PlayerData {
    Vec3 position;        // world position as read from the entity
    Vec3 headPosition;    // position + up * height
    float health = -1.0f; // < 0 => unknown
    int team = -1;        // < 0 => unknown
    float distance = 0.0f;
    bool isAlive = true;
    bool isLocalPlayer = false;
    bool isEnemy = true;
    uintptr_t address = 0;
    char name[64] = "player";
};
