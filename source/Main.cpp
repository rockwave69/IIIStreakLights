#include "plugin.h"
#include "CCamera.h"
#include "CCoronas.h"
#include "CSprite.h"
#include "CTimer.h"
#include "CModelInfo.h"
#include "CVehicle.h"
#include "CAutomobile.h"
#include "CVehicleModelInfo.h"
#include "CVector.h"

#include <stdint.h>
#include "debugmenu_public.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

extern "C"
{
    DebugMenuAPI gDebugMenuAPI = {};
}

// Main_Re3StyleStreak_Professional.cpp
// GTA San Andreas 1.0 US / Plugin-SDK.
// Adds re3-inspired streaks while retaining the game's normal light system.
// Screen-space history replaces the STREAK_ON flag unavailable through SA's corona API.
// This plugin never dereferences vehicle headlight/taillight material pointers: it
// uses guarded model dummy positions and native corona/sprite APIs instead.

namespace
{
    constexpr float HEADLIGHT_CORE_FAR_CLIP = 45.0f;
    constexpr float REVERSE_LIGHT_CORE_SIZE_MULTIPLIER = 0.70f;
    constexpr float REVERSE_LIGHT_FAR_CLIP = 28.0f;
    constexpr float HEADLIGHT_NEAR_CLIP = 0.5f;
    constexpr float HEADLIGHT_FADE_SPEED = 50.0f;

    // Vehicle model dummy indices used for the lamp positions.
    constexpr int DUMMY_HEADLIGHTS = 0;
    constexpr int DUMMY_REAR_LIGHTS = 1;

    constexpr float DEFAULT_HEADLIGHT_X = 0.8f;
    constexpr float DEFAULT_HEADLIGHT_Y = 2.0f;
    constexpr float DEFAULT_HEADLIGHT_Z = 0.5f;
    constexpr float DEFAULT_REAR_LIGHT_Y = -2.0f;

    struct HeadlightColour
    {
        unsigned char red;
        unsigned char green;
        unsigned char blue;
    };

    // SA does not expose re3's STREAK_ON flag, so keep a short screen-space history.
    constexpr int HEADLIGHT_STREAK_HISTORY = 6;
    constexpr float HEADLIGHT_STREAK_HALF_HEIGHT = 7.0f;
    constexpr float REAR_STREAK_HALF_HEIGHT = 5.0f;
    constexpr float HEADLIGHT_STREAK_BASE_HALF_WIDTH = 220.0f;
    constexpr float HEADLIGHT_STREAK_MAX_HALF_WIDTH = 440.0f;
    constexpr float HEADLIGHT_STREAK_BASE_ALPHA = 74.0f;
    constexpr float REAR_STREAK_BASE_HALF_WIDTH = 125.0f;
    constexpr float REAR_STREAK_MAX_HALF_WIDTH = 205.0f;
    constexpr float REAR_STREAK_BASE_ALPHA = 68.0f;

    // Runtime-tunable values exposed through the debug menu.
    int32_t bCustomStreakEnabled = 1;

    float fFrontStreakLengthScale = 1.0f;
    float fFrontStreakHeightScale = 1.0f;
    float fFrontStreakBrightness = 1.0f;
    float fFrontCoreSizeScale = 1.0f;
    float fFrontCoreBrightness = 0.84f;

    float fRearStreakLengthScale = 1.0f;
    float fRearStreakHeightScale = 1.0f;
    float fRearStreakBrightness = 1.0f;
    float fRearRoundCoreSizeScale = 1.0f;
    float fRearRoundCoreBrightness = 1.0f;

    float fReverseStreakLengthScale = 1.0f;
    float fReverseStreakBrightness = 1.0f;
    float fReverseCoreSizeScale = 1.0f;
    float fReverseCoreBrightness = 1.0f;

    float fBikeReverseStreakSizeMult = 0.55f;
    float fBikeReverseCoreSizeMult = 0.60f;

    float fSideAngleExponent = 4.0f;
    float fFrontAngleFadeStart = -0.88f;
    float fFrontAngleFadeEnd = -0.25f;
    float fFrontCoreAngleFadeStart = -0.995f;
    float fFrontCoreAngleFadeEnd = -0.88f;
    float fFrontCoreDistanceFadeStart = 23.0f;
    float fFrontCoreDistanceFadeEnd = 32.0f;
    float fReverseCoreAngleFadeStart = 0.80f;
    float fReverseCoreAngleFadeEnd = 0.97f;

    float fRearTailDistanceFadeStart = 9.0f;
    float fRearTailDistanceFadeEnd = 23.0f;
    float fStreakDistanceFadeStart = 25.0f;
    float fStreakDistanceFadeEnd = 40.0f;
    float fRearRoundAngleEntryStart = -0.20f;
    float fRearRoundAngleEntryEnd = 0.12f;
    float fRearRoundAngleExitStart = 0.28f;
    float fRearRoundAngleExitEnd = 0.78f;

    bool bDebugMenuRegistered = false;

    // A fault in an add-on vehicle model must not take down the game. Model
    // indices that trigger a recoverable access violation are quarantined for
    // the rest of the session; if the bad pointer cannot be attributed to a
    // model, the custom layer is disabled globally.
    constexpr int MAX_QUARANTINED_MODEL_INDICES = 512;
    int aQuarantinedModelIndices[MAX_QUARANTINED_MODEL_INDICES] = {};
    int nQuarantinedModelIndices = 0;
    bool bRuntimeFaultDetected = false;

    bool IsFiniteVector(const CVector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    bool IsSaneLocalLightPosition(const CVector& value)
    {
        // Vehicle dummy offsets should be small, local-space coordinates.
        // Reject NaN/Inf and corrupt values instead of feeding them to the renderer.
        constexpr float MAX_LOCAL_LIGHT_OFFSET = 30.0f;
        return IsFiniteVector(value) &&
               std::fabs(value.x) <= MAX_LOCAL_LIGHT_OFFSET &&
               std::fabs(value.y) <= MAX_LOCAL_LIGHT_OFFSET &&
               std::fabs(value.z) <= MAX_LOCAL_LIGHT_OFFSET;
    }

    bool IsSaneWorldLightPosition(const CVector& value)
    {
        // Allow custom maps, but reject obviously corrupt world coordinates.
        constexpr float MAX_WORLD_COORDINATE = 100000.0f;
        return IsFiniteVector(value) &&
               std::fabs(value.x) <= MAX_WORLD_COORDINATE &&
               std::fabs(value.y) <= MAX_WORLD_COORDINATE &&
               std::fabs(value.z) <= MAX_WORLD_COORDINATE;
    }

    bool IsModelQuarantined(int modelIndex)
    {
        for (int i = 0; i < nQuarantinedModelIndices; ++i)
        {
            if (aQuarantinedModelIndices[i] == modelIndex)
                return true;
        }
        return false;
    }

    bool QuarantineModel(int modelIndex)
    {
        if (modelIndex < 0)
            return false;
        if (IsModelQuarantined(modelIndex))
            return true;
        if (nQuarantinedModelIndices >= MAX_QUARANTINED_MODEL_INDICES)
            return false;
        aQuarantinedModelIndices[nQuarantinedModelIndices++] = modelIndex;
        return true;
    }

    float Clamp01(float value)
    {
        return std::max(0.0f, std::min(1.0f, value));
    }

    float SmoothStep(float edge0, float edge1, float value)
    {
        if (edge0 == edge1)
            return value < edge0 ? 0.0f : 1.0f;

        const float t = Clamp01((value - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    // Quintic easing gives a softer transition at both ends than SmoothStep.
    float SmootherStep(float edge0, float edge1, float value)
    {
        if (edge0 == edge1)
            return value < edge0 ? 0.0f : 1.0f;

        const float t = Clamp01((value - edge0) / (edge1 - edge0));
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    struct CStreakSample
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float recipZ = 0.0f;
        unsigned char red = 255;
        unsigned char green = 255;
        unsigned char blue = 255;
        float intensity = 1.0f;
        // Independent angular visibility used to scale streak length under oblique views.
        float angleFade = 1.0f;
        bool valid = false;
    };

    struct CStreakHistory
    {
        CStreakSample aSamples[HEADLIGHT_STREAK_HISTORY];
        unsigned int lastUpdatedFrame = 0xFFFFFFFFu;
        unsigned int lastSeenFrame = 0;
        int sampleCount = 0;
        bool active = false;
        bool isRear = false;
        bool isReverse = false;
        float sizeScale = 1.0f;
    };

    std::unordered_map<unsigned int, CStreakHistory> StreakHistoryById;

    bool ProjectLightToScreen(const CVector& worldPos, CStreakSample& sample,
                             const HeadlightColour& colour, float intensity,
                             float angleFade)
    {
        if (!IsSaneWorldLightPosition(worldPos))
            return false;

        const RwV3d position = { worldPos.x, worldPos.y, worldPos.z };
        RwV3d screenPosition{};
        float projectedWidth = 0.0f;
        float projectedHeight = 0.0f;

        if (!CSprite::CalcScreenCoors(position, &screenPosition,
                                      &projectedWidth, &projectedHeight, false, false))
            return false;
        if (!std::isfinite(screenPosition.x) || !std::isfinite(screenPosition.y) ||
            !std::isfinite(screenPosition.z) || !std::isfinite(projectedWidth) ||
            !std::isfinite(projectedHeight))
            return false;

        // CalcScreenCoors can succeed for points that project outside the viewport.
        // Read the actual active camera raster dimensions (not the maximum desktop
        // resolution), otherwise off-screen lights can create long lines across view.
        if (!TheCamera.m_pRwCamera)
            return false;

        RwRaster* cameraRaster = RwCameraGetRaster(TheCamera.m_pRwCamera);
        if (!cameraRaster)
            return false;

        const float viewportWidth = static_cast<float>(RwRasterGetWidth(cameraRaster));
        const float viewportHeight = static_cast<float>(RwRasterGetHeight(cameraRaster));
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
            screenPosition.x < 0.0f || screenPosition.x >= viewportWidth ||
            screenPosition.y < 0.0f || screenPosition.y >= viewportHeight)
            return false;

        // Clamp the custom streak to a shorter range and fade it before the cutoff.
        // screenPosition.z is the projected camera-space depth used by the sprite API.
        if (screenPosition.z <= 0.5f || screenPosition.z >= fStreakDistanceFadeEnd)
            return false;

        sample.x = screenPosition.x;
        sample.y = screenPosition.y;
        sample.z = screenPosition.z;
        sample.recipZ = 1.0f / screenPosition.z;
        // Keep colour separate from intensity. Applying intensity to both RGB
        // and alpha would square the fade and make the transition look abrupt.
        sample.red = colour.red;
        sample.green = colour.green;
        sample.blue = colour.blue;
        const float distanceFade = 1.0f - SmoothStep(
            fStreakDistanceFadeStart, fStreakDistanceFadeEnd, screenPosition.z);
        sample.intensity = Clamp01(intensity) * distanceFade;
        sample.angleFade = Clamp01(angleFade);
        sample.valid = sample.recipZ > 0.0f && sample.intensity > 0.001f;
        return sample.valid;
    }

    void UpdateStreakHistory(unsigned int coronaId, const CVector& worldPos,
                             const HeadlightColour& colour, float intensity,
                             bool isRear, float angleFade,
                             float sizeScale, bool isReverse)
    {
        if (!bCustomStreakEnabled)
            return;

        CStreakHistory& history = StreakHistoryById[coronaId];
        history.isRear = isRear;
        history.isReverse = isReverse;
        history.sizeScale = std::max(0.1f, std::min(1.0f, sizeScale));
        const unsigned int frame = CTimer::m_FrameCounter;
        CStreakSample current;

        history.lastSeenFrame = frame;
        history.active = true;

        if (!ProjectLightToScreen(worldPos, current, colour, intensity, angleFade))
        {
            history.active = false;
            history.sampleCount = 0;
            return;
        }

        if (history.lastUpdatedFrame != frame)
        {
            for (int i = HEADLIGHT_STREAK_HISTORY - 1; i > 0; --i)
                history.aSamples[i] = history.aSamples[i - 1];

            if (history.sampleCount < HEADLIGHT_STREAK_HISTORY)
                ++history.sampleCount;

            history.lastUpdatedFrame = frame;
        }

        history.aSamples[0] = current;
    }

    void ClearStreakHistory(unsigned int coronaId)
    {
        auto it = StreakHistoryById.find(coronaId);
        if (it != StreakHistoryById.end())
        {
            it->second.active = false;
            it->second.sampleCount = 0;
        }
    }

    // Draw immediately instead of relying on the shared sprite buffer. The
    // stock streak raster is still used, but every history sample is emitted as
    // a visible screen-space flare. This mirrors re3's per-corona history while
    // avoiding buffer state/order problems in a Plugin-SDK drawing callback.
    void RenderStreakSprite(const CStreakSample& sample, float halfWidth,
                            float halfHeight, unsigned char red,
                            unsigned char green, unsigned char blue,
                            short intensity)
    {
        if (!sample.valid || sample.recipZ <= 0.0f || intensity <= 0 ||
            !std::isfinite(sample.x) || !std::isfinite(sample.y) ||
            !std::isfinite(sample.z) || !std::isfinite(sample.recipZ) ||
            !std::isfinite(halfWidth) || !std::isfinite(halfHeight) ||
            halfWidth <= 0.0f || halfHeight <= 0.0f)
            return;

        // SA's sprite implementation scales RGB by the short intensity value;
        // the byte after rhw is vertex alpha. Keep vertex alpha fully enabled
        // for additive blending, fade RGB through intensity, and use default UVs.
        CSprite::RenderOneXLUSprite(
            sample.x, sample.y, sample.z,
            halfWidth, halfHeight,
            red, green, blue,
            intensity, sample.recipZ,
            255, 0, 0
        );
    }

    void RenderStreaks()
    {
        const unsigned int frame = CTimer::m_FrameCounter;
        if (!bCustomStreakEnabled)
        {
            StreakHistoryById.clear();
            return;
        }
        if (!gpCoronaTexture)
            return;

        // GTA SA stores the horizontal flare texture under HEADLIGHTLINE.
        // CORONATYPE_STREAK is a different, empty texture slot in SA Plugin-SDK.
        RwTexture* streakTexture = gpCoronaTexture[CORONATYPE_HEADLIGHTLINE];
        if (!streakTexture)
            return;
        RwRaster* streakRaster = RwTextureGetRaster(streakTexture);
        if (!streakRaster)
            return;

        // Use additive blending and restore RenderWare state after drawing.
        void* previousRaster = nullptr;
        void* previousSrcBlend = nullptr;
        void* previousDstBlend = nullptr;
        void* previousVertexAlpha = nullptr;
        void* previousZWrite = nullptr;
        void* previousZTest = nullptr;

        RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &previousRaster);
        RwRenderStateGet(rwRENDERSTATESRCBLEND, &previousSrcBlend);
        RwRenderStateGet(rwRENDERSTATEDESTBLEND, &previousDstBlend);
        RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &previousVertexAlpha);
        RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &previousZWrite);
        RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &previousZTest);

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, streakRaster);
        RwRenderStateSet(rwRENDERSTATESRCBLEND,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(rwBLENDSRCALPHA)));
        RwRenderStateSet(rwRENDERSTATEDESTBLEND,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(rwBLENDONE)));
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(TRUE)));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(FALSE)));
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(TRUE)));

        for (auto it = StreakHistoryById.begin(); it != StreakHistoryById.end(); )
        {
            CStreakHistory& history = it->second;
            if (frame - history.lastSeenFrame > 1u)
            {
                it = StreakHistoryById.erase(it);
                continue;
            }

            if (!history.active || history.sampleCount < 1 || !history.aSamples[0].valid)
            {
                ++it;
                continue;
            }

            const CStreakSample& current = history.aSamples[0];
            const float currentIntensity = Clamp01(current.intensity);
            if (currentIntensity <= 0.001f)
            {
                ++it;
                continue;
            }

            // Draw one main sprite; repeated full-size samples would over-brighten
            // a stationary lamp under additive blending.
            const float baseHalfWidth = history.isRear
                ? REAR_STREAK_BASE_HALF_WIDTH : HEADLIGHT_STREAK_BASE_HALF_WIDTH;
            const float maxHalfWidth = history.isRear
                ? REAR_STREAK_MAX_HALF_WIDTH : HEADLIGHT_STREAK_MAX_HALF_WIDTH;
            const float streakLengthScale = !history.isRear
                ? fFrontStreakLengthScale
                : (history.isReverse ? fReverseStreakLengthScale : fRearStreakLengthScale);
            const float streakHeightScale = history.isRear
                ? fRearStreakHeightScale : fFrontStreakHeightScale;
            const float halfHeight = (history.isRear
                ? REAR_STREAK_HALF_HEIGHT : HEADLIGHT_STREAK_HALF_HEIGHT) *
                history.sizeScale * streakHeightScale;
            const float rawHalfWidth = (baseHalfWidth +
                (maxHalfWidth - baseHalfWidth) * currentIntensity) *
                history.sizeScale * streakLengthScale;
            // Angle controls streak length independently of brightness. A quartic
            // taper removes the minimum-width floor near a broadside view.
            const float angleFacing = Clamp01(current.angleFade);
            const float angleWidthScale = std::pow(angleFacing, fSideAngleExponent);
            // Reduce thickness slightly as the streak shortens.
            const float angleHeightScale = 0.65f + 0.35f * angleFacing;
            // Intensity affects width modestly; alpha controls brightness.
            const float intensityWidthFade = 0.55f + 0.45f * SmootherStep(0.025f, 0.60f, currentIntensity);
            const float widthFade = intensityWidthFade * angleWidthScale;
            const float mainHalfWidth = rawHalfWidth * widthFade;
            const float mainHalfHeight = halfHeight * angleHeightScale;
            if (mainHalfWidth < 1.0f)
            {
                ++it;
                continue;
            }
            const float baseAlpha = history.isRear
                ? REAR_STREAK_BASE_ALPHA : HEADLIGHT_STREAK_BASE_ALPHA;
            const float streakBrightness = !history.isRear
                ? fFrontStreakBrightness
                : (history.isReverse ? fReverseStreakBrightness : fRearStreakBrightness);
            const short mainAlpha = static_cast<short>(std::min(
                255.0f, baseAlpha * currentIntensity * streakBrightness));
            RenderStreakSprite(current, mainHalfWidth, mainHalfHeight,
                               current.red, current.green, current.blue, mainAlpha);

            // Add faint history echoes only while the lamp moves on-screen.
            int oldestValidIndex = -1;
            for (int i = history.sampleCount - 1; i >= 1; --i)
            {
                if (history.aSamples[i].valid)
                {
                    oldestValidIndex = i;
                    break;
                }
            }

            if (oldestValidIndex >= 1)
            {
                const CStreakSample& oldest = history.aSamples[oldestValidIndex];
                const float dx = current.x - oldest.x;
                const float dy = current.y - oldest.y;
                const float motionSquared = dx * dx + dy * dy;

                if (motionSquared > 9.0f && currentIntensity > 0.12f &&
                    current.angleFade > 0.18f && widthFade > 0.08f)
                {
                    const int echoCount = std::min(4, oldestValidIndex);
                    for (int echo = 1; echo <= echoCount; ++echo)
                    {
                        const float t = static_cast<float>(echo) / (echoCount + 1.0f);
                        CStreakSample interpolated = current;
                        interpolated.x = current.x + (oldest.x - current.x) * t;
                        interpolated.y = current.y + (oldest.y - current.y) * t;
                        interpolated.z = current.z + (oldest.z - current.z) * t;
                        interpolated.recipZ = current.recipZ + (oldest.recipZ - current.recipZ) * t;
                        const float echoBaseAlpha = history.isRear ? 20.0f : 24.0f;
                        const short echoAlpha = static_cast<short>(std::min(
                            255.0f, echoBaseAlpha * currentIntensity * streakBrightness * (1.0f - t)));
                        RenderStreakSprite(interpolated,
                            std::max(1.0f, mainHalfWidth * 0.18f),
                            (history.isRear ? 3.6f : 4.4f) * angleWidthScale * angleHeightScale * history.sizeScale,
                            interpolated.red, interpolated.green, interpolated.blue,
                            echoAlpha);
                    }
                }
            }

            ++it;
        }

        // Restore the previous RenderWare state.
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, previousRaster);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, previousSrcBlend);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, previousDstBlend);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, previousVertexAlpha);
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, previousZWrite);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, previousZTest);
    }

    CVector TransformLocalPosition(const CVehicle* vehicle, const CVector& localPosition)
    {
        const CMatrix& matrix = *vehicle->m_matrix;
        return CVector(
            matrix.pos.x + matrix.right.x * localPosition.x +
                matrix.up.x * localPosition.y + matrix.at.x * localPosition.z,
            matrix.pos.y + matrix.right.y * localPosition.x +
                matrix.up.y * localPosition.y + matrix.at.y * localPosition.z,
            matrix.pos.z + matrix.right.z * localPosition.x +
                matrix.up.z * localPosition.y + matrix.at.z * localPosition.z
        );
    }

    CVector MirrorHeadlightPosition(const CVector& position)
    {
        return CVector(-position.x, position.y, position.z);
    }

    bool IsPositionSet(const CVector& position)
    {
        return IsSaneLocalLightPosition(position) &&
               (position.x != 0.0f || position.y != 0.0f || position.z != 0.0f);
    }

    void GetVehicleLightPositions(CVehicle* vehicle, CVehicleModelInfo* modelInfo,
                                  int dummyIndex, CVector& lightPos,
                                  CVector& lightL, CVector& lightR)
    {
        if (modelInfo && modelInfo->m_pVehicleStruct)
        {
            const CVector dummyPos = modelInfo->m_pVehicleStruct->m_avDummyPos[dummyIndex];
            if (IsPositionSet(dummyPos))
                lightPos = dummyPos;
        }

        const CVector lightPosR = MirrorHeadlightPosition(lightPos);
        lightL = TransformLocalPosition(vehicle, lightPos);
        lightR = TransformLocalPosition(vehicle, lightPosR);
    }

    float GetCameraBehindness(const CVehicle* vehicle, float& camDist)
    {
        const CVector& cameraPos = TheCamera.m_mCameraMatrix.pos;
        const CVector& vehiclePos = vehicle->m_matrix->pos;
        const CVector& forward = vehicle->m_matrix->up;

        if (!IsSaneWorldLightPosition(cameraPos) ||
            !IsSaneWorldLightPosition(vehiclePos) || !IsFiniteVector(forward))
        {
            camDist = 1000000.0f;
            return 0.0f;
        }

        CVector lookVector(
            vehiclePos.x - cameraPos.x,
            vehiclePos.y - cameraPos.y,
            vehiclePos.z - cameraPos.z
        );

        camDist = std::sqrt(
            lookVector.x * lookVector.x +
            lookVector.y * lookVector.y +
            lookVector.z * lookVector.z
        );

        if (camDist > 0.0001f)
        {
            const float inverseDistance = 1.0f / camDist;
            lookVector.x *= inverseDistance;
            lookVector.y *= inverseDistance;
            lookVector.z *= inverseDistance;
        }
        else
        {
            lookVector = CVector(1.0f, 0.0f, 0.0f);
        }

        float behindness = lookVector.x * forward.x +
                           lookVector.y * forward.y +
                           lookVector.z * forward.z;

        // Clamp floating-point drift before calculating the camera angle.
        if (behindness < -1.0f)
            behindness = -1.0f;
        else if (behindness > 1.0f)
            behindness = 1.0f;

        return behindness;
    }

    HeadlightColour GetHeadlightColour(const CVehicle* vehicle)
    {
        // SA's handling data supports halogen headlights. Keep the two stock
        // colour variants used by the re3/reVC algorithm.
        if (vehicle->m_pHandlingData && vehicle->m_nHandlingFlags.bHalogenLights)
            return { 150, 150, 195 };

        return { 160, 160, 140 };
    }

    void RegisterLightCorona(unsigned int coronaId, const CVector& position,
                             const HeadlightColour& colour, unsigned char alpha,
                             float size, float farClip, eCoronaType coronaType,
                             float angle)
    {
        if (!IsSaneWorldLightPosition(position) || !std::isfinite(size) ||
            !std::isfinite(farClip) || !std::isfinite(angle))
            return;

        CCoronas::RegisterCorona(
            coronaId, nullptr,
            colour.red, colour.green, colour.blue, alpha,
            position, size, farClip, coronaType, FLARETYPE_NONE,
            true, false, 0, angle, false,
            HEADLIGHT_NEAR_CLIP, 0, HEADLIGHT_FADE_SPEED, false, false
        );
    }

    void RegisterHeadlightCore(unsigned int coronaId, const CVector& position,
                               const HeadlightColour& colour, float size, float angle,
                               float fade)
    {
        const float scaledAlpha = 255.0f * Clamp01(fade) * Clamp01(fFrontCoreBrightness);
        const unsigned char alpha = static_cast<unsigned char>(scaledAlpha);
        RegisterLightCorona(coronaId, position, colour, alpha,
                            size * fFrontCoreSizeScale,
                            HEADLIGHT_CORE_FAR_CLIP, CORONATYPE_HEADLIGHT, angle);
    }

    void HideHeadlightCore(unsigned int coronaId, const CVector& worldPos, float angle)
    {
        // Re-register the same ID with zero alpha so the previous corona is
        // replaced immediately instead of lingering as a fading old instance.
        RegisterHeadlightCore(coronaId, worldPos, { 0, 0, 0 }, 1.2f, angle, 0);
    }

    void RegisterReverseLightCore(unsigned int coronaId, const CVector& position,
                                  float angle, float fade, float sizeScale)
    {
        const float scaledAlpha = std::min(
            255.0f, 190.0f * Clamp01(fade) * std::max(0.0f, fReverseCoreBrightness));
        RegisterLightCorona(coronaId, position, { 200, 215, 235 },
                            static_cast<unsigned char>(scaledAlpha),
                            0.9f * REVERSE_LIGHT_CORE_SIZE_MULTIPLIER *
                                sizeScale * fReverseCoreSizeScale,
                            REVERSE_LIGHT_FAR_CLIP, CORONATYPE_HEADLIGHT, angle);
    }

    bool IsVehicleReversing(const CVehicle* vehicle)
    {
        if (vehicle->m_nCurrentGear != 0)
            return false;

        // Gear zero is used by the reverse gear in SA. Require reverse input or
        // actual backward movement so neutral/parked vehicles do not glow.
        const CVector& forward = vehicle->m_matrix->up;
        const float forwardSpeed = vehicle->m_vecMoveSpeed.x * forward.x +
                                   vehicle->m_vecMoveSpeed.y * forward.y +
                                   vehicle->m_vecMoveSpeed.z * forward.z;

        return vehicle->m_fGasPedal < -0.05f || forwardSpeed < -0.01f;
    }

    void ProcessReverseLights(bool reversing, bool isMotorcycle,
                              float behindness, float camDist,
                              unsigned int baseId, float angle,
                              const CVector& lightL, const CVector& lightR)
    {
        // Reverse cores use a narrow angle and distance envelope.
        const float coreAngleFade = SmoothStep(
            fReverseCoreAngleFadeStart, fReverseCoreAngleFadeEnd, behindness);
        const float coreDistanceFade = 1.0f - SmoothStep(
            fFrontCoreDistanceFadeStart, fFrontCoreDistanceFadeEnd, camDist);
        const float coreFade = reversing ? coreAngleFade * coreDistanceFade : 0.0f;

        const float coreSizeScale = isMotorcycle
            ? fBikeReverseCoreSizeMult : 1.0f;
        RegisterReverseLightCore(baseId + 0x79u, lightL, angle, coreFade, coreSizeScale);
        RegisterReverseLightCore(baseId + 0x7Au, lightR, angle, coreFade, coreSizeScale);
    }

    void RegisterRearLampCore(unsigned int coronaId, const CVector& position,
                              const HeadlightColour& colour, float angle, float fade,
                              float sizeScale, bool isReverse)
    {
        // The round side-view replacement is intentionally smaller than the
        // main rear-view streak, so it reads as a compact lamp instead of a halo.
        const float maxAlpha = isReverse ? 78.0f : 95.0f;
        const float brightness = isReverse
            ? fReverseCoreBrightness : fRearRoundCoreBrightness;
        const float coreSizeScale = isReverse
            ? fReverseCoreSizeScale : fRearRoundCoreSizeScale;
        const float scaledAlpha = std::min(
            255.0f, maxAlpha * Clamp01(fade) * std::max(0.0f, brightness));
        RegisterLightCorona(coronaId, position, colour,
                            static_cast<unsigned char>(scaledAlpha),
                            0.30f * sizeScale * coreSizeScale,
                            HEADLIGHT_CORE_FAR_CLIP, CORONATYPE_HEADLIGHT, angle);
    }

    void ProcessRearLights(CVehicle* vehicle, bool reversing, bool isMotorcycle,
                          float behindness, float camDist,
                          unsigned int baseId,
                          const CVector& lightL, const CVector& lightR)
    {
        const unsigned int leftStreakId = baseId + 0x7Du;
        const unsigned int rightStreakId = baseId + 0x7Eu;
        const float rearVisibilityFade = SmootherStep(
            -fFrontAngleFadeEnd, -fFrontAngleFadeStart, behindness);
        // Fade and shorten the streak as the camera moves behind the vehicle;
        // the compact side-view corona prevents the line crossing the body.
        const float rearVisibility = rearVisibilityFade * rearVisibilityFade;

        const bool effectEnabled = vehicle->bLightsOn || reversing;
        const float normalIntensity = std::max(0.0f,
            std::min(1.0f, 0.38f + 0.30f * behindness));

        // The streak and side-view corona share the same colour transition.
        const HeadlightColour streakColour = reversing
            ? HeadlightColour{ 205, 220, 250 }
            : HeadlightColour{ 255, 24, 12 };
        const float streakBrightness = reversing ? 0.60f : normalIntensity;
        // Fade only the normal red taillight streak in with distance. Near the
        // camera the stock red tail lamps remain, but the custom long red line is
        // hidden; it then grows smoothly instead of popping in at one distance.
        const float redTailDistanceFade = SmootherStep(
            fRearTailDistanceFadeStart, fRearTailDistanceFadeEnd,
            camDist);
        const float distanceFade = reversing ? 1.0f : redTailDistanceFade;
        const float intensity = streakBrightness * rearVisibility * distanceFade;

        // Cross-fade the compact side-view corona into the rear streak.
        const float roundEntryFade = SmootherStep(
            fRearRoundAngleEntryStart, fRearRoundAngleEntryEnd, behindness);
        const float roundExitFade = 1.0f - SmootherStep(
            fRearRoundAngleExitStart, fRearRoundAngleExitEnd, behindness);
        const float roundFade = effectEnabled
            ? roundEntryFade * roundExitFade
            : 0.0f;
        const float angle = std::acos(std::max(-1.0f, std::min(1.0f, behindness)));
        const float reverseLampSizeScale = reversing && isMotorcycle
            ? fBikeReverseCoreSizeMult : 1.0f;
        RegisterRearLampCore(baseId + 0x81u, lightL, streakColour, angle,
                                  roundFade, reverseLampSizeScale, reversing);
        RegisterRearLampCore(baseId + 0x82u, lightR, streakColour, angle,
                                  roundFade, reverseLampSizeScale, reversing);

        // Do not register another long native corona for ordinary tail lamps.
        // The stock lamps remain intact; the custom layer draws the elongated line
        // from the rear view and the small circular replacement near side angles.
        if (!effectEnabled || rearVisibility <= 0.001f)
        {
            ClearStreakHistory(leftStreakId);
            ClearStreakHistory(rightStreakId);
            return;
        }

        const float streakSizeScale = reversing && isMotorcycle
            ? fBikeReverseStreakSizeMult : 1.0f;
        UpdateStreakHistory(leftStreakId, lightL, streakColour, intensity,
                              true, rearVisibilityFade, streakSizeScale, reversing);
        UpdateStreakHistory(rightStreakId, lightR, streakColour, intensity,
                              true, rearVisibilityFade, streakSizeScale, reversing);
    }

    // The stock damage manager is available on CAutomobile-derived road cars.
    // Do not cast bikes, boats, aircraft or trailers to CAutomobile.
    void GetHeadlightWorkingState(CVehicle* vehicle, CVehicleModelInfo* modelInfo,
                                   bool& leftWorking, bool& rightWorking)
    {
        leftWorking = rightWorking = true;

        if (!modelInfo || (modelInfo->m_nVehicleType != VEHICLE_AUTOMOBILE &&
                           modelInfo->m_nVehicleType != VEHICLE_MTRUCK))
            return;

        CAutomobile* automobile = static_cast<CAutomobile*>(vehicle);
        leftWorking = automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_LEFT) == 0;
        rightWorking = automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_RIGHT) == 0;
    }

    void ProcessVehicleLights(CVehicle* vehicle)
    {
        if (!vehicle || !vehicle->m_matrix)
            return;

        const CMatrix& vehicleMatrix = *vehicle->m_matrix;
        if (!IsSaneWorldLightPosition(vehicleMatrix.pos) ||
            !IsFiniteVector(vehicleMatrix.right) ||
            !IsFiniteVector(vehicleMatrix.up) ||
            !IsFiniteVector(vehicleMatrix.at))
            return;

        CVehicleModelInfo* modelInfo = static_cast<CVehicleModelInfo*>(
            CModelInfo::GetModelInfo(vehicle->m_nModelIndex));

        // Without the vehicle structure, dummy data is unavailable/unreliable.
        // Skip this model's custom lighting; the game's own light system remains untouched.
        if (!modelInfo || !modelInfo->m_pVehicleStruct)
            return;

        bool leftLightWorking = true;
        bool rightLightWorking = true;
        GetHeadlightWorkingState(vehicle, modelInfo, leftLightWorking, rightLightWorking);

        CVector lightPos(DEFAULT_HEADLIGHT_X, DEFAULT_HEADLIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector lightL, lightR;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_HEADLIGHTS,
                                 lightPos, lightL, lightR);

        float camDist = 0.0f;
        const float behindness = GetCameraBehindness(vehicle, camDist);
        if (!std::isfinite(behindness) || !std::isfinite(camDist) || camDist < 0.0f)
            return;

        const unsigned int baseId = static_cast<unsigned int>(
            reinterpret_cast<std::uintptr_t>(vehicle));
        const float angle = std::acos(behindness);
        const HeadlightColour colour = GetHeadlightColour(vehicle);
        const bool reversing = IsVehicleReversing(vehicle);
        const bool isMotorcycle = modelInfo && modelInfo->m_nVehicleType == VEHICLE_BIKE;

        CVector localRearLeft(-DEFAULT_HEADLIGHT_X, DEFAULT_REAR_LIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector rearLightL, rearLightR;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_REAR_LIGHTS,
                                 localRearLeft, rearLightL, rearLightR);

        if (!IsSaneWorldLightPosition(lightL) || !IsSaneWorldLightPosition(lightR) ||
            !IsSaneWorldLightPosition(rearLightL) || !IsSaneWorldLightPosition(rearLightR) ||
            !std::isfinite(camDist) || camDist < 0.0f)
            return;

        if (vehicle->bLightsOn)
        {
            // Fade front streaks smoothly as the camera moves toward the side.
            const float frontVisibilityFade = 1.0f - SmootherStep(
                fFrontAngleFadeStart, fFrontAngleFadeEnd, behindness);
            // The extra fade limits screen-space lines crossing the body.
            const float frontVisibility = frontVisibilityFade * frontVisibilityFade;
            const float rawIntensity = -0.5f * behindness + 0.3f;
            const float baseIntensity = Clamp01(rawIntensity);
            const float intensity = baseIntensity * frontVisibility;
            // Match damage state to the dummy side; models can store either side first.
            const bool firstLampIsVehicleLeft = lightPos.x < 0.0f;
            const bool firstLightWorking = firstLampIsVehicleLeft
                ? leftLightWorking : rightLightWorking;
            const bool secondLightWorking = firstLampIsVehicleLeft
                ? rightLightWorking : leftLightWorking;

            // Keep the two physical lamps independent: a damaged lamp clears only
            // its own streak and core, not the one on the opposite side.
            if (firstLightWorking)
                UpdateStreakHistory(baseId + 0x71u, lightL, colour, intensity,
                                    false, frontVisibilityFade, 1.0f, false);
            else
                ClearStreakHistory(baseId + 0x71u);

            if (secondLightWorking)
                UpdateStreakHistory(baseId + 0x72u, lightR, colour, intensity,
                                    false, frontVisibilityFade, 1.0f, false);
            else
                ClearStreakHistory(baseId + 0x72u);

            // Keep the round core's angle and distance fades narrower than the streak.
            const float coreAngleFade = 1.0f - SmoothStep(
                fFrontCoreAngleFadeStart, fFrontCoreAngleFadeEnd, behindness);
            const float coreDistanceFade = 1.0f - SmoothStep(
                fFrontCoreDistanceFadeStart, fFrontCoreDistanceFadeEnd, camDist);
            const float coreFade = coreAngleFade * coreDistanceFade;

            if (firstLightWorking && coreFade > 0.001f)
                RegisterHeadlightCore(baseId + 0x73u, lightL, colour, 1.2f, angle, coreFade);
            else
                HideHeadlightCore(baseId + 0x73u, lightL, angle);

            if (secondLightWorking && coreFade > 0.001f)
                RegisterHeadlightCore(baseId + 0x74u, lightR, colour, 1.2f, angle, coreFade);
            else
                HideHeadlightCore(baseId + 0x74u, lightR, angle);
        }
        else
        {
            ClearStreakHistory(baseId + 0x71u);
            ClearStreakHistory(baseId + 0x72u);
            HideHeadlightCore(baseId + 0x73u, lightL, angle);
            HideHeadlightCore(baseId + 0x74u, lightR, angle);
        }

        // Keep stock rear lamps and add the custom screen-space layer.
        ProcessRearLights(vehicle, reversing, isMotorcycle, behindness,
                                 camDist, baseId, rearLightL, rearLightR);

        // Reverse lamps are independent of the headlight switch.
        ProcessReverseLights(reversing, isMotorcycle, behindness,
                                   camDist, baseId, angle,
                                   rearLightL, rearLightR);
    }

#if defined(_MSC_VER)
    int FilterRecoverableVehicleException(unsigned long exceptionCode)
    {
        // Only recover from memory-access failures. Other exceptions should retain
        // normal crash/debugger behavior rather than being silently swallowed.
        if (exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            exceptionCode == EXCEPTION_IN_PAGE_ERROR)
            return EXCEPTION_EXECUTE_HANDLER;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ProcessVehicleLightsGuarded(CVehicle* vehicle)
    {
        if (bRuntimeFaultDetected || !vehicle)
            return;

        // Keep this SEH boundary free of C++ objects with non-trivial destructors.
        // It is a last-resort barrier for malformed add-on vehicle/model data.
        volatile int modelIndex = -1;
        __try
        {
            modelIndex = vehicle->m_nModelIndex;
            if (IsModelQuarantined(modelIndex))
                return;
            ProcessVehicleLights(vehicle);
        }
        __except (FilterRecoverableVehicleException(GetExceptionCode()))
        {
            if (modelIndex < 0 || !QuarantineModel(modelIndex))
            {
                // Unknown fault source or quarantine table full: fail closed.
                bRuntimeFaultDetected = true;
                bCustomStreakEnabled = 0;
            }
        }
    }

    void RenderStreaksGuarded()
    {
        if (bRuntimeFaultDetected)
            return;

        __try
        {
            RenderStreaks();
        }
        __except (FilterRecoverableVehicleException(GetExceptionCode()))
        {
            // Rendering faults are not safely attributable to one vehicle. Turn
            // off only this plugin's custom layer; stock game rendering continues.
            bRuntimeFaultDetected = true;
            bCustomStreakEnabled = 0;
        }
    }
#else
    // GTA SA ASI plugins are normally built with MSVC. Keep other toolchains
    // buildable, while retaining all deterministic pointer/coordinate validation.
    void ProcessVehicleLightsGuarded(CVehicle* vehicle)
    {
        if (!bRuntimeFaultDetected && vehicle)
            ProcessVehicleLights(vehicle);
    }

    void RenderStreaksGuarded()
    {
        if (!bRuntimeFaultDetected)
            RenderStreaks();
    }
#endif

    void ResetStreakDebugSettings()
    {
        bCustomStreakEnabled = 1;
        fFrontStreakLengthScale = 1.0f;
        fFrontStreakHeightScale = 1.0f;
        fFrontStreakBrightness = 1.0f;
        fFrontCoreSizeScale = 1.0f;
        fFrontCoreBrightness = 0.84f;
        fRearStreakLengthScale = 1.0f;
        fRearStreakHeightScale = 1.0f;
        fRearStreakBrightness = 1.0f;
        fRearRoundCoreSizeScale = 1.0f;
        fRearRoundCoreBrightness = 1.0f;
        fReverseStreakLengthScale = 1.0f;
        fReverseStreakBrightness = 1.0f;
        fReverseCoreSizeScale = 1.0f;
        fReverseCoreBrightness = 1.0f;
        fBikeReverseStreakSizeMult = 0.55f;
        fBikeReverseCoreSizeMult = 0.60f;
        fSideAngleExponent = 4.0f;
        fFrontAngleFadeStart = -0.88f;
        fFrontAngleFadeEnd = -0.25f;
        fFrontCoreAngleFadeStart = -0.995f;
        fFrontCoreAngleFadeEnd = -0.88f;
        fFrontCoreDistanceFadeStart = 23.0f;
        fFrontCoreDistanceFadeEnd = 32.0f;
        fReverseCoreAngleFadeStart = 0.80f;
        fReverseCoreAngleFadeEnd = 0.97f;
        fRearTailDistanceFadeStart = 9.0f;
        fRearTailDistanceFadeEnd = 23.0f;
        fStreakDistanceFadeStart = 25.0f;
        fStreakDistanceFadeEnd = 40.0f;
        fRearRoundAngleEntryStart = -0.20f;
        fRearRoundAngleEntryEnd = 0.12f;
        fRearRoundAngleExitStart = 0.28f;
        fRearRoundAngleExitEnd = 0.78f;
    }

    void RegisterStreakDebugMenu()
    {
        if (bDebugMenuRegistered || !DebugMenuLoad())
            return;

        // The official API loader marks itself loaded even if an export is missing;
        // verify every function used below before registering any entries.
        if (!gDebugMenuAPI.addint32 || !gDebugMenuAPI.addfloat32 ||
            !gDebugMenuAPI.addcmd || !gDebugMenuAPI.setwrap)
            return;

        static const char* toggleStrings[] = { "Off", "On" };
        DebugMenuEntry* toggle = DebugMenuAddVar(
            "SA Streaks|General", "Custom streak lines",
            &bCustomStreakEnabled, nullptr, 1, 0, 1, toggleStrings);
        if (toggle)
            DebugMenuEntrySetWrap(toggle, true);
        DebugMenuAddCmd("SA Streaks|General", "Reset all values", ResetStreakDebugSettings);

        DebugMenuAddVar("SA Streaks|Front", "Streak length", &fFrontStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Front", "Streak height", &fFrontStreakHeightScale,
                        nullptr, 0.05f, 0.50f, 2.00f);
        DebugMenuAddVar("SA Streaks|Front", "Streak brightness", &fFrontStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Front", "Round core size", &fFrontCoreSizeScale,
                        nullptr, 0.05f, 0.40f, 1.80f);
        DebugMenuAddVar("SA Streaks|Front", "Round core brightness", &fFrontCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Front", "Angle fade start", &fFrontAngleFadeStart,
                        nullptr, 0.02f, -0.99f, -0.30f);
        DebugMenuAddVar("SA Streaks|Front", "Angle fade end", &fFrontAngleFadeEnd,
                        nullptr, 0.02f, -0.25f, 0.50f);
        DebugMenuAddVar("SA Streaks|Front", "Core angle start", &fFrontCoreAngleFadeStart,
                        nullptr, 0.005f, -1.0f, -0.905f);
        DebugMenuAddVar("SA Streaks|Front", "Core angle end", &fFrontCoreAngleFadeEnd,
                        nullptr, 0.005f, -0.90f, -0.50f);
        DebugMenuAddVar("SA Streaks|Front", "Core distance start", &fFrontCoreDistanceFadeStart,
                        nullptr, 1.0f, 5.0f, 30.0f);
        DebugMenuAddVar("SA Streaks|Front", "Core distance end", &fFrontCoreDistanceFadeEnd,
                        nullptr, 1.0f, 31.0f, 45.0f);

        DebugMenuAddVar("SA Streaks|Rear", "Streak length", &fRearStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Rear", "Streak height", &fRearStreakHeightScale,
                        nullptr, 0.05f, 0.50f, 2.00f);
        DebugMenuAddVar("SA Streaks|Rear", "Streak brightness", &fRearStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Rear", "Side core size", &fRearRoundCoreSizeScale,
                        nullptr, 0.05f, 0.20f, 2.00f);
        DebugMenuAddVar("SA Streaks|Rear", "Side core brightness", &fRearRoundCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Distance fade start", &fRearTailDistanceFadeStart,
                        nullptr, 1.0f, 2.0f, 20.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Distance fade end", &fRearTailDistanceFadeEnd,
                        nullptr, 1.0f, 21.0f, 35.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry start", &fRearRoundAngleEntryStart,
                        nullptr, 0.02f, -0.50f, 0.10f);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry end", &fRearRoundAngleEntryEnd,
                        nullptr, 0.01f, 0.11f, 0.19f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit start", &fRearRoundAngleExitStart,
                        nullptr, 0.02f, 0.20f, 0.70f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit end", &fRearRoundAngleExitEnd,
                        nullptr, 0.02f, 0.71f, 0.99f);

        DebugMenuAddVar("SA Streaks|Reverse", "Streak length", &fReverseStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Reverse", "Streak brightness", &fReverseStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Reverse", "Round core size", &fReverseCoreSizeScale,
                        nullptr, 0.05f, 0.40f, 1.60f);
        DebugMenuAddVar("SA Streaks|Reverse", "Round core brightness", &fReverseCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Reverse", "Bike streak size", &fBikeReverseStreakSizeMult,
                        nullptr, 0.05f, 0.10f, 1.00f);
        DebugMenuAddVar("SA Streaks|Reverse", "Bike core size", &fBikeReverseCoreSizeMult,
                        nullptr, 0.05f, 0.10f, 1.00f);
        DebugMenuAddVar("SA Streaks|Reverse", "Core angle start", &fReverseCoreAngleFadeStart,
                        nullptr, 0.01f, 0.50f, 0.95f);
        DebugMenuAddVar("SA Streaks|Reverse", "Core angle end", &fReverseCoreAngleFadeEnd,
                        nullptr, 0.005f, 0.96f, 1.00f);

        DebugMenuAddVar("SA Streaks|Advanced", "Side angle exponent", &fSideAngleExponent,
                        nullptr, 0.25f, 2.0f, 8.0f);
        DebugMenuAddVar("SA Streaks|Advanced", "Streak distance start", &fStreakDistanceFadeStart,
                        nullptr, 1.0f, 5.0f, 38.0f);
        DebugMenuAddVar("SA Streaks|Advanced", "Streak distance end", &fStreakDistanceFadeEnd,
                        nullptr, 1.0f, 39.0f, 50.0f);

        bDebugMenuRegistered = true;
    }

    struct Bootstrap
    {
        Bootstrap()
        {
            // There are at most a few streak IDs per rendered vehicle. Reserve
            // ahead of time to avoid hash-table reallocation during render events.
            StreakHistoryById.reserve(512);
            plugin::Events::initGameEvent += RegisterStreakDebugMenu;
            plugin::Events::vehicleRenderEvent += ProcessVehicleLightsGuarded;
            plugin::Events::drawingEvent += RenderStreaksGuarded;
        }
    } pluginBootstrap;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
