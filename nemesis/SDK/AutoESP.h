#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include "Types.h"

// ============================================================
//  AutoESP - offset-free, self calibrating world reader.
//
//  The game is IL2CPP + protected, its exports are stripped and
//  every hardcoded struct offset we had was garbage. So instead of
//  guessing offsets we derive everything from the process itself:
//
//   1. Camera:   scan memory for a world->clip matrix. A perspective
//                view-projection matrix has very strong invariants
//                (unit length W row, mutually orthogonal rows, aspect
//                ratio equal to the real back buffer aspect), so a
//                false positive is practically impossible.
//                From that matrix we also recover camera position,
//                forward vector, fov and the world up axis.
//
//   2. Entities: with the camera known we look for float3 values that
//                sit close to the camera, move between two snapshots
//                and share a common owner. Owners are voted on:
//                (class pointer, field offset) pairs with the most
//                instances win -> that is the player class and the
//                position field. Health / team fields are guessed the
//                same way afterwards.
//
//  Everything runs on a worker thread, the render thread only reads
//  a snapshot, so a bad scan can never stall the game.
// ============================================================

namespace AutoESP {

    enum class Stage : int {
        Idle = 0,
        ScanCamera,
        CameraLocked,
        ScanEntities,
        Tracking,
        Failed
    };

    struct Status {
        Stage stage = Stage::Idle;

        // camera stage
        uintptr_t matrixAddress = 0;
        int matrixCandidates = 0;
        int cameraIndex = 0;
        int cameraCount = 0;
        float fovDeg = 0.0f;
        float aspect = 0.0f;
        Vec3 camPos;
        Vec3 camForward;
        int upAxis = 1;           // 0 = X, 1 = Y, 2 = Z

        // entity stage
        uintptr_t entityClass = 0;
        int posOffset = -1;
        int healthOffset = -1;
        int teamOffset = -1;
        int posCandidates = 0;      // float3 values sitting next to the eye
        int followCandidates = 0;   // ... that moved with the eye on the first walk
        int follow2Candidates = 0;  // ... and on the second walk
        int ownerClasses = 0;       // classes that could own such a field
        int groupCount = 0;         // (class, offset) groups found in the heaps
        int groupsPlausible = 0;    // ... that contain us and a few more objects
        int pickedMovers = 0;       // objects of the winning class that moved
        float selfBelowEye = 0.0f;  // how far below the eye our own object sits
        int classVotes = 0;
        float walkDistance = 0.0f;  // how far the eye travelled during calibration
        float selfDistance = -1.0f; // nearest tracked object to the eye
        bool strictClassMatch = false;
        bool selfAnchored = false;
        bool modelAlive = false;
        bool rayMode = false;       // last discovery used the crosshair ray
        bool candidateCapHit = false;

        // runtime
        int trackedEntities = 0;
        int movingEntities = 0;     // objects that were ever seen moving
        int rawPoints = 0;          // fallback: raw coordinate addresses being tracked
        int scannedRegions = 0;
        double scannedMB = 0.0;
        double lastScanSeconds = 0.0;
        char className[64] = "";
        char stageDetail[96] = "";
        char message[192] = "not started";
    };

    // lifecycle
    void Start();
    void Stop();
    void Recalibrate();        // full reset: camera + entities
    void RescanEntities();     // keep camera, redo entity discovery
    void CalibrateOnTarget();  // look at a player, then call this - scans the crosshair ray
    void NextCamera();         // switch to another camera matrix candidate

    // called from the render thread every frame
    void SetScreenSize(float width, float height);

    // results
    bool GetViewMatrix(float out[16]);              // Unity layout (column major)
    bool GetCamera(Vec3& outPos, Vec3& outForward);
    void GetPlayers(std::vector<PlayerData>& out);
    Status GetStatus();

    // projection helper, uses the Unity matrix layout
    bool WorldToScreen(const Vec3& world, const float matrix[16],
                       float screenW, float screenH, Vec3& outScreen);

    // tunables shown in the menu
    extern float g_scanRadius;      // meters around the camera used while scanning
    extern float g_entityHeight;    // assumed player height for the box
    extern float g_entityFootOffset;// shifts the box anchor along the up axis
    extern int   g_upAxisOverride;  // -1 = auto
    extern bool  g_autoFields;      // try to resolve health / team fields
    extern bool  g_onlyMoving;      // only draw objects that were seen moving
}
