#include "AutoESP.h"
#include "../Console/DebugConsole.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace AutoESP {

    // ---------------- tunables ----------------
    float g_scanRadius       = 3.0f;
    float g_entityHeight     = 1.85f;
    float g_entityFootOffset = 0.0f;
    int   g_upAxisOverride   = 1;
    bool  g_autoFields       = true;
    // Do not hide a newly discovered player until the movement sampler sees it.
    // The old default made a valid model render an empty ESP for up to a second.
    bool  g_onlyMoving       = false;

    // ---------------- internal state ----------------
    struct EntityModel {
        uintptr_t klass   = 0;
        int posOffset     = -1;
        int healthOffset  = -1;
        int teamOffset    = -1;
        bool healthIsFloat = true;
        bool strict       = false;
    };

    static std::mutex              g_lock;
    static std::unordered_set<uintptr_t> g_movedSet;   // objects seen moving at least once
    static std::unordered_set<uint64_t>  g_rejected;   // (class, offset) pairs proven dead

    // Fallback tracking: raw addresses that were seen holding a moving world
    // coordinate. No class, no offsets - whatever these point at gets clustered
    // and boxed, so a crosshair scan always produces something visible even when
    // the object layout cannot be resolved.
    struct RawPoint {
        uintptr_t addr = 0;
        Vec3      pos;
    };
    static std::vector<RawPoint> g_rawPoints;
    static Status                  g_status;
    static EntityModel             g_model;
    static std::vector<uintptr_t>  g_objects;

    static std::atomic<uintptr_t>  g_matrixAddr{ 0 };
    static std::atomic<int>        g_matrixLayout{ 0 };   // 0 = unity column major, 1 = transposed
    static std::atomic<int>        g_upAxis{ 1 };
    static std::atomic<bool>       g_running{ false };
    static std::atomic<bool>       g_wantFullRecal{ false };
    static std::atomic<bool>       g_wantEntityRescan{ false };
    static std::atomic<bool>       g_wantTargetScan{ false };
    static std::atomic<float>      g_screenW{ 1920.0f };
    static std::atomic<float>      g_screenH{ 1080.0f };
    static HANDLE                  g_thread = nullptr;

    static const size_t kMaxRegionSize = 1024ull * 1024 * 1024;
    static const size_t kChunk        = 64 * 1024;
    static const size_t kMaxPosCand   = 3000000;
    static const size_t kMaxMoving    = 24000;
    static const int    kFieldWindow  = 0x400;

    // How close our own player object is expected to sit to the eye. Exposed in
    // the menu because a game may keep the position at the feet, at the object
    // root or somewhere else entirely.
    static inline float SelfRadius() {
        float r = g_scanRadius;
        if (r < 0.5f)  r = 0.5f;
        if (r > 15.0f) r = 15.0f;
        return r;
    }

    // ============================================================
    //  raw memory access (SEH guarded leaf helpers)
    // ============================================================
    static bool RawCopy(void* dst, const void* src, size_t size) {
        __try {
            memcpy(dst, src, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename T>
    static bool Read(uintptr_t address, T& out) {
        if (address < 0x10000) return false;
        return RawCopy(&out, reinterpret_cast<const void*>(address), sizeof(T));
    }

    static bool ReadBlock(uintptr_t address, void* dst, size_t size) {
        if (address < 0x10000) return false;
        return RawCopy(dst, reinterpret_cast<const void*>(address), size);
    }

    // A pointer that looks like it could be an Il2CppClass*: the first fields of
    // Il2CppClass are image*, gc_desc*, name*, namespaze* - so we can validate a
    // candidate by checking that it owns two readable C strings.
    static bool IsAsciiString(uintptr_t address, int minLen, int maxLen) {
        if (address < 0x10000) return false;
        char buf[132];
        int n = maxLen + 1;
        if (n > 130) n = 130;
        if (!ReadBlock(address, buf, static_cast<size_t>(n))) return false;
        for (int i = 0; i < n; i++) {
            unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c == 0) return i >= minLen;
            if (c < 0x20 || c > 0x7E) return false;
        }
        return false;
    }

    static bool LooksLikeClass(uintptr_t klass) {
        if (klass < 0x10000 || (klass & 7) != 0) return false;
        uintptr_t head[4] = { 0, 0, 0, 0 };   // image, gc_desc, name, namespaze
        if (!ReadBlock(klass, head, sizeof(head))) return false;
        if (head[0] < 0x10000 || (head[0] & 7) != 0) return false;
        if (!IsAsciiString(head[2], 1, 96)) return false;
        if (head[3] != 0 && !IsAsciiString(head[3], 0, 128)) return false;
        return true;
    }

    static void ClassName(uintptr_t klass, char* out, size_t outSize) {
        out[0] = 0;
        uintptr_t namePtr = 0;
        if (!Read(klass + 0x10, namePtr) || namePtr < 0x10000) return;
        char buf[96] = { 0 };
        if (!ReadBlock(namePtr, buf, sizeof(buf) - 1)) return;
        buf[sizeof(buf) - 1] = 0;
        strncpy_s(out, outSize, buf, _TRUNCATE);
    }
    // ============================================================
    //  view-projection matrix detection
    // ============================================================
    // Unity keeps Matrix4x4 column major in memory, some copies (constant buffer
    // staging) are transposed, so both layouts are probed. Row r / column c is at
    //   layout 0 (unity):      index c * 4 + r
    //   layout 1 (transposed): index r * 4 + c
    static inline void FetchRows(const float* m, int layout, float r0[4], float r1[4], float r3[4]) {
        if (layout == 0) {
            r0[0] = m[0];  r0[1] = m[4];  r0[2] = m[8];  r0[3] = m[12];
            r1[0] = m[1];  r1[1] = m[5];  r1[2] = m[9];  r1[3] = m[13];
            r3[0] = m[3];  r3[1] = m[7];  r3[2] = m[11]; r3[3] = m[15];
        } else {
            r0[0] = m[0];  r0[1] = m[1];  r0[2] = m[2];  r0[3] = m[3];
            r1[0] = m[4];  r1[1] = m[5];  r1[2] = m[6];  r1[3] = m[7];
            r3[0] = m[12]; r3[1] = m[13]; r3[2] = m[14]; r3[3] = m[15];
        }
    }

    static void Transpose(const float* src, float* dst) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                dst[c * 4 + r] = src[r * 4 + c];
    }

    // Returns true when the 16 floats form a perspective world -> clip matrix.
    static bool IsViewProjection(const float* m, int layout, float aspectHint,
                                 float* outFov, float* outAspect) {
        float r0[4], r1[4], r3[4];
        FetchRows(m, layout, r0, r1, r3);

        // cheap gate first: the W row direction is the camera forward vector, so
        // its length must be exactly 1
        if (!(r3[0] > -1.02f && r3[0] < 1.02f)) return false;
        if (!(r3[1] > -1.02f && r3[1] < 1.02f)) return false;
        if (!(r3[2] > -1.02f && r3[2] < 1.02f)) return false;
        float l3sq = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
        if (l3sq < 0.9801f || l3sq > 1.0201f) return false;

        float l0sq = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
        float l1sq = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
        if (l0sq < 0.16f || l0sq > 64.0f) return false;
        if (l1sq < 0.16f || l1sq > 64.0f) return false;

        float l0 = std::sqrt(l0sq), l1 = std::sqrt(l1sq), l3 = std::sqrt(l3sq);

        // the three rows are built from the orthonormal camera basis
        if (std::fabs(r0[0] * r3[0] + r0[1] * r3[1] + r0[2] * r3[2]) > 0.02f * l0 * l3) return false;
        if (std::fabs(r1[0] * r3[0] + r1[1] * r3[1] + r1[2] * r3[2]) > 0.02f * l1 * l3) return false;
        if (std::fabs(r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]) > 0.02f * l0 * l1) return false;

        if (!std::isfinite(r0[3]) || !std::isfinite(r1[3]) || !std::isfinite(r3[3])) return false;
        if (std::fabs(r0[3]) > 1.0e7f || std::fabs(r1[3]) > 1.0e7f || std::fabs(r3[3]) > 1.0e7f) return false;

        // Unity renders camera relative: unity_MatrixVP then carries no translation
        // at all and expects (worldPos - cameraPos) as input. Such a matrix is a
        // perfectly valid view projection but it cannot project world coordinates,
        // and every one of them collapses to an eye position of exactly (0,0,0) -
        // which used to win the cluster vote and break everything downstream.
        if (std::fabs(r0[3]) + std::fabs(r1[3]) + std::fabs(r3[3]) < 0.05f) return false;

        float aspect = l1 / l0;
        if (aspect < 0.5f || aspect > 4.0f) return false;
        if (aspectHint > 0.1f && std::fabs(aspect - aspectHint) > 0.22f * aspectHint) return false;

        float fov = 2.0f * std::atan(1.0f / l1) * 57.2957795f;
        if (fov < 25.0f || fov > 145.0f) return false;

        if (outFov)    *outFov = fov;
        if (outAspect) *outAspect = aspect;
        return true;
    }

    // camera position: the rows give three orthogonal planes through the eye
    static void CameraFromMatrix(const float* m, Vec3& pos, Vec3& fwd, float& fovDeg, float& aspect) {
        float r0[4], r1[4], r3[4];
        FetchRows(m, 0, r0, r1, r3);
        Vec3 a(r0[0], r0[1], r0[2]), b(r1[0], r1[1], r1[2]), c(r3[0], r3[1], r3[2]);
        float la = a.LengthSqr(), lb = b.LengthSqr(), lc = c.LengthSqr();
        if (la < 1e-8f || lb < 1e-8f || lc < 1e-8f) { pos = Vec3(); fwd = Vec3(0, 0, 1); return; }
        pos = a * (-r0[3] / la) + b * (-r1[3] / lb) + c * (-r3[3] / lc);
        fwd = Vec3Normalize(c);
        aspect = std::sqrt(lb / la);
        fovDeg = 2.0f * std::atan(1.0f / std::sqrt(lb)) * 57.2957795f;
    }
    bool WorldToScreen(const Vec3& world, const float m[16], float screenW, float screenH, Vec3& out) {
        float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8]  + m[12];
        float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9]  + m[13];
        float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];

        if (!(clipW > 0.05f)) return false;
        if (!std::isfinite(clipX) || !std::isfinite(clipY)) return false;

        float ndcX = clipX / clipW;
        float ndcY = clipY / clipW;
        out.x = (screenW * 0.5f) * (1.0f + ndcX);
        out.y = (screenH * 0.5f) * (1.0f - ndcY);
        out.z = clipW;
        return true;
    }

    // ============================================================
    //  memory regions
    // ============================================================
    struct Region {
        uintptr_t base;
        size_t    size;
        bool      isPrivate;
    };

    static void CollectRegions(std::vector<Region>& out, bool includeImages) {
        out.clear();
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t address = 0x10000;
        const uintptr_t limit = 0x7FFFFFFEFFFFull;

        while (address < limit) {
            if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= address) break;

            DWORD prot = mbi.Protect & 0xFF;
            bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                             prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE);
            bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

            if (mbi.State == MEM_COMMIT && readable && !guarded &&
                mbi.RegionSize >= 0x1000 && mbi.RegionSize <= kMaxRegionSize &&
                (includeImages || mbi.Type != MEM_IMAGE)) {
                Region r;
                r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                r.size = mbi.RegionSize;
                r.isPrivate = (mbi.Type == MEM_PRIVATE);
                out.push_back(r);
            }
            address = next;
        }
    }

    static bool IsWritableHeap(uintptr_t address) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xFF;
        return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
    }
    // ============================================================
    //  stage 1 - find the camera
    // ============================================================
    struct MatrixHit {
        uintptr_t addr;
        int   layout;
        float fov;
        float aspect;
        Vec3  cam;
    };

    static void ScanMatrixCandidates(float aspectHint, std::vector<MatrixHit>& hits) {
        hits.clear();
        std::vector<Region> regions;
        CollectRegions(regions, true);

        std::vector<uint8_t> buffer(kChunk + 128);
        size_t scannedBytes = 0;

        for (size_t ri = 0; ri < regions.size() && g_running.load() && hits.size() < 8192; ri++) {
            const Region& r = regions[ri];
            size_t offset = 0;
            while (offset < r.size) {
                size_t want = r.size - offset;
                if (want > kChunk) want = kChunk;
                if (!RawCopy(buffer.data(), reinterpret_cast<const void*>(r.base + offset), want)) {
                    offset += kChunk;
                    continue;
                }
                scannedBytes += want;

                if (want >= 64) {
                    for (size_t i = 0; i + 64 <= want; i += 4) {
                        const float* m = reinterpret_cast<const float*>(buffer.data() + i);
                        for (int layout = 0; layout < 2; layout++) {
                            float fov = 0.0f, aspect = 0.0f;
                            if (!IsViewProjection(m, layout, aspectHint, &fov, &aspect)) continue;

                            float unity[16];
                            if (layout == 0) memcpy(unity, m, sizeof(unity));
                            else Transpose(m, unity);

                            MatrixHit hit;
                            hit.addr = r.base + offset + i;
                            hit.layout = layout;
                            hit.fov = fov;
                            hit.aspect = aspect;
                            float f2 = 0.0f, a2 = 0.0f;
                            Vec3 fwd;
                            CameraFromMatrix(unity, hit.cam, fwd, f2, a2);
                            if (!std::isfinite(hit.cam.x) || std::fabs(hit.cam.x) > 1.0e6f) break;
                            if (hit.cam.LengthSqr() < 4.0f) break;    // camera relative copy or menu camera
                            hits.push_back(hit);
                            break;
                        }
                        if (hits.size() >= 8192) { offset = r.size; break; }
                    }
                }
                if (offset >= r.size) break;
                offset += (kChunk - 64);
            }
        }

        std::lock_guard<std::mutex> guard(g_lock);
        g_status.scannedRegions = static_cast<int>(regions.size());
        g_status.scannedMB = static_cast<double>(scannedBytes) / (1024.0 * 1024.0);
        g_status.matrixCandidates = static_cast<int>(hits.size());
    }
    // Several copies of the matrix exist (camera component, render pipeline,
    // constant buffer staging...). The real main camera is the one most copies
    // agree on, so cluster by resulting eye position and keep the biggest group.
    static std::vector<MatrixHit> g_cameras;   // candidates of the winning cluster
    static std::atomic<int> g_cameraIndex{ 0 };

    static void ApplyCameraCandidate(size_t index) {
        std::lock_guard<std::mutex> guard(g_lock);
        if (index >= g_cameras.size()) return;

        const MatrixHit& win = g_cameras[index];
        g_matrixAddr.store(win.addr);
        g_matrixLayout.store(win.layout);
        g_cameraIndex.store(static_cast<int>(index));

        g_status.matrixAddress = win.addr;
        g_status.fovDeg = win.fov;
        g_status.aspect = win.aspect;
        g_status.camPos = win.cam;
        g_status.cameraIndex = static_cast<int>(index);
        g_status.cameraCount = static_cast<int>(g_cameras.size());
        snprintf(g_status.message, sizeof(g_status.message),
                 "camera %d/%d locked @ 0x%llX (fov %.0f, aspect %.2f)",
                 static_cast<int>(index) + 1, static_cast<int>(g_cameras.size()),
                 static_cast<unsigned long long>(win.addr), win.fov, win.aspect);
    }

    static bool PickCameraMatrix(float aspectHint) {
        std::vector<MatrixHit> hits;
        ScanMatrixCandidates(aspectHint, hits);
        if (hits.empty()) return false;

        size_t bestIdx = 0;
        int bestScore = -1;
        for (size_t i = 0; i < hits.size(); i++) {
            int score = 0;
            for (size_t j = 0; j < hits.size(); j++) {
                if ((hits[i].cam - hits[j].cam).LengthSqr() < 0.25f) score++;
            }
            if (score > bestScore) { bestScore = score; bestIdx = i; }
        }

        Vec3 anchor = hits[bestIdx].cam;

        std::vector<MatrixHit> cluster;
        for (size_t i = 0; i < hits.size(); i++) {
            if ((hits[i].cam - anchor).LengthSqr() < 0.25f) cluster.push_back(hits[i]);
        }

        // keep only addresses that are still valid a moment later, preferring
        // the ones that actually change (live matrices, not stale scratch data)
        std::vector<float> before(cluster.size() * 16, 0.0f);
        for (size_t i = 0; i < cluster.size(); i++)
            ReadBlock(cluster[i].addr, &before[i * 16], sizeof(float) * 16);

        // A matrix-shaped value frequently lives in a short-lived render
        // scratch buffer.  Give the next render pass time to overwrite it so
        // we retain a persistent camera matrix instead of locking a buffer
        // which vanishes on the very next frame.
        Sleep(180);

        std::vector<MatrixHit> live, stale;
        for (size_t i = 0; i < cluster.size(); i++) {
            float now[16];
            if (!ReadBlock(cluster[i].addr, now, sizeof(now))) continue;
            float fov = 0.0f, aspect = 0.0f;
            if (!IsViewProjection(now, cluster[i].layout, aspectHint, &fov, &aspect)) continue;
            if (memcmp(now, &before[i * 16], sizeof(now)) != 0) live.push_back(cluster[i]);
            else stale.push_back(cluster[i]);
        }
        if (live.empty()) live = stale;
        if (live.empty()) return false;

        // an fps game also has a viewmodel camera at the same position; the main
        // camera is the one duplicated most often, wider fov breaks ties
        std::vector<MatrixHit> ordered;
        while (!live.empty()) {
            size_t pick = 0;
            int pickCount = -1;
            for (size_t i = 0; i < live.size(); i++) {
                int count = 0;
                for (size_t j = 0; j < live.size(); j++) {
                    if (std::fabs(live[i].fov - live[j].fov) < 0.5f) count++;
                }
                if (count > pickCount || (count == pickCount && live[i].fov > live[pick].fov)) {
                    pickCount = count;
                    pick = i;
                }
            }
            float pickedFov = live[pick].fov;
            for (size_t i = 0; i < live.size(); ) {
                if (std::fabs(live[i].fov - pickedFov) < 0.5f) {
                    ordered.push_back(live[i]);
                    live.erase(live.begin() + static_cast<long long>(i));
                } else {
                    i++;
                }
            }
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_cameras = ordered;
        }
        ApplyCameraCandidate(0);
        return true;
    }

    void NextCamera() {
        size_t count;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            count = g_cameras.size();
        }
        if (count < 2) return;
        int next = (g_cameraIndex.load() + 1) % static_cast<int>(count);
        ApplyCameraCandidate(static_cast<size_t>(next));
    }

    // reads the live matrix, always returns it in the unity layout
    bool GetViewMatrix(float out[16]) {
        uintptr_t addr = g_matrixAddr.load();
        if (!addr) return false;
        int layout = g_matrixLayout.load();

        float raw[16];
        if (!ReadBlock(addr, raw, sizeof(raw))) return false;
        if (!IsViewProjection(raw, layout, 0.0f, nullptr, nullptr)) return false;

        if (layout == 0) memcpy(out, raw, sizeof(raw));
        else Transpose(raw, out);
        return true;
    }

    bool GetCamera(Vec3& outPos, Vec3& outForward) {
        float m[16];
        if (!GetViewMatrix(m)) return false;
        float fov = 0.0f, aspect = 0.0f;
        CameraFromMatrix(m, outPos, outForward, fov, aspect);
        return true;
    }
    static inline float Axis(const Vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

    static int DetectUpAxis() {
        if (g_upAxisOverride >= 0 && g_upAxisOverride <= 2) return g_upAxisOverride;
        double acc[3] = { 0.0, 0.0, 0.0 };
        int samples = 0;
        for (int i = 0; i < 12 && g_running.load(); i++) {
            float m[16];
            if (GetViewMatrix(m)) {
                float u[3] = { m[1], m[5], m[9] };
                float len = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                if (len > 1.0e-4f) {
                    acc[0] += std::fabs(u[0] / len);
                    acc[1] += std::fabs(u[1] / len);
                    acc[2] += std::fabs(u[2] / len);
                    samples++;
                }
            }
            Sleep(50);
        }
        if (samples == 0) return 1;
        int best = 0;
        if (acc[1] > acc[best]) best = 1;
        if (acc[2] > acc[best]) best = 2;
        return best;
    }

    // ============================================================
    //  stage 2 - find entities
    // ============================================================
    struct PosCand {
        uintptr_t addr;
        Vec3      value;
    };

    static void ScanPositionCandidates(const Vec3& cam, int upAxis, float minDist, float maxDist,
                                      float verticalBand, std::vector<PosCand>& out) {
        out.clear();
        std::vector<Region> regions;
        CollectRegions(regions, false);          // objects live in private heaps

        const float radius = maxDist;
        const float minX = cam.x - radius, maxX = cam.x + radius;
        const float minY = cam.y - radius, maxY = cam.y + radius;
        const float minZ = cam.z - radius, maxZ = cam.z + radius;
        const float camUp = Axis(cam, upAxis);
        const float maxSqr = maxDist * maxDist;
        const float minSqr = minDist * minDist;

        std::vector<uint8_t> buffer(kChunk + 32);

        for (size_t ri = 0; ri < regions.size() && g_running.load(); ri++) {
            const Region& r = regions[ri];
            if (!r.isPrivate) continue;
            size_t offset = 0;
            while (offset < r.size) {
                size_t want = r.size - offset;
                if (want > kChunk) want = kChunk;
                if (!RawCopy(buffer.data(), reinterpret_cast<const void*>(r.base + offset), want)) {
                    offset += kChunk;
                    continue;
                }
                if (want >= 12) {
                    const uint8_t* raw = buffer.data();
                    for (size_t i = 0; i + 12 <= want; i += 4) {
                        const float* p = reinterpret_cast<const float*>(raw + i);
                        float x = p[0];
                        if (!(x > minX && x < maxX)) continue;
                        float y = p[1];
                        if (!(y > minY && y < maxY)) continue;
                        float z = p[2];
                        if (!(z > minZ && z < maxZ)) continue;

                        Vec3 v(x, y, z);
                        float dSqr = (v - cam).LengthSqr();
                        if (dSqr < minSqr || dSqr > maxSqr) continue;
                        if (std::fabs(Axis(v, upAxis) - camUp) > verticalBand) continue;
                        if (x == y && y == z) continue;

                        PosCand c;
                        c.addr = r.base + offset + i;
                        c.value = v;
                        out.push_back(c);
                        if (out.size() >= kMaxPosCand) return;
                    }
                }
                if (want < kChunk) break;
                offset += (kChunk - 16);
            }
        }
    }
    static bool PlausiblePosition(const Vec3& v);
    static void ReadValuesBlocked(const std::vector<PosCand>& cands, std::vector<Vec3>& out);
    static uint64_t ModelKey(uintptr_t klass, int offset);
    static bool IsRejected(uintptr_t klass, int offset);
    static void RejectModel(uintptr_t klass, int offset);
    static void SetMessage(const char* text);
    static void SetStage(Stage stage, const char* text);

    // Keeps only the values that move together with the eye - that is our own
    // player object. Velocities, local space offsets, bone and animation buffers
    // all fail this test, which is what made the first version draw boxes near
    // the screen center: it had locked onto a field that was not a world position.
    static void FilterCameraFollowers(const std::vector<PosCand>& cands,
                                      const Vec3& camBefore, const Vec3& camAfter,
                                      std::vector<PosCand>& out) {
        out.clear();
        Vec3 camDelta = camAfter - camBefore;

        std::vector<Vec3> now;
        ReadValuesBlocked(cands, now);

        for (size_t i = 0; i < cands.size(); i++) {
            if (!PlausiblePosition(now[i])) continue;
            if ((now[i] - camAfter).Length() > SelfRadius()) continue;

            Vec3 err = (now[i] - cands[i].value) - camDelta;
            if (err.Length() > 0.75f) continue;

            PosCand c;
            c.addr = cands[i].addr;
            c.value = now[i];
            out.push_back(c);
            if (out.size() >= kMaxMoving) break;
        }
    }

    static uint32_t PtrHash(uintptr_t v) {
        uint64_t x = static_cast<uint64_t>(v);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 29;
        return static_cast<uint32_t>(x);
    }

    // Same sweep, but the volume is a thin tube along the crosshair ray. Aiming at
    // a player and scanning that tube is the cheapest way to get hold of a real
    // player coordinate: the tube is smaller than the sphere around the eye, and
    // whatever moves inside it is almost certainly the guy you are looking at.
    static void ScanRayCandidates(const Vec3& cam, const Vec3& fwd, float tubeRadius,
                                  float minDist, float maxDist, std::vector<PosCand>& out) {
        out.clear();
        std::vector<Region> regions;
        CollectRegions(regions, false);

        // axis aligned bounds of the tube, so the hot loop can reject a value with
        // a single compare instead of running the whole projection
        float loX = cam.x + (fwd.x < 0.0f ? fwd.x * maxDist : 0.0f) - tubeRadius;
        float hiX = cam.x + (fwd.x > 0.0f ? fwd.x * maxDist : 0.0f) + tubeRadius;
        float loY = cam.y + (fwd.y < 0.0f ? fwd.y * maxDist : 0.0f) - tubeRadius;
        float hiY = cam.y + (fwd.y > 0.0f ? fwd.y * maxDist : 0.0f) + tubeRadius;
        float loZ = cam.z + (fwd.z < 0.0f ? fwd.z * maxDist : 0.0f) - tubeRadius;
        float hiZ = cam.z + (fwd.z > 0.0f ? fwd.z * maxDist : 0.0f) + tubeRadius;

        std::vector<uint8_t> buffer(kChunk + 32);

        for (size_t ri = 0; ri < regions.size() && g_running.load(); ri++) {
            const Region& r = regions[ri];
            if (!r.isPrivate) continue;
            size_t offset = 0;
            while (offset < r.size) {
                size_t want = r.size - offset;
                if (want > kChunk) want = kChunk;
                if (!RawCopy(buffer.data(), reinterpret_cast<const void*>(r.base + offset), want)) {
                    offset += kChunk;
                    continue;
                }
                if (want >= 12) {
                    const uint8_t* raw = buffer.data();
                    for (size_t i = 0; i + 12 <= want; i += 4) {
                        const float* p = reinterpret_cast<const float*>(raw + i);
                        float x = p[0];
                        if (!(x > loX && x < hiX)) continue;
                        float y = p[1];
                        if (!(y > loY && y < hiY)) continue;
                        float z = p[2];
                        if (!(z > loZ && z < hiZ)) continue;

                        float vx = x - cam.x, vy = y - cam.y, vz = z - cam.z;
                        float t = vx * fwd.x + vy * fwd.y + vz * fwd.z;
                        if (t < minDist || t > maxDist) continue;

                        float px = vx - fwd.x * t;
                        float py = vy - fwd.y * t;
                        float pz = vz - fwd.z * t;
                        if (px * px + py * py + pz * pz > tubeRadius * tubeRadius) continue;

                        PosCand c;
                        c.addr = r.base + offset + i;
                        c.value = Vec3(x, y, z);
                        out.push_back(c);
                        if (out.size() >= kMaxPosCand) return;
                    }
                }
                if (want < kChunk) break;
                offset += (kChunk - 16);
            }
        }
    }

    // Re-reads a sorted candidate list one page at a time. Doing an SEH guarded
    // read per address made a million candidates take the better part of a minute.
    static void ReadValuesBlocked(const std::vector<PosCand>& cands, std::vector<Vec3>& out) {
        const size_t kPage = 0x1000;
        static uint8_t page[kPage];

        out.assign(cands.size(), Vec3(1.0e9f, 1.0e9f, 1.0e9f));

        size_t i = 0;
        while (i < cands.size() && g_running.load()) {
            uintptr_t base = cands[i].addr & ~static_cast<uintptr_t>(kPage - 1);
            bool ok = RawCopy(page, reinterpret_cast<const void*>(base), kPage);

            size_t j = i;
            while (j < cands.size() && cands[j].addr >= base && cands[j].addr < base + kPage) {
                size_t off = static_cast<size_t>(cands[j].addr - base);
                if (ok && off + 12 <= kPage) {
                    memcpy(&out[j], page + off, sizeof(Vec3));
                } else {
                    Vec3 v;
                    if (Read(cands[j].addr, v)) out[j] = v;
                }
                j++;
            }
            i = j;
        }
    }

    // keeps whatever changed by a walk-like amount over the given delay
    static void FilterMovers(const std::vector<PosCand>& cands, int delayMs,
                             std::vector<PosCand>& out) {
        out.clear();
        for (int i = 0; i < delayMs / 50 && g_running.load(); i++) Sleep(50);

        std::vector<Vec3> now;
        ReadValuesBlocked(cands, now);

        for (size_t i = 0; i < cands.size(); i++) {
            if (!PlausiblePosition(now[i])) continue;
            float moved = (now[i] - cands[i].value).Length();
            if (moved < 0.02f || moved > 12.0f) continue;

            PosCand c;
            c.addr = cands[i].addr;
            c.value = now[i];
            out.push_back(c);
            if (out.size() >= kMaxMoving) break;
        }
    }

    struct OwnerCandidate {
        uintptr_t klass = 0;
        bool strict = false;
        std::vector<int> offsets;
    };

    // every (class pointer, field offset) pair that could own one of the values
    static void CollectOwners(const std::vector<PosCand>& selfCands,
                              std::unordered_map<uintptr_t, OwnerCandidate>& owners) {
        std::unordered_map<uintptr_t, int> ptrCache;
        std::vector<uint8_t> window(kFieldWindow + 16);

        for (size_t i = 0; i < selfCands.size() && g_running.load(); i++) {
            uintptr_t addr = selfCands[i].addr;
            if (addr <= static_cast<uintptr_t>(kFieldWindow)) continue;
            if (!ReadBlock(addr - kFieldWindow, window.data(), kFieldWindow)) continue;

            int align = static_cast<int>(addr & 7);
            for (int off = 0x10 + ((8 - align) & 7); off <= kFieldWindow - 8; off += 8) {
                uintptr_t klass = 0;
                memcpy(&klass, window.data() + (kFieldWindow - off), sizeof(klass));
                if (klass < 0x10000 || (klass & 7) != 0) continue;

                int quality;
                auto it = ptrCache.find(klass);
                if (it != ptrCache.end()) {
                    quality = it->second;
                } else {
                    uintptr_t probe = 0;
                    if (!Read(klass, probe)) quality = 0;
                    else quality = LooksLikeClass(klass) ? 2 : 1;
                    ptrCache[klass] = quality;
                }
                if (quality == 0) continue;

                OwnerCandidate& oc = owners[klass];
                oc.klass = klass;
                if (quality == 2) oc.strict = true;
                bool known = false;
                for (size_t k = 0; k < oc.offsets.size(); k++) {
                    if (oc.offsets[k] == off) { known = true; break; }
                }
                if (!known && oc.offsets.size() < 16) oc.offsets.push_back(off);
            }
            if (owners.size() > 1500) break;
        }
    }
    struct Group {
        uintptr_t klass = 0;
        int offset = 0;
        bool strict = false;
        std::vector<uintptr_t> bases;
    };

    // One sweep over the heaps that collects every instance of every candidate
    // class. A 64k byte filter keeps the hot loop away from the hash map.
    static void SweepInstances(const std::unordered_map<uintptr_t, OwnerCandidate>& owners,
                               std::vector<Group>& groups) {
        groups.clear();
        if (owners.empty()) return;

        std::vector<uint8_t> filter(65536, 0);
        for (std::unordered_map<uintptr_t, OwnerCandidate>::const_iterator it = owners.begin();
             it != owners.end(); ++it) {
            filter[PtrHash(it->first) & 0xFFFF] = 1;
        }

        std::unordered_map<uint64_t, size_t> index;
        std::vector<Region> regions;
        CollectRegions(regions, false);
        std::vector<uint8_t> buffer(kChunk + 16);

        for (size_t ri = 0; ri < regions.size() && g_running.load(); ri++) {
            const Region& r = regions[ri];
            if (!r.isPrivate) continue;
            size_t offset = 0;
            while (offset < r.size) {
                size_t want = r.size - offset;
                if (want > kChunk) want = kChunk;
                if (!RawCopy(buffer.data(), reinterpret_cast<const void*>(r.base + offset), want)) {
                    offset += kChunk;
                    continue;
                }
                for (size_t i = 0; i + 8 <= want; i += 8) {
                    uintptr_t value = 0;
                    memcpy(&value, buffer.data() + i, sizeof(value));
                    if (value < 0x10000 || (value & 7) != 0) continue;
                    if (!filter[PtrHash(value) & 0xFFFF]) continue;

                    std::unordered_map<uintptr_t, OwnerCandidate>::const_iterator it = owners.find(value);
                    if (it == owners.end()) continue;

                    uintptr_t base = r.base + offset + i;
                    if (!IsWritableHeap(base)) continue;

                    // one read for the whole field window instead of one per offset
                    uint8_t fields[kFieldWindow + 16];
                    if (!ReadBlock(base, fields, sizeof(fields))) continue;

                    // an il2cpp object header is [klass][monitor], so the second
                    // qword is either null or a real pointer - random data is not
                    uintptr_t second = 0;
                    memcpy(&second, fields + 8, sizeof(second));
                    if (second != 0 && (second < 0x10000 || (second & 7) != 0)) continue;

                    for (size_t k = 0; k < it->second.offsets.size(); k++) {
                        int off = it->second.offsets[k];
                        if (off < 0 || off + 12 > static_cast<int>(sizeof(fields))) continue;

                        Vec3 pos;
                        memcpy(&pos, fields + off, sizeof(pos));
                        if (!PlausiblePosition(pos)) continue;

                        uint64_t key = ((value >> 3) * 1000003ull) ^ (static_cast<uint64_t>(off) << 48);
                        std::unordered_map<uint64_t, size_t>::iterator gi = index.find(key);
                        size_t gidx;
                        if (gi == index.end()) {
                            Group g;
                            g.klass = value;
                            g.offset = off;
                            g.strict = it->second.strict;
                            groups.push_back(g);
                            gidx = groups.size() - 1;
                            index[key] = gidx;
                        } else {
                            gidx = gi->second;
                        }
                        if (groups[gidx].bases.size() < 128) groups[gidx].bases.push_back(base);
                    }
                }
                if (want < kChunk) break;
                offset += (kChunk - 8);
            }
        }
    }

    // The winning group must contain us (an instance sitting on the camera) and
    // then it is scored. Two signals separate real players from dropped weapons
    // and other pickups, which also live in classes with a few instances and one
    // copy in our hands:
    //   - other instances of a player class move, dropped items never do
    //   - our own player object sits roughly a body below the eye, a held weapon
    //     sits at eye level
    struct GroupEval {
        size_t gi = 0;
        std::vector<uintptr_t> bases;
        std::vector<Vec3> positions;
        int   selfIndex = -1;
        float selfBelow = 0.0f;
        float spread = 0.0f;
    };

    static bool PickBestGroup(const std::vector<Group>& groups, const Vec3& cam, int upAxis,
                              EntityModel& outModel, int& outVotes) {
        const float camUp = Axis(cam, upAxis);
        std::vector<GroupEval> evals;

        for (size_t gi = 0; gi < groups.size(); gi++) {
            const Group& g = groups[gi];
            if (g.bases.size() < 1 || g.bases.size() > 64) continue;
            if (IsRejected(g.klass, g.offset)) continue;

            GroupEval ev;
            ev.gi = gi;
            float bestSelf = -1.0f;
            bool anyNear = false;

            for (size_t i = 0; i < g.bases.size(); i++) {
                uintptr_t klass = 0;
                if (!Read(g.bases[i], klass) || klass != g.klass) continue;
                Vec3 p;
                if (!Read(g.bases[i] + g.offset, p) || !PlausiblePosition(p)) continue;

                float d = (p - cam).Length();
                if (d > 600.0f) continue;              // different coordinate space
                if (d < 400.0f) anyNear = true;
                if (d < SelfRadius() && (bestSelf < 0.0f || d < bestSelf)) {
                    bestSelf = d;
                    ev.selfIndex = static_cast<int>(ev.positions.size());
                }
                ev.bases.push_back(g.bases[i]);
                ev.positions.push_back(p);
            }

            if (ev.positions.empty() || !anyNear) continue;
            if (ev.positions.size() > 64) continue;

            int selfCount = 0;
            for (size_t i = 0; i < ev.positions.size(); i++) {
                if ((ev.positions[i] - cam).Length() < SelfRadius()) selfCount++;
                for (size_t j = i + 1; j < ev.positions.size(); j++) {
                    float d = (ev.positions[i] - ev.positions[j]).Length();
                    if (d > ev.spread) ev.spread = d;
                }
            }
            if (selfCount > 4) continue;

            if (ev.selfIndex >= 0)
                ev.selfBelow = camUp - Axis(ev.positions[static_cast<size_t>(ev.selfIndex)], upAxis);
            evals.push_back(ev);
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status.groupsPlausible = static_cast<int>(evals.size());
        }
        if (evals.empty()) return false;

        // Do one immediate comparison.  Movement remains a scoring signal, but
        // never holds the first ESP frame hostage for 1.4 seconds.
        SetStage(Stage::ScanEntities, "validating player candidates...");

        int bestScore = -1;
        long long bestEval = -1;
        int bestMovers = 0;

        for (size_t e = 0; e < evals.size(); e++) {
            GroupEval& ev = evals[e];
            const Group& g = groups[ev.gi];

            int movers = 0;
            for (size_t i = 0; i < ev.bases.size(); i++) {
                uintptr_t klass = 0;
                if (!Read(ev.bases[i], klass) || klass != g.klass) continue;
                Vec3 p;
                if (!Read(ev.bases[i] + g.offset, p) || !PlausiblePosition(p)) continue;
                float d = (p - ev.positions[i]).Length();
                if (d > 0.04f && d < 20.0f) movers++;
            }

            // a class nobody moves in and that does not even contain us is junk
            if (movers == 0 && ev.selfIndex < 0) continue;

            int n = static_cast<int>(ev.positions.size());
            int score = n * 2;
            if (movers > 0) score += 45;
            score += movers * 8;
            if (ev.selfIndex >= 0) score += 20;
            if (ev.selfBelow > 0.7f && ev.selfBelow < 2.6f) score += 25;
            if (ev.spread > 2.0f) score += 6;
            if (g.strict) score += 6;
            if (n >= 2 && n <= 40) score += 6;

            if (score > bestScore) {
                bestScore = score;
                bestEval = static_cast<long long>(e);
                bestMovers = movers;
            }
        }

        if (bestEval < 0) return false;

        const GroupEval& winEval = evals[static_cast<size_t>(bestEval)];
        const Group& win = groups[winEval.gi];
        outModel.klass = win.klass;
        outModel.posOffset = win.offset;
        outModel.strict = win.strict;
        outVotes = static_cast<int>(winEval.positions.size());

        std::lock_guard<std::mutex> guard(g_lock);
        g_status.pickedMovers = bestMovers;
        g_status.selfBelowEye = winEval.selfBelow;
        return true;
    }
    static bool PlausiblePosition(const Vec3& v) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
        if (std::fabs(v.x) > 1.0e5f || std::fabs(v.y) > 1.0e5f || std::fabs(v.z) > 1.0e5f) return false;
        if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) return false;
        return true;
    }

    // Sweeps the given regions for every live instance of the resolved class.
    // Instances of one class stay in a couple of heap regions, so remembering
    // where they were found lets the tracker refresh in a few milliseconds
    // instead of walking the whole address space.
    static std::vector<Region> g_instanceRegions;

    static void ScanRegionsForInstances(const std::vector<Region>& regions, const EntityModel& model,
                                        std::vector<uintptr_t>& out, std::vector<Region>* hitRegions) {
        out.clear();
        if (hitRegions) hitRegions->clear();
        if (!model.klass || model.posOffset < 0) return;

        std::vector<uint8_t> buffer(kChunk + 16);

        for (size_t ri = 0; ri < regions.size() && g_running.load() && out.size() < 96; ri++) {
            const Region& r = regions[ri];
            if (!r.isPrivate) continue;
            bool hit = false;
            size_t offset = 0;
            while (offset < r.size) {
                size_t want = r.size - offset;
                if (want > kChunk) want = kChunk;
                if (!RawCopy(buffer.data(), reinterpret_cast<const void*>(r.base + offset), want)) {
                    offset += kChunk;
                    continue;
                }
                if (out.size() >= 96) break;
                for (size_t i = 0; i + 8 <= want; i += 8) {
                    uintptr_t value = 0;
                    memcpy(&value, buffer.data() + i, sizeof(value));
                    if (value != model.klass) continue;

                    uintptr_t base = r.base + offset + i;
                    Vec3 pos;
                    if (!Read(base + model.posOffset, pos)) continue;
                    if (!PlausiblePosition(pos)) continue;
                    if (!IsWritableHeap(base)) continue;

                    uintptr_t second = 0;
                    if (!Read(base + 8, second)) continue;
                    if (second != 0 && (second < 0x10000 || (second & 7) != 0)) continue;

                    out.push_back(base);
                    hit = true;
                    if (out.size() >= 96) break;
                }
                if (want < kChunk) break;
                offset += (kChunk - 8);
            }
            if (hit && hitRegions) hitRegions->push_back(r);
        }
    }

    static void FindInstances(const EntityModel& model, std::vector<uintptr_t>& out) {
        std::vector<Region> regions;
        CollectRegions(regions, false);

        std::vector<Region> hitRegions;
        ScanRegionsForInstances(regions, model, out, &hitRegions);

        std::lock_guard<std::mutex> guard(g_lock);
        g_instanceRegions = hitRegions;
    }

    // cheap tracker refresh, safe to call several times per second
    static void FastRefreshInstances() {
        EntityModel model;
        std::vector<Region> regions;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            model = g_model;
            regions = g_instanceRegions;
        }
        if (!model.klass || regions.empty()) return;

        std::vector<uintptr_t> objects;
        ScanRegionsForInstances(regions, model, objects, nullptr);
        if (objects.empty()) return;      // keep the previous list instead of blanking out

        std::lock_guard<std::mutex> guard(g_lock);
        g_objects = objects;
        g_status.trackedEntities = static_cast<int>(objects.size());
    }

    // Health / team are guessed the same way: a field that behaves like health
    // (0..100, at least one entity at full hp) or like a team id (few distinct
    // small values shared by the entities).
    static void ResolveFields(EntityModel& model, const std::vector<uintptr_t>& objects) {
        model.healthOffset = -1;
        model.teamOffset = -1;
        if (!g_autoFields || objects.size() < 2) return;

        std::vector<std::vector<uint8_t>> windows;
        for (size_t i = 0; i < objects.size() && windows.size() < 24; i++) {
            std::vector<uint8_t> w(kFieldWindow, 0);
            if (!ReadBlock(objects[i], w.data(), kFieldWindow)) continue;
            windows.push_back(w);
        }
        if (windows.size() < 2) return;

        int bestHealth = -1, bestHealthScore = 0;
        bool bestHealthFloat = true;
        int bestTeam = -1, bestTeamScore = 0;

        for (int off = 0x10; off <= kFieldWindow - 4; off += 4) {
            if (model.posOffset >= 0 && off > model.posOffset - 4 && off < model.posOffset + 12) continue;

            bool okFloat = true, okInt = true, okTeam = true;
            bool hasFull = false;
            float fVals[24];
            int   iVals[24];
            size_t n = windows.size();

            for (size_t k = 0; k < n; k++) {
                float f; int iv;
                memcpy(&f, windows[k].data() + off, sizeof(f));
                memcpy(&iv, windows[k].data() + off, sizeof(iv));
                fVals[k] = f; iVals[k] = iv;

                if (!(std::isfinite(f) && f > 0.5f && f <= 100.5f)) okFloat = false;
                if (!(iv >= 1 && iv <= 100)) okInt = false;
                if (!(iv >= 0 && iv <= 8)) okTeam = false;
                if ((okFloat && f > 99.0f && f < 101.0f) || (okInt && iv == 100)) hasFull = true;
            }

            int distinctF = 0, distinctI = 0;
            for (size_t k = 0; k < n; k++) {
                bool dupF = false, dupI = false;
                for (size_t j = 0; j < k; j++) {
                    if (fVals[j] == fVals[k]) dupF = true;
                    if (iVals[j] == iVals[k]) dupI = true;
                }
                if (!dupF) distinctF++;
                if (!dupI) distinctI++;
            }

            if (okFloat && distinctF >= 2) {
                int score = distinctF + (hasFull ? 6 : 0) + 1;
                if (score > bestHealthScore) { bestHealthScore = score; bestHealth = off; bestHealthFloat = true; }
            }
            if (okInt && distinctI >= 2) {
                int score = distinctI + (hasFull ? 6 : 0);
                if (score > bestHealthScore) { bestHealthScore = score; bestHealth = off; bestHealthFloat = false; }
            }
            if (okTeam && distinctI == 2) {
                int score = 10 - (off / 0x100);
                if (score > bestTeamScore) { bestTeamScore = score; bestTeam = off; }
            }
        }

        model.healthOffset = bestHealth;
        model.healthIsFloat = bestHealthFloat;
        model.teamOffset = bestTeam;
    }
    // ============================================================
    //  worker thread
    // ============================================================
    static void SetMessage(const char* text) {
        std::lock_guard<std::mutex> guard(g_lock);
        strncpy_s(g_status.message, sizeof(g_status.message), text, _TRUNCATE);
    }

    static void SetStage(Stage stage, const char* text) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_status.stage = stage;
        if (text) strncpy_s(g_status.message, sizeof(g_status.message), text, _TRUNCATE);
    }

    static void ClearEntities() {
        std::lock_guard<std::mutex> guard(g_lock);
        g_model = EntityModel();
        g_objects.clear();
        g_movedSet.clear();
        g_rawPoints.clear();
        g_status.entityClass = 0;
        g_status.posOffset = -1;
        g_status.healthOffset = -1;
        g_status.teamOffset = -1;
        g_status.trackedEntities = 0;
        g_status.rawPoints = 0;
        g_status.classVotes = 0;
        g_status.selfAnchored = false;
        g_status.modelAlive = false;
        g_status.className[0] = 0;
    }

    // The model is considered alive while something in it still behaves like a
    // player: either one of the objects sits on the camera (that is us) or at
    // least one of them moved recently. In the main menu neither is true, so the
    // model gets dropped instead of painting boxes over the lobby.
    static bool CheckModelAlive() {
        EntityModel model;
        std::vector<uintptr_t> objects;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            model = g_model;
            objects = g_objects;
        }
        if (!model.klass || model.posOffset < 0 || objects.empty()) return false;

        Vec3 cam, fwd;
        if (!GetCamera(cam, fwd)) return false;

        static std::vector<uintptr_t> lastBases;
        static std::vector<Vec3>      lastPos;

        std::vector<uintptr_t> bases;
        std::vector<Vec3> positions;
        std::vector<uintptr_t> justMoved;
        float nearest = -1.0f;
        int movers = 0;

        for (size_t i = 0; i < objects.size(); i++) {
            uintptr_t klass = 0;
            if (!Read(objects[i], klass) || klass != model.klass) continue;
            Vec3 p;
            if (!Read(objects[i] + model.posOffset, p) || !PlausiblePosition(p)) continue;

            float d = (p - cam).Length();
            if (nearest < 0.0f || d < nearest) nearest = d;

            for (size_t k = 0; k < lastBases.size(); k++) {
                if (lastBases[k] != objects[i]) continue;
                float moved = (p - lastPos[k]).Length();
                if (moved > 0.04f && moved < 25.0f) {
                    movers++;
                    justMoved.push_back(objects[i]);
                }
                break;
            }

            bases.push_back(objects[i]);
            positions.push_back(p);
        }

        lastBases = bases;
        lastPos = positions;

        bool anchored = (nearest >= 0.0f && nearest < SelfRadius() * 1.8f);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status.selfDistance = nearest;
            g_status.selfAnchored = anchored;
            g_status.pickedMovers = movers;
            for (size_t i = 0; i < justMoved.size(); i++) g_movedSet.insert(justMoved[i]);
            g_status.movingEntities = static_cast<int>(g_movedSet.size());
        }
        return !bases.empty() && (movers > 0 || anchored);
    }

    // ============================================================
    //  fast path: the offsets that were dumped from this build
    // ============================================================
    // The old dump left three static RVAs in client.dll (entity list v1 / v2 and
    // local player). Instead of trusting them blindly they get verified against
    // the live camera: a pointer only counts when the object behind it really
    // holds a world position next to the eye, or when it is a list whose entries
    // share a class and all hold plausible coordinates. A window of +-0x200 around
    // each RVA is probed so a small shift after a game patch is tolerated.
    static bool FindCommonPositionOffset(const std::vector<uintptr_t>& objs, const Vec3& cam,
                                        int& outOffset) {
        if (objs.empty()) return false;

        std::vector<std::vector<uint8_t>> windows;
        for (size_t i = 0; i < objs.size() && windows.size() < 16; i++) {
            std::vector<uint8_t> w(kFieldWindow, 0);
            if (!ReadBlock(objs[i], w.data(), kFieldWindow)) continue;
            windows.push_back(w);
        }
        if (windows.empty()) return false;

        int bestOff = -1;
        int bestScore = -1;

        for (int off = 0x10; off + 12 <= kFieldWindow; off += 4) {
            int valid = 0, nearCount = 0;
            float spread = 0.0f;
            std::vector<Vec3> vals;

            for (size_t k = 0; k < windows.size(); k++) {
                Vec3 p;
                memcpy(&p, windows[k].data() + off, sizeof(p));
                if (!PlausiblePosition(p)) { valid = -1; break; }
                float d = (p - cam).Length();
                if (d > 600.0f) { valid = -1; break; }
                if (d < SelfRadius() * 2.5f) nearCount++;
                vals.push_back(p);
                valid++;
            }
            if (valid <= 0) continue;

            for (size_t a = 0; a < vals.size(); a++)
                for (size_t b = a + 1; b < vals.size(); b++) {
                    float d = (vals[a] - vals[b]).Length();
                    if (d > spread) spread = d;
                }

            int score = valid * 3 + (nearCount > 0 ? 20 : 0) + (spread > 1.0f ? 10 : 0);
            if (score > bestScore) { bestScore = score; bestOff = off; }
        }

        if (bestOff < 0) return false;
        outOffset = bestOff;
        return true;
    }

    static void CollectListObjects(uintptr_t container, std::vector<uintptr_t>& out) {
        out.clear();

        uintptr_t arrays[2] = { container, 0 };
        Read(container + 0x10, arrays[1]);          // List<T> backing array

        for (int a = 0; a < 2; a++) {
            uintptr_t arr = arrays[a];
            if (arr < 0x10000 || (arr & 7) != 0) continue;

            int32_t count = 0;
            if (!Read(arr + 0x18, count)) continue;
            if (count < 1 || count > 64) continue;

            std::vector<uintptr_t> objs;
            for (int i = 0; i < count; i++) {
                uintptr_t obj = 0;
                if (!Read(arr + 0x20 + static_cast<uintptr_t>(i) * 8, obj)) break;
                if (obj < 0x10000 || (obj & 7) != 0) continue;
                uintptr_t klass = 0;
                if (!Read(obj, klass) || klass < 0x10000 || (klass & 7) != 0) continue;
                objs.push_back(obj);
            }
            if (objs.size() >= 2) { out = objs; return; }
        }
    }

    static bool TryStaticModel(EntityModel& outModel, std::vector<uintptr_t>& outObjects) {
        outObjects.clear();

        uintptr_t client = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        if (!client) return false;

        Vec3 cam, fwd;
        if (!GetCamera(cam, fwd)) return false;

        const uintptr_t seeds[] = { 0x71598A0, 0x7157868, 0x7156DA0 };

        for (int s = 0; s < 3; s++) {
            for (int slot = -0x40; slot <= 0x40; slot++) {
                uintptr_t staticAddr = client + seeds[s] + static_cast<intptr_t>(slot) * 8;
                uintptr_t ptr = 0;
                if (!Read(staticAddr, ptr)) continue;
                if (ptr < 0x10000 || (ptr & 7) != 0) continue;

                uintptr_t klass = 0;
                if (!Read(ptr, klass) || klass < 0x10000 || (klass & 7) != 0) continue;

                // a list of players gives us the class, the field and the objects
                std::vector<uintptr_t> objs;
                CollectListObjects(ptr, objs);
                if (objs.size() >= 2) {
                    uintptr_t firstKlass = 0;
                    if (!Read(objs[0], firstKlass)) continue;

                    std::vector<uintptr_t> same;
                    for (size_t i = 0; i < objs.size(); i++) {
                        uintptr_t k = 0;
                        if (Read(objs[i], k) && k == firstKlass) same.push_back(objs[i]);
                    }
                    int off = -1;
                    if (same.size() >= 2 && LooksLikeClass(firstKlass) &&
                        FindCommonPositionOffset(same, cam, off) &&
                        !IsRejected(firstKlass, off)) {
                        outModel.klass = firstKlass;
                        outModel.posOffset = off;
                        outModel.strict = true;
                        outObjects = same;
                        return true;
                    }
                }

                // or it is our own player object: it must carry a position near the eye
                std::vector<uintptr_t> single;
                single.push_back(ptr);
                int off = -1;
                if (LooksLikeClass(klass) && FindCommonPositionOffset(single, cam, off) &&
                    !IsRejected(klass, off)) {
                    Vec3 p;
                    if (Read(ptr + off, p) && (p - cam).Length() < SelfRadius() * 1.8f) {
                        outModel.klass = klass;
                        outModel.posOffset = off;
                        outModel.strict = true;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    static uint64_t ModelKey(uintptr_t klass, int offset) {
        return ((klass >> 3) * 1000003ull) ^ (static_cast<uint64_t>(offset) << 48);
    }

    static bool IsRejected(uintptr_t klass, int offset) {
        std::lock_guard<std::mutex> guard(g_lock);
        return g_rejected.find(ModelKey(klass, offset)) != g_rejected.end();
    }

    static void RejectModel(uintptr_t klass, int offset) {
        if (!klass || offset < 0) return;
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_rejected.size() < 4096) g_rejected.insert(ModelKey(klass, offset));
    }

    // A model is only accepted when it behaves like players: a handful of
    // instances, at least two of them in different places, and either one sitting
    // on the camera (that is us) or one that moves within a second.
    static bool ValidateModel(const EntityModel& model, std::vector<uintptr_t>& objects) {
        if (!model.klass || model.posOffset < 0x10) return false;
        if (IsRejected(model.klass, model.posOffset)) return false;

        if (objects.size() < 2 || objects.size() > 64) {
            std::vector<uintptr_t> found;
            FindInstances(model, found);
            objects = found;
        }
        if (objects.size() < 2 || objects.size() > 64) return false;

        Vec3 cam, fwd;
        if (!GetCamera(cam, fwd)) return false;

        std::vector<Vec3> before;
        float nearest = -1.0f;
        float spread = 0.0f;

        for (size_t i = 0; i < objects.size(); i++) {
            Vec3 p;
            uintptr_t klass = 0;
            if (!Read(objects[i], klass) || klass != model.klass) return false;
            if (!Read(objects[i] + model.posOffset, p) || !PlausiblePosition(p)) return false;
            float d = (p - cam).Length();
            if (d > 600.0f) return false;
            if (nearest < 0.0f || d < nearest) nearest = d;
            for (size_t j = 0; j < before.size(); j++) {
                float s = (before[j] - p).Length();
                if (s > spread) spread = s;
            }
            before.push_back(p);
        }
        if (spread < 0.5f) return false;

        bool anchored = (nearest >= 0.0f && nearest < SelfRadius() * 1.8f);

        // A local object anchored to the camera is already a strong enough
        // validation signal.  Waiting here made the known-offset fast path take
        // at least 1.2 seconds before it could draw anything.
        if (!anchored) Sleep(25);

        int movers = 0;
        for (size_t i = 0; i < objects.size(); i++) {
            Vec3 p;
            uintptr_t klass = 0;
            if (!Read(objects[i], klass) || klass != model.klass) continue;
            if (!Read(objects[i] + model.posOffset, p) || !PlausiblePosition(p)) continue;
            float moved = (p - before[i]).Length();
            if (moved > 0.04f && moved < 25.0f) movers++;
        }

        if (movers == 0 && !anchored) {
            RejectModel(model.klass, model.posOffset);
            return false;
        }
        return true;
    }

    static void StoreRawPoints(const std::vector<PosCand>& cands) {
        std::vector<RawPoint> points;
        for (size_t i = 0; i < cands.size() && points.size() < 400; i++) {
            RawPoint rp;
            rp.addr = cands[i].addr;
            rp.pos = cands[i].value;
            points.push_back(rp);
        }

        std::lock_guard<std::mutex> guard(g_lock);
        g_rawPoints = points;
        g_status.rawPoints = static_cast<int>(points.size());
        if (!points.empty()) {
            g_status.modelAlive = true;
            g_status.stage = Stage::Tracking;
        }
    }

    static void DiscoverEntities(bool rayMode) {
        ULONGLONG started = GetTickCount64();

        Vec3 camA, fwd;
        if (!GetCamera(camA, fwd)) {
            SetMessage("camera lost while scanning entities");
            return;
        }

        std::vector<PosCand> candidates;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status.rayMode = rayMode;
            g_status.followCandidates = 0;
            g_status.follow2Candidates = 0;
            g_status.ownerClasses = 0;
            g_status.groupCount = 0;
            g_status.groupsPlausible = 0;
        }

        // fast path first: the dumped static pointers, verified against the camera
        {
            SetStage(Stage::ScanEntities, "checking the known static offsets...");
            EntityModel staticModel;
            std::vector<uintptr_t> staticObjects;
            if (TryStaticModel(staticModel, staticObjects) &&
                ValidateModel(staticModel, staticObjects)) {
                ResolveFields(staticModel, staticObjects);

                char klassName[64] = { 0 };
                if (staticModel.strict) ClassName(staticModel.klass, klassName, sizeof(klassName));

                std::lock_guard<std::mutex> guard(g_lock);
                g_model = staticModel;
                g_objects = staticObjects;
                g_status.entityClass = staticModel.klass;
                g_status.posOffset = staticModel.posOffset;
                g_status.healthOffset = staticModel.healthOffset;
                g_status.teamOffset = staticModel.teamOffset;
                g_status.classVotes = static_cast<int>(staticObjects.size());
                g_status.strictClassMatch = staticModel.strict;
                g_status.selfAnchored = true;
                g_status.modelAlive = true;
                g_status.trackedEntities = static_cast<int>(staticObjects.size());
                g_status.lastScanSeconds = static_cast<double>(GetTickCount64() - started) / 1000.0;
                strncpy_s(g_status.className, sizeof(g_status.className), klassName, _TRUNCATE);
                snprintf(g_status.stageDetail, sizeof(g_status.stageDetail),
                         "static offsets verified, no scan needed");
                snprintf(g_status.message, sizeof(g_status.message),
                         "locked: class 0x%llX pos +0x%X, %d objects",
                         static_cast<unsigned long long>(staticModel.klass),
                         staticModel.posOffset, static_cast<int>(staticObjects.size()));
                g_status.stage = Stage::Tracking;
                return;
            }
        }

        if (rayMode) {
            // aim at a player: everything inside a thin tube along the view ray
            SetStage(Stage::ScanEntities, "scanning the crosshair ray - keep looking at the player");
            ScanRayCandidates(camA, fwd, 0.7f, 1.5f, 120.0f, candidates);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_status.posCandidates = static_cast<int>(candidates.size());
                g_status.candidateCapHit = (candidates.size() >= kMaxPosCand);
            }
            if (candidates.empty()) {
                SetMessage("nothing on the crosshair ray - aim at a player and try again");
                return;
            }

            SetStage(Stage::ScanEntities, "checking what moves on the ray...");
            std::vector<PosCand> movers;
            FilterMovers(candidates, 500, movers);
            if (!movers.empty()) candidates = movers;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_status.followCandidates = static_cast<int>(candidates.size());
            }
        } else {
            SetStage(Stage::ScanEntities, "scanning for your own player object...");
            ScanPositionCandidates(camA, g_upAxis.load(), 0.02f, SelfRadius(), SelfRadius(), candidates);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_status.posCandidates = static_cast<int>(candidates.size());
                g_status.candidateCapHit = (candidates.size() >= kMaxPosCand);
            }
            if (candidates.empty()) {
                SetMessage("no coordinates next to the eye - raise the anchor radius and rescan");
                return;
            }

            // free bonus: if the eye travelled while scanning, drop everything static
            Vec3 camB;
            float moved = 0.0f;
            if (GetCamera(camB, fwd)) {
                moved = (camB - camA).Length();
                if (moved > 0.75f) {
                    std::vector<PosCand> follow;
                    FilterCameraFollowers(candidates, camA, camB, follow);
                    if (!follow.empty()) candidates = follow;
                }
            }
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_status.followCandidates = static_cast<int>(candidates.size());
                g_status.walkDistance = moved;
            }

            // Feeding a million candidates into the owner resolver costs a full
            // second per ten thousand, so keep the ones whose height above the
            // ground looks like a pair of feet below the eye.
            const size_t kOwnerBudget = 40000;
            if (candidates.size() > kOwnerBudget) {
                const int up = g_upAxis.load();
                const float camUp = Axis(camA, up);
                std::vector<PosCand> ranked;
                for (float tolerance = 0.25f; tolerance <= 3.0f && ranked.size() < kOwnerBudget;
                     tolerance *= 2.0f) {
                    ranked.clear();
                    for (size_t i = 0; i < candidates.size(); i++) {
                        float below = camUp - Axis(candidates[i].value, up);
                        if (std::fabs(below - 1.5f) > tolerance) continue;
                        ranked.push_back(candidates[i]);
                        if (ranked.size() >= kOwnerBudget) break;
                    }
                }
                if (!ranked.empty()) candidates = ranked;
                else candidates.resize(kOwnerBudget);
            }
        }

        // which classes own those values
        SetStage(Stage::ScanEntities, "resolving the objects that own those coordinates...");
        // Keep a compact rendering fallback before the owner discovery releases
        // the candidate buffer.  A bad class heuristic must not turn a valid
        // on-screen coordinate into an empty ESP.
        const std::vector<PosCand> rawFallback = candidates;
        std::unordered_map<uintptr_t, OwnerCandidate> owners;
        CollectOwners(candidates, owners);
        candidates.clear();
        candidates.shrink_to_fit();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status.ownerClasses = static_cast<int>(owners.size());
        }
        if (owners.empty()) {
            StoreRawPoints(rawFallback);
            SetMessage("drawing verified coordinates (player class unresolved)");
            return;
        }

        // every instance of those classes
        SetStage(Stage::ScanEntities, "collecting every instance of those classes...");
        std::vector<Group> groups;
        SweepInstances(owners, groups);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status.groupCount = static_cast<int>(groups.size());
        }

        Vec3 camNow;
        if (!GetCamera(camNow, fwd)) camNow = camA;

        EntityModel model;
        int votes = 0;
        if (!PickBestGroup(groups, camNow, g_upAxis.load(), model, votes)) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "drawing verified coordinates (%d classes, %d groups unresolved)",
                     static_cast<int>(owners.size()), static_cast<int>(groups.size()));
            StoreRawPoints(rawFallback);
            SetMessage(buf);
            return;
        }

        std::vector<uintptr_t> objects;
        FindInstances(model, objects);
        if (!ValidateModel(model, objects)) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "class 0x%llX +0x%X rejected; drawing verified coordinates",
                     static_cast<unsigned long long>(model.klass), model.posOffset);
            RejectModel(model.klass, model.posOffset);
            StoreRawPoints(rawFallback);
            SetMessage(buf);
            return;
        }
        ResolveFields(model, objects);

        char klassName[64] = { 0 };
        if (model.strict) ClassName(model.klass, klassName, sizeof(klassName));

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_model = model;
            g_objects = objects;
            g_status.entityClass = model.klass;
            g_status.posOffset = model.posOffset;
            g_status.healthOffset = model.healthOffset;
            g_status.teamOffset = model.teamOffset;
            g_status.classVotes = votes;
            g_status.strictClassMatch = model.strict;
            g_status.selfAnchored = true;
            g_status.modelAlive = true;
            g_status.trackedEntities = static_cast<int>(objects.size());
            g_status.lastScanSeconds = static_cast<double>(GetTickCount64() - started) / 1000.0;
            strncpy_s(g_status.className, sizeof(g_status.className), klassName, _TRUNCATE);
            snprintf(g_status.stageDetail, sizeof(g_status.stageDetail),
                     "%s scan, %d moved, you %.2f m below the eye",
                     rayMode ? "ray" : "eye", g_status.pickedMovers, g_status.selfBelowEye);
            snprintf(g_status.message, sizeof(g_status.message),
                     "locked: class 0x%llX pos +0x%X, %d objects",
                     static_cast<unsigned long long>(model.klass), model.posOffset,
                     static_cast<int>(objects.size()));
            g_status.stage = Stage::Tracking;
        }
    }
    static void RefreshInstances() {
        EntityModel model;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            model = g_model;
        }
        if (!model.klass || model.posOffset < 0) return;

        std::vector<uintptr_t> objects;
        FindInstances(model, objects);
        if (g_autoFields && (model.healthOffset < 0 || model.teamOffset < 0) && objects.size() >= 2)
            ResolveFields(model, objects);

        std::lock_guard<std::mutex> guard(g_lock);
        g_objects = objects;
        g_model = model;
        g_status.healthOffset = model.healthOffset;
        g_status.teamOffset = model.teamOffset;
        g_status.trackedEntities = static_cast<int>(objects.size());
    }

    static DWORD WINAPI Worker(LPVOID) {
        ULONGLONG lastRefresh = 0;
        ULONGLONG lastAnchor = 0;
        ULONGLONG lastMatrixLog = 0;
        ULONGLONG lastQuietLog = 0;
        int anchorFails = 0;
        int matrixFailovers = 0;
        bool initialDiscoveryPending = true;

        while (g_running.load()) {
            if (g_wantFullRecal.exchange(false)) {
                g_matrixAddr.store(0);
                ClearEntities();
                SetStage(Stage::Idle, "recalibrating...");
                initialDiscoveryPending = true;
            }

            if (g_matrixAddr.load() == 0) {
                SetStage(Stage::ScanCamera, "searching for the camera matrix...");
                float w = g_screenW.load(), h = g_screenH.load();
                float aspectHint = (h > 1.0f) ? (w / h) : 0.0f;

                if (PickCameraMatrix(aspectHint)) {
                    int up = DetectUpAxis();
                    g_upAxis.store(up);
                    {
                        std::lock_guard<std::mutex> guard(g_lock);
                        g_status.upAxis = up;
                        g_status.stage = Stage::CameraLocked;
                    }
                    DebugConsole::Log(LogLevel::Success, "[AutoESP] camera matrix locked");
                } else {
                    SetStage(Stage::ScanCamera, "camera matrix not found - load into a match, then retry");
                    // A matrix scan walks a large part of the address space;
                    // back off while the game is still loading instead of
                    // starting another expensive pass every frame.
                    Sleep(500);
                }
                continue;
            }

            float probe[16];
            if (!GetViewMatrix(probe)) {
                int cameraCount = 0;
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    cameraCount = static_cast<int>(g_cameras.size());
                }
                // The scan already found several camera candidates.  Rotate
                // through them first; it is instant and avoids another full
                // memory walk when the first candidate was a transient copy.
                if (++matrixFailovers < cameraCount) {
                    NextCamera();
                    Sleep(8);
                    continue;
                }
                matrixFailovers = 0;
                ULONGLONG now = GetTickCount64();
                if (now - lastMatrixLog >= 3000) {
                    DebugConsole::Log(LogLevel::Warning, "[AutoESP] no stable camera candidate; rescanning");
                    lastMatrixLog = now;
                }
                g_matrixAddr.store(0);
                ClearEntities();
                continue;
            }
            matrixFailovers = 0;

            {
                Vec3 pos, fwd;
                float fov = 0.0f, aspect = 0.0f;
                CameraFromMatrix(probe, pos, fwd, fov, aspect);
                std::lock_guard<std::mutex> guard(g_lock);
                g_status.camPos = pos;
                g_status.camForward = fwd;
                g_status.fovDeg = fov;
                g_status.aspect = aspect;
            }

            bool needEntities = g_wantEntityRescan.exchange(false);
            bool rayScan = g_wantTargetScan.exchange(false);
            if (rayScan) needEntities = true;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                // Run automatic discovery once after the camera appears.  A
                // failed heuristic used to continuously rescan the entire
                // process, so the ESP spent most of its life scanning instead
                // of drawing.  Further scans are explicitly requested by F2 or
                // the menu, and a full recalibration arms this once again.
                if ((g_model.klass == 0 || g_model.posOffset < 0) && initialDiscoveryPending) {
                    needEntities = true;
                    // The only useful no-offset fallback is a narrow sample
                    // through the crosshair.  It produces drawable target
                    // coordinates immediately instead of filling the cache
                    // with positions around the local player.
                    rayScan = true;
                }
            }

            if (needEntities) {
                ClearEntities();
                DiscoverEntities(rayScan);
                initialDiscoveryPending = false;
                lastRefresh = GetTickCount64();
                lastAnchor = lastRefresh;
                anchorFails = 0;

                bool locked;
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    locked = (g_model.klass != 0);
                }

                if (!locked) {
                    SetStage(Stage::CameraLocked,
                             "fast discovery found no player model - aim at a player and press F2");
                }

                // Yield once, then continue straight into the tracking path.
                // There is no reason to idle for 1-2.5 seconds after a model
                // has already been resolved.
                Sleep(1);
                continue;
            }

            if (GetTickCount64() - lastAnchor > 900) {
                bool alive = CheckModelAlive();
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    g_status.modelAlive = alive;
                }
                lastAnchor = GetTickCount64();

                if (!alive) {
                    anchorFails++;
                    if (anchorFails == 1) {
                        RefreshInstances();
                        lastRefresh = GetTickCount64();
                    }
                    if (anchorFails >= 6) {
                        ULONGLONG now = GetTickCount64();
                        if (now - lastQuietLog >= 5000) {
                            DebugConsole::Log(LogLevel::Warning,
                                              "[AutoESP] tracked objects went quiet; dropping the model");
                            lastQuietLog = now;
                        }
                        {
                            std::lock_guard<std::mutex> guard(g_lock);
                            if (g_model.klass) {
                                if (g_rejected.size() < 4096)
                                    g_rejected.insert(ModelKey(g_model.klass, g_model.posOffset));
                            }
                        }
                        ClearEntities();
                        anchorFails = 0;
                        SetStage(Stage::CameraLocked, "objects went quiet - waiting to recalibrate");
                        continue;
                    }
                } else {
                    anchorFails = 0;
                }
            }

            if (GetTickCount64() - lastRefresh > 8000) {
                RefreshInstances();
                lastRefresh = GetTickCount64();
            } else {
                FastRefreshInstances();
            }

            Sleep(8);
        }
        return 0;
    }
    // ============================================================
    //  public interface
    // ============================================================
    void SetScreenSize(float width, float height) {
        if (width > 16.0f)  g_screenW.store(width);
        if (height > 16.0f) g_screenH.store(height);
    }

    void Start() {
        if (g_running.load()) return;
        g_running.store(true);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_status = Status();
            g_status.stage = Stage::ScanCamera;
            strncpy_s(g_status.message, sizeof(g_status.message), "starting...", _TRUNCATE);
        }
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        DebugConsole::Log(LogLevel::Info, "[AutoESP] calibration worker started");
    }

    void Stop() {
        if (!g_running.load()) return;
        g_running.store(false);
        if (g_thread) {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }

    void Recalibrate()    { g_wantFullRecal.store(true); }
    void RescanEntities() { g_wantEntityRescan.store(true); }

    void CalibrateOnTarget() {
        g_wantTargetScan.store(true);
        g_wantEntityRescan.store(true);
        SetMessage("crosshair scan queued - keep looking at the player");
    }

    Status GetStatus() {
        std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
        static Status cached;
        if (lock.owns_lock()) cached = g_status;
        return cached;
    }

    void GetPlayers(std::vector<PlayerData>& out) {
        static EntityModel cachedModel;
        static std::vector<uintptr_t> cachedObjects;
        static std::unordered_set<uintptr_t> cachedMoved;
        static std::vector<RawPoint> cachedRawPoints;
        static char cachedName[64] = { 0 };

        out.clear();
        {
            std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
            if (lock.owns_lock()) {
                cachedModel = g_model;
                cachedObjects = g_objects;
                cachedMoved = g_movedSet;
                cachedRawPoints = g_rawPoints;
                memcpy(cachedName, g_status.className, sizeof(cachedName));
            }
        }
        const bool hasModel = cachedModel.klass != 0 && cachedModel.posOffset >= 0 && !cachedObjects.empty();
        if (!hasModel && cachedRawPoints.empty()) return;

        float m[16];
        if (!GetViewMatrix(m)) return;

        Vec3 cam, fwd;
        float fov = 0.0f, aspect = 0.0f;
        CameraFromMatrix(m, cam, fwd, fov, aspect);

        int up = g_upAxis.load();
        Vec3 upVec((up == 0) ? 1.0f : 0.0f, (up == 1) ? 1.0f : 0.0f, (up == 2) ? 1.0f : 0.0f);

        for (size_t i = 0; i < cachedObjects.size(); i++) {
            uintptr_t base = cachedObjects[i];
            uintptr_t klass = 0;
            if (!Read(base, klass) || klass != cachedModel.klass) continue;

            Vec3 pos;
            if (!Read(base + cachedModel.posOffset, pos)) continue;
            if (!PlausiblePosition(pos)) continue;

            float distance = (pos - cam).Length();

            // props and other junk never move; a player that stopped walking keeps
            // its flag, so this only ever removes scenery
            if (g_onlyMoving && distance > SelfRadius() &&
                cachedMoved.find(base) == cachedMoved.end()) {
                continue;
            }

            PlayerData p;
            p.address = base;
            p.distance = distance;
            Vec3 feet = pos + upVec * g_entityFootOffset;
            p.position = feet;
            p.headPosition = feet + upVec * g_entityHeight;

            if (cachedModel.healthOffset >= 0) {
                if (cachedModel.healthIsFloat) {
                    float hp = 0.0f;
                    if (Read(base + cachedModel.healthOffset, hp) && std::isfinite(hp))
                        p.health = hp;
                } else {
                    int hp = 0;
                    if (Read(base + cachedModel.healthOffset, hp))
                        p.health = static_cast<float>(hp);
                }
                if (p.health > 0.0f && p.health <= 100.5f) p.isAlive = true;
            }
            if (cachedModel.teamOffset >= 0) {
                int team = -1;
                if (Read(base + cachedModel.teamOffset, team) && team >= 0 && team <= 8)
                    p.team = team;
            }

            snprintf(p.name, sizeof(p.name), "%s", cachedName[0] ? cachedName : "player");
            out.push_back(p);
        }

        // Last-resort draw path. These are live Vec3 values that passed the
        // camera/ray filter, so dots remain useful even when Unity's object
        // header layout changes and the class cannot be reconstructed.
        if (!hasModel) {
            for (size_t i = 0; i < cachedRawPoints.size(); i++) {
                Vec3 pos;
                if (!Read(cachedRawPoints[i].addr, pos) || !PlausiblePosition(pos)) continue;

                PlayerData p;
                p.address = cachedRawPoints[i].addr;
                p.distance = (pos - cam).Length();
                p.position = pos + upVec * g_entityFootOffset;
                p.headPosition = p.position + upVec * g_entityHeight;
                p.isEnemy = true;
                snprintf(p.name, sizeof(p.name), "target");
                out.push_back(p);
            }
        }

        // the entity sitting on the camera is us
        int localIdx = -1;
        float localDist = 3.0f;
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i].distance < localDist) { localDist = out[i].distance; localIdx = static_cast<int>(i); }
        }
        int localTeam = -1;
        if (localIdx >= 0) {
            out[static_cast<size_t>(localIdx)].isLocalPlayer = true;
            out[static_cast<size_t>(localIdx)].isEnemy = false;
            localTeam = out[static_cast<size_t>(localIdx)].team;
        }
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i].isLocalPlayer) continue;
            if (localTeam >= 0 && out[i].team >= 0) out[i].isEnemy = (out[i].team != localTeam);
            else out[i].isEnemy = true;
        }
    }

} // namespace AutoESP
