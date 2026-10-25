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
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include <unordered_map>

extern "C"
{
    DebugMenuAPI gDebugMenuAPI = {};
}

// GTA San Andreas 1.0 US / Plugin-SDK.
// Adds re3-inspired streaks while retaining the game's normal light system.
// Screen-space history replaces the STREAK_ON flag unavailable through SA's corona API.

namespace
{
    constexpr float HEADLIGHT_CORE_FAR_CLIP = 45.0f;
    constexpr float REVERSE_LIGHT_CORE_SIZE_MULTIPLIER = 0.70f;
    constexpr int QUADBIKE_MODEL_ID = 471; // GTA San Andreas Quadbike model ID.
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
        unsigned char Red;
        unsigned char Green;
        unsigned char Blue;
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

    // Set in DllMain; the INI is resolved relative to this ASI/DLL, not the game working directory.
    HMODULE s_PluginModuleHandle = nullptr;

    // Runtime settings exposed through the debug menu and optional INI file.
    std::int32_t s_CustomStreakEnabled = 1;

    float s_FrontStreakLengthScale = 1.0f;
    float s_FrontStreakHeightScale = 1.0f;
    float s_FrontStreakBrightness = 1.0f;
    float s_FrontCoreSizeScale = 1.0f;
    float s_FrontCoreBrightness = 0.65f;

    float s_RearStreakLengthScale = 1.0f;
    float s_RearStreakHeightScale = 1.0f;
    float s_RearStreakBrightness = 1.0f;
    float s_RearRoundCoreSizeScale = 1.0f;
    float s_RearRoundCoreBrightness = 1.0f;

    float s_ReverseStreakLengthScale = 1.0f;
    float s_ReverseStreakBrightness = 1.0f;
    float s_ReverseCoreSizeScale = 1.0f;
    float s_ReverseCoreBrightness = 1.0f;

    float s_BikeReverseStreakSizeMultiplier = 0.55f;
    float s_BikeReverseCoreSizeMultiplier = 0.60f;
    float s_BikeReverseBrightnessMultiplier = 0.35f;

    float s_SideAngleExponent = 4.0f;
    float s_FrontAngleFadeStart = -0.88f;
    float s_FrontAngleFadeEnd = -0.25f;
    float s_FrontCoreAngleFadeStart = -0.995f;
    float s_FrontCoreAngleFadeEnd = -0.88f;
    float s_FrontCoreDistanceFadeStart = 23.0f;
    float s_FrontCoreDistanceFadeEnd = 32.0f;
    float s_ReverseCoreAngleFadeStart = 0.80f;
    float s_ReverseCoreAngleFadeEnd = 0.97f;

    float s_RearTailDistanceFadeStart = 9.0f;
    float s_RearTailDistanceFadeEnd = 23.0f;
    float s_StreakDistanceFadeStart = 25.0f;
    float s_StreakDistanceFadeEnd = 40.0f;
    float s_RearRoundAngleEntryStart = -0.20f;
    float s_RearRoundAngleEntryEnd = 0.12f;
    float s_RearRoundAngleExitStart = 0.28f;
    float s_RearRoundAngleExitEnd = 0.78f;

    bool s_DebugMenuRegistered = false;

    std::wstring GetPluginConfigPath()
    {
        if (!s_PluginModuleHandle)
            return {};

        // Use a large buffer so a long installation path does not silently truncate.
        std::vector<wchar_t> modulePath(32768, L'\0');
        const DWORD pathLength = GetModuleFileNameW(
            s_PluginModuleHandle, &modulePath[0], static_cast<DWORD>(modulePath.size()));
        if (pathLength == 0 || pathLength >= modulePath.size())
            return {};

        std::wstring configPath(&modulePath[0], pathLength);
        const std::wstring::size_type separator = configPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return {};

        configPath.resize(separator + 1);
        configPath += L"IIIStreakLights.ini";
        return configPath;
    }

    bool ReadIniNumber(const std::wstring& configPath, const wchar_t* section,
                       const wchar_t* key, float& result)
    {
        wchar_t text[128] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", text,
            static_cast<DWORD>(sizeof(text) / sizeof(text[0])), configPath.c_str());
        if (length == 0)
            return false;

        wchar_t* end = nullptr;
        const float value = std::wcstof(text, &end);
        if (end == text || !std::isfinite(value))
            return false;

        while (*end && std::iswspace(*end))
            ++end;
        // Percentage entries may be written as either "65" or "65%".
        if (*end == L'%')
        {
            ++end;
            while (*end && std::iswspace(*end))
                ++end;
        }
        if (*end != L'\0')
            return false;

        result = value;
        return true;
    }

    bool ReadIniBool(const std::wstring& configPath, const wchar_t* section,
                     const wchar_t* key, bool& result)
    {
        wchar_t text[32] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", text,
            static_cast<DWORD>(sizeof(text) / sizeof(text[0])), configPath.c_str());
        if (length == 0)
            return false;

        std::wstring value(text);
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });

        if (value == L"true" || value == L"yes" || value == L"on")
        {
            result = true;
            return true;
        }
        if (value == L"false" || value == L"no" || value == L"off")
        {
            result = false;
            return true;
        }

        float number = 0.0f;
        if (!ReadIniNumber(configPath, section, key, number))
            return false;
        result = number != 0.0f;
        return true;
    }

    void ReadIniPercent(const std::wstring& configPath, const wchar_t* section,
                        const wchar_t* key, float& setting,
                        float minimumPercent, float maximumPercent)
    {
        float percentage = 0.0f;
        if (ReadIniNumber(configPath, section, key, percentage))
        {
            percentage = std::max(minimumPercent, std::min(maximumPercent, percentage));
            setting = percentage / 100.0f;
        }
    }

    void ReadIniDistance(const std::wstring& configPath, const wchar_t* section,
                         const wchar_t* key, float& setting,
                         float minimumDistance, float maximumDistance)
    {
        float distance = 0.0f;
        if (ReadIniNumber(configPath, section, key, distance))
            setting = std::max(minimumDistance, std::min(maximumDistance, distance));
    }

    void ReadIniAngleDegrees(const std::wstring& configPath, const wchar_t* section,
                             const wchar_t* key, float& setting)
    {
        float degrees = 0.0f;
        if (!ReadIniNumber(configPath, section, key, degrees))
            return;

        degrees = std::max(0.0f, std::min(180.0f, degrees));
        constexpr float DEGREES_TO_RADIANS = 0.017453292519943295f;
        setting = std::cos(degrees * DEGREES_TO_RADIANS);
    }

    void LoadPluginConfig()
    {
        const std::wstring configPath = GetPluginConfigPath();
        if (configPath.empty())
            return;

        // Missing file/keys leave the code-defined defaults unchanged.
        bool enabled = s_CustomStreakEnabled != 0;
        if (ReadIniBool(configPath, L"General", L"Enabled", enabled))
            s_CustomStreakEnabled = enabled ? 1 : 0;

        ReadIniPercent(configPath, L"Front", L"StreakLength", s_FrontStreakLengthScale, 25.0f, 250.0f);
        ReadIniPercent(configPath, L"Front", L"StreakHeight", s_FrontStreakHeightScale, 50.0f, 200.0f);
        ReadIniPercent(configPath, L"Front", L"StreakBrightness", s_FrontStreakBrightness, 0.0f, 150.0f);
        ReadIniPercent(configPath, L"Front", L"CoreSize", s_FrontCoreSizeScale, 40.0f, 180.0f);
        ReadIniPercent(configPath, L"Front", L"CoreBrightness", s_FrontCoreBrightness, 0.0f, 100.0f);
        ReadIniAngleDegrees(configPath, L"Front", L"AngleFadeStartDegrees", s_FrontAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Front", L"AngleFadeEndDegrees", s_FrontAngleFadeEnd);
        ReadIniAngleDegrees(configPath, L"Front", L"CoreAngleFadeStartDegrees", s_FrontCoreAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Front", L"CoreAngleFadeEndDegrees", s_FrontCoreAngleFadeEnd);
        ReadIniDistance(configPath, L"Front", L"CoreDistanceFadeStart", s_FrontCoreDistanceFadeStart, 5.0f, 30.0f);
        ReadIniDistance(configPath, L"Front", L"CoreDistanceFadeEnd", s_FrontCoreDistanceFadeEnd, 31.0f, 45.0f);

        ReadIniPercent(configPath, L"Rear", L"StreakLength", s_RearStreakLengthScale, 25.0f, 250.0f);
        ReadIniPercent(configPath, L"Rear", L"StreakHeight", s_RearStreakHeightScale, 50.0f, 200.0f);
        ReadIniPercent(configPath, L"Rear", L"StreakBrightness", s_RearStreakBrightness, 0.0f, 150.0f);
        ReadIniPercent(configPath, L"Rear", L"SideCoreSize", s_RearRoundCoreSizeScale, 20.0f, 200.0f);
        ReadIniPercent(configPath, L"Rear", L"SideCoreBrightness", s_RearRoundCoreBrightness, 0.0f, 100.0f);
        ReadIniDistance(configPath, L"Rear", L"DistanceFadeStart", s_RearTailDistanceFadeStart, 2.0f, 20.0f);
        ReadIniDistance(configPath, L"Rear", L"DistanceFadeEnd", s_RearTailDistanceFadeEnd, 21.0f, 35.0f);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundEntryAngleStartDegrees", s_RearRoundAngleEntryStart);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundEntryAngleEndDegrees", s_RearRoundAngleEntryEnd);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundExitAngleStartDegrees", s_RearRoundAngleExitStart);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundExitAngleEndDegrees", s_RearRoundAngleExitEnd);

        ReadIniPercent(configPath, L"Reverse", L"StreakLength", s_ReverseStreakLengthScale, 25.0f, 250.0f);
        ReadIniPercent(configPath, L"Reverse", L"StreakBrightness", s_ReverseStreakBrightness, 0.0f, 150.0f);
        ReadIniPercent(configPath, L"Reverse", L"CoreSize", s_ReverseCoreSizeScale, 40.0f, 160.0f);
        ReadIniPercent(configPath, L"Reverse", L"CoreBrightness", s_ReverseCoreBrightness, 0.0f, 100.0f);
        ReadIniPercent(configPath, L"Reverse", L"BikeStreakSize", s_BikeReverseStreakSizeMultiplier, 10.0f, 100.0f);
        ReadIniPercent(configPath, L"Reverse", L"BikeCoreSize", s_BikeReverseCoreSizeMultiplier, 10.0f, 100.0f);
        ReadIniPercent(configPath, L"Reverse", L"BikeQuadBrightness", s_BikeReverseBrightnessMultiplier, 0.0f, 100.0f);
        ReadIniAngleDegrees(configPath, L"Reverse", L"CoreAngleFadeStartDegrees", s_ReverseCoreAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Reverse", L"CoreAngleFadeEndDegrees", s_ReverseCoreAngleFadeEnd);

        ReadIniNumber(configPath, L"Advanced", L"SideAngleExponent", s_SideAngleExponent);
        s_SideAngleExponent = std::max(2.0f, std::min(8.0f, s_SideAngleExponent));
        ReadIniDistance(configPath, L"Advanced", L"StreakDistanceFadeStart", s_StreakDistanceFadeStart, 5.0f, 38.0f);
        ReadIniDistance(configPath, L"Advanced", L"StreakDistanceFadeEnd", s_StreakDistanceFadeEnd, 39.0f, 50.0f);

        // Fade edges must remain ordered in cosine/dot-product space.
        // If a pair is misconfigured, fall back to its built-in values.
        if (s_FrontAngleFadeStart >= s_FrontAngleFadeEnd)
        {
            s_FrontAngleFadeStart = -0.88f;
            s_FrontAngleFadeEnd = -0.25f;
        }
        if (s_FrontCoreAngleFadeStart >= s_FrontCoreAngleFadeEnd)
        {
            s_FrontCoreAngleFadeStart = -0.995f;
            s_FrontCoreAngleFadeEnd = -0.88f;
        }
        if (s_ReverseCoreAngleFadeStart >= s_ReverseCoreAngleFadeEnd)
        {
            s_ReverseCoreAngleFadeStart = 0.80f;
            s_ReverseCoreAngleFadeEnd = 0.97f;
        }
        if (s_RearRoundAngleEntryStart >= s_RearRoundAngleEntryEnd)
        {
            s_RearRoundAngleEntryStart = -0.20f;
            s_RearRoundAngleEntryEnd = 0.12f;
        }
        if (s_RearRoundAngleExitStart >= s_RearRoundAngleExitEnd)
        {
            s_RearRoundAngleExitStart = 0.28f;
            s_RearRoundAngleExitEnd = 0.78f;
        }
    }

    float Clamp01(float value)
    {
        return std::max(0.0f, std::min(1.0f, value));
    }

    // Quintic easing gives a smooth transition at both ends.
    float SmootherStep(float edge0, float edge1, float value)
    {
        if (edge0 == edge1)
            return value < edge0 ? 0.0f : 1.0f;

        const float t = Clamp01((value - edge0) / (edge1 - edge0));
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    struct StreakSample
    {
        float ScreenX = 0.0f;
        float ScreenY = 0.0f;
        float Depth = 0.0f;
        float ReciprocalDepth = 0.0f;
        unsigned char Red = 255;
        unsigned char Green = 255;
        unsigned char Blue = 255;
        float Intensity = 1.0f;
        // Independent angular visibility used to scale streak length under oblique views.
        float AngleFade = 1.0f;
        bool Valid = false;
    };

    struct StreakHistory
    {
        StreakSample Samples[HEADLIGHT_STREAK_HISTORY];
        unsigned int LastUpdatedFrame = 0xFFFFFFFFu;
        unsigned int LastSeenFrame = 0;
        int SampleCount = 0;
        bool Active = false;
        bool IsRear = false;
        bool IsReverse = false;
        float SizeScale = 1.0f;
    };

    std::unordered_map<unsigned int, StreakHistory> s_StreakHistoryById;

    bool ProjectLightToScreen(const CVector& worldPosition, StreakSample& sample,
                             const HeadlightColour& colour, float intensity,
                             float angleFade)
    {
        const RwV3d position = { worldPosition.x, worldPosition.y, worldPosition.z };
        RwV3d screenPosition{};
        float projectedWidth = 0.0f;
        float projectedHeight = 0.0f;

        if (!CSprite::CalcScreenCoors(position, &screenPosition,
                                      &projectedWidth, &projectedHeight, false, false))
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
        if (screenPosition.z <= 0.5f || screenPosition.z >= s_StreakDistanceFadeEnd)
            return false;

        sample.ScreenX = screenPosition.x;
        sample.ScreenY = screenPosition.y;
        sample.Depth = screenPosition.z;
        sample.ReciprocalDepth = 1.0f / screenPosition.z;
        // Keep colour separate from intensity. Applying intensity to both RGB
        // and alpha would square the fade and make the transition look abrupt.
        sample.Red = colour.Red;
        sample.Green = colour.Green;
        sample.Blue = colour.Blue;
        const float distanceFade = 1.0f - SmootherStep(
            s_StreakDistanceFadeStart, s_StreakDistanceFadeEnd, screenPosition.z);
        sample.Intensity = Clamp01(intensity) * distanceFade;
        sample.AngleFade = Clamp01(angleFade);
        sample.Valid = sample.ReciprocalDepth > 0.0f && sample.Intensity > 0.001f;
        return sample.Valid;
    }

    void UpdateStreakHistory(unsigned int coronaId, const CVector& worldPosition,
                             const HeadlightColour& colour, float intensity,
                             bool isRear, float angleFade,
                             float sizeScale, bool isReverse)
    {
        if (!s_CustomStreakEnabled)
            return;

        StreakHistory& history = s_StreakHistoryById[coronaId];
        history.IsRear = isRear;
        history.IsReverse = isReverse;
        history.SizeScale = std::max(0.1f, std::min(1.0f, sizeScale));
        const unsigned int frame = CTimer::m_FrameCounter;
        StreakSample current;

        history.LastSeenFrame = frame;
        history.Active = true;

        if (!ProjectLightToScreen(worldPosition, current, colour, intensity, angleFade))
        {
            history.Active = false;
            history.SampleCount = 0;
            return;
        }

        if (history.LastUpdatedFrame != frame)
        {
            for (int i = HEADLIGHT_STREAK_HISTORY - 1; i > 0; --i)
                history.Samples[i] = history.Samples[i - 1];

            if (history.SampleCount < HEADLIGHT_STREAK_HISTORY)
                ++history.SampleCount;

            history.LastUpdatedFrame = frame;
        }

        history.Samples[0] = current;
    }

    void ClearStreakHistory(unsigned int coronaId)
    {
        auto it = s_StreakHistoryById.find(coronaId);
        if (it != s_StreakHistoryById.end())
        {
            it->second.Active = false;
            it->second.SampleCount = 0;
        }
    }

    // Draw immediately instead of relying on the shared sprite buffer. The
    // stock streak raster is still used, but every history sample is emitted as
    // a visible screen-space flare. This mirrors re3's per-corona history while
    // avoiding buffer state/order problems in a Plugin-SDK drawing callback.
    void RenderStreakSprite(const StreakSample& sample, float halfWidth,
                            float halfHeight, unsigned char red,
                            unsigned char green, unsigned char blue,
                            short intensity)
    {
        if (!sample.Valid || sample.ReciprocalDepth <= 0.0f || intensity <= 0)
            return;

        // SA's sprite implementation scales RGB by the short intensity value;
        // the byte after rhw is vertex alpha. Keep vertex alpha fully enabled
        // for additive blending, fade RGB through intensity, and use default UVs.
        CSprite::RenderOneXLUSprite(
            sample.ScreenX, sample.ScreenY, sample.Depth,
            halfWidth, halfHeight,
            red, green, blue,
            intensity, sample.ReciprocalDepth,
            255, 0, 0
        );
    }

    void RenderStreaks()
    {
        const unsigned int frame = CTimer::m_FrameCounter;
        if (!s_CustomStreakEnabled)
        {
            s_StreakHistoryById.clear();
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
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(rwBLENDSRCALPHA)));
        RwRenderStateSet(rwRENDERSTATEDESTBLEND,
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(rwBLENDONE)));
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE,
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(TRUE)));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(FALSE)));
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE,
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(TRUE)));

        for (auto it = s_StreakHistoryById.begin(); it != s_StreakHistoryById.end(); )
        {
            StreakHistory& history = it->second;
            if (frame - history.LastSeenFrame > 1u)
            {
                it = s_StreakHistoryById.erase(it);
                continue;
            }

            if (!history.Active || history.SampleCount < 1 || !history.Samples[0].Valid)
            {
                ++it;
                continue;
            }

            const StreakSample& current = history.Samples[0];
            const float currentIntensity = Clamp01(current.Intensity);
            if (currentIntensity <= 0.001f)
            {
                ++it;
                continue;
            }

            // Draw one main sprite; repeated full-size samples would over-brighten
            // a stationary lamp under additive blending.
            const float baseHalfWidth = history.IsRear
                ? REAR_STREAK_BASE_HALF_WIDTH : HEADLIGHT_STREAK_BASE_HALF_WIDTH;
            const float maxHalfWidth = history.IsRear
                ? REAR_STREAK_MAX_HALF_WIDTH : HEADLIGHT_STREAK_MAX_HALF_WIDTH;
            const float streakLengthScale = !history.IsRear
                ? s_FrontStreakLengthScale
                : (history.IsReverse ? s_ReverseStreakLengthScale : s_RearStreakLengthScale);
            const float streakHeightScale = history.IsRear
                ? s_RearStreakHeightScale : s_FrontStreakHeightScale;
            const float halfHeight = (history.IsRear
                ? REAR_STREAK_HALF_HEIGHT : HEADLIGHT_STREAK_HALF_HEIGHT) *
                history.SizeScale * streakHeightScale;
            const float rawHalfWidth = (baseHalfWidth +
                (maxHalfWidth - baseHalfWidth) * currentIntensity) *
                history.SizeScale * streakLengthScale;
            // Angle controls streak length independently of brightness. A quartic
            // taper removes the minimum-width floor near a broadside view.
            const float angleFacing = Clamp01(current.AngleFade);
            const float angleWidthScale = std::pow(angleFacing, s_SideAngleExponent);
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
            const float baseAlpha = history.IsRear
                ? REAR_STREAK_BASE_ALPHA : HEADLIGHT_STREAK_BASE_ALPHA;
            const float streakBrightness = !history.IsRear
                ? s_FrontStreakBrightness
                : (history.IsReverse ? s_ReverseStreakBrightness : s_RearStreakBrightness);
            const short mainAlpha = static_cast<short>(std::min(
                255.0f, baseAlpha * currentIntensity * streakBrightness));
            RenderStreakSprite(current, mainHalfWidth, mainHalfHeight,
                               current.Red, current.Green, current.Blue, mainAlpha);

            // Add faint history echoes only while the lamp moves on-screen.
            int oldestValidIndex = -1;
            for (int i = history.SampleCount - 1; i >= 1; --i)
            {
                if (history.Samples[i].Valid)
                {
                    oldestValidIndex = i;
                    break;
                }
            }

            if (oldestValidIndex >= 1)
            {
                const StreakSample& oldest = history.Samples[oldestValidIndex];
                const float dx = current.ScreenX - oldest.ScreenX;
                const float dy = current.ScreenY - oldest.ScreenY;
                const float motionSquared = dx * dx + dy * dy;

                if (motionSquared > 9.0f && currentIntensity > 0.12f &&
                    current.AngleFade > 0.18f && widthFade > 0.08f)
                {
                    const int echoCount = std::min(4, oldestValidIndex);
                    for (int echo = 1; echo <= echoCount; ++echo)
                    {
                        const float t = static_cast<float>(echo) / (echoCount + 1.0f);
                        StreakSample interpolated = current;
                        interpolated.ScreenX = current.ScreenX + (oldest.ScreenX - current.ScreenX) * t;
                        interpolated.ScreenY = current.ScreenY + (oldest.ScreenY - current.ScreenY) * t;
                        interpolated.Depth = current.Depth + (oldest.Depth - current.Depth) * t;
                        interpolated.ReciprocalDepth = current.ReciprocalDepth +
                            (oldest.ReciprocalDepth - current.ReciprocalDepth) * t;
                        const float echoBaseAlpha = history.IsRear ? 20.0f : 24.0f;
                        const short echoAlpha = static_cast<short>(std::min(
                            255.0f, echoBaseAlpha * currentIntensity * streakBrightness * (1.0f - t)));
                        RenderStreakSprite(interpolated,
                            std::max(1.0f, mainHalfWidth * 0.18f),
                            (history.IsRear ? 3.6f : 4.4f) * angleWidthScale * angleHeightScale * history.SizeScale,
                            interpolated.Red, interpolated.Green, interpolated.Blue,
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
        return position.x != 0.0f || position.y != 0.0f || position.z != 0.0f;
    }

    void GetVehicleLightPositions(CVehicle* vehicle, CVehicleModelInfo* modelInfo,
                                  int dummyIndex, CVector& localLightPosition,
                                  CVector& leftLightPosition, CVector& rightLightPosition)
    {
        if (modelInfo && modelInfo->m_pVehicleStruct)
        {
            const CVector dummyPos = modelInfo->m_pVehicleStruct->m_avDummyPos[dummyIndex];
            if (IsPositionSet(dummyPos))
                localLightPosition = dummyPos;
        }

        const CVector mirroredLightPosition = MirrorHeadlightPosition(localLightPosition);
        leftLightPosition = TransformLocalPosition(vehicle, localLightPosition);
        rightLightPosition = TransformLocalPosition(vehicle, mirroredLightPosition);
    }

    float GetCameraBehindness(const CVehicle* vehicle, float& cameraDistance)
    {
        const CVector& cameraPos = TheCamera.m_mCameraMatrix.pos;
        const CVector& vehiclePos = vehicle->m_matrix->pos;

        CVector lookVector(
            vehiclePos.x - cameraPos.x,
            vehiclePos.y - cameraPos.y,
            vehiclePos.z - cameraPos.z
        );

        cameraDistance = std::sqrt(
            lookVector.x * lookVector.x +
            lookVector.y * lookVector.y +
            lookVector.z * lookVector.z
        );

        if (cameraDistance > 0.0001f)
        {
            const float inverseDistance = 1.0f / cameraDistance;
            lookVector.x *= inverseDistance;
            lookVector.y *= inverseDistance;
            lookVector.z *= inverseDistance;
        }
        else
        {
            lookVector = CVector(1.0f, 0.0f, 0.0f);
        }

        const CVector& forward = vehicle->m_matrix->up;
        float cameraBehindness = lookVector.x * forward.x +
                           lookVector.y * forward.y +
                           lookVector.z * forward.z;

        // Clamp floating-point drift before calculating the camera angle.
        if (cameraBehindness < -1.0f)
            cameraBehindness = -1.0f;
        else if (cameraBehindness > 1.0f)
            cameraBehindness = 1.0f;

        return cameraBehindness;
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
                             float coronaAngle)
    {
        CCoronas::RegisterCorona(
            coronaId, nullptr,
            colour.Red, colour.Green, colour.Blue, alpha,
            position, size, farClip, coronaType, FLARETYPE_NONE,
            true, false, 0, coronaAngle, false,
            HEADLIGHT_NEAR_CLIP, 0, HEADLIGHT_FADE_SPEED, false, false
        );
    }

    void RegisterHeadlightCore(unsigned int coronaId, const CVector& position,
                               const HeadlightColour& colour, float size, float coronaAngle,
                               float fade)
    {
        const float scaledAlpha = 255.0f * Clamp01(fade) * Clamp01(s_FrontCoreBrightness);
        const unsigned char alpha = static_cast<unsigned char>(scaledAlpha);
        RegisterLightCorona(coronaId, position, colour, alpha,
                            size * s_FrontCoreSizeScale,
                            HEADLIGHT_CORE_FAR_CLIP, CORONATYPE_HEADLIGHT, coronaAngle);
    }

    void HideHeadlightCore(unsigned int coronaId, const CVector& worldPosition, float coronaAngle)
    {
        // Re-register the same ID with zero alpha so the previous corona is
        // replaced immediately instead of lingering as a fading old instance.
        RegisterHeadlightCore(coronaId, worldPosition, { 0, 0, 0 }, 1.2f, coronaAngle, 0);
    }

    void RegisterReverseLightCore(unsigned int coronaId, const CVector& position,
                                  float coronaAngle, float fade, float sizeScale,
                                  float brightnessMultiplier)
    {
        const float scaledAlpha = std::min(
            255.0f, 190.0f * Clamp01(fade) *
                std::max(0.0f, s_ReverseCoreBrightness) * Clamp01(brightnessMultiplier));
        RegisterLightCorona(coronaId, position, { 200, 215, 235 },
                            static_cast<unsigned char>(scaledAlpha),
                            0.9f * REVERSE_LIGHT_CORE_SIZE_MULTIPLIER *
                                sizeScale * s_ReverseCoreSizeScale,
                            REVERSE_LIGHT_FAR_CLIP, CORONATYPE_HEADLIGHT, coronaAngle);
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

    void ProcessReverseLights(bool isReversing, bool isBike,
                              float reverseBrightnessMultiplier,
                              float cameraBehindness, float coreDistanceFade,
                              unsigned int baseCoronaId, float coronaAngle,
                              const CVector& leftLightPosition, const CVector& rightLightPosition)
    {
        // Reverse cores use a narrow angle and distance envelope.
        const float coreAngleFade = SmootherStep(
            s_ReverseCoreAngleFadeStart, s_ReverseCoreAngleFadeEnd, cameraBehindness);
        const float coreFade = isReversing ? coreAngleFade * coreDistanceFade : 0.0f;

        const float coreSizeScale = isBike
            ? s_BikeReverseCoreSizeMultiplier : 1.0f;
        RegisterReverseLightCore(baseCoronaId + 0x79u, leftLightPosition, coronaAngle, coreFade, coreSizeScale,
                                reverseBrightnessMultiplier);
        RegisterReverseLightCore(baseCoronaId + 0x7Au, rightLightPosition, coronaAngle, coreFade, coreSizeScale,
                                reverseBrightnessMultiplier);
    }

    void RegisterRearLampCore(unsigned int coronaId, const CVector& position,
                              const HeadlightColour& colour, float coronaAngle, float fade,
                              float sizeScale, bool isReverse,
                              float reverseBrightnessMultiplier)
    {
        // The round side-view replacement is intentionally smaller than the
        // main rear-view streak, so it reads as a compact lamp instead of a halo.
        const float maxAlpha = isReverse ? 78.0f : 95.0f;
        const float brightness = isReverse
            ? s_ReverseCoreBrightness * Clamp01(reverseBrightnessMultiplier)
            : s_RearRoundCoreBrightness;
        const float coreSizeScale = isReverse
            ? s_ReverseCoreSizeScale : s_RearRoundCoreSizeScale;
        const float scaledAlpha = std::min(
            255.0f, maxAlpha * Clamp01(fade) * std::max(0.0f, brightness));
        RegisterLightCorona(coronaId, position, colour,
                            static_cast<unsigned char>(scaledAlpha),
                            0.30f * sizeScale * coreSizeScale,
                            HEADLIGHT_CORE_FAR_CLIP, CORONATYPE_HEADLIGHT, coronaAngle);
    }

    void ProcessRearLights(CVehicle* vehicle, bool isReversing, bool isBike,
                          float reverseBrightnessMultiplier, float cameraBehindness,
                          float cameraDistance, unsigned int baseCoronaId, float coronaAngle,
                          const CVector& leftLightPosition, const CVector& rightLightPosition)
    {
        const unsigned int leftStreakId = baseCoronaId + 0x7Du;
        const unsigned int rightStreakId = baseCoronaId + 0x7Eu;
        const float rearVisibilityFade = SmootherStep(
            -s_FrontAngleFadeEnd, -s_FrontAngleFadeStart, cameraBehindness);
        // Fade and shorten the streak as the camera moves behind the vehicle;
        // the compact side-view corona prevents the line crossing the body.
        const float rearVisibility = rearVisibilityFade * rearVisibilityFade;

        const bool rearLightsEnabled = vehicle->bLightsOn || isReversing;
        const float normalIntensity = Clamp01(0.38f + 0.30f * cameraBehindness);

        // The streak and side-view corona share the same colour transition.
        const HeadlightColour streakColour = isReversing
            ? HeadlightColour{ 205, 220, 250 }
            : HeadlightColour{ 255, 24, 12 };
        const float streakBrightness = isReversing
            ? 0.60f * Clamp01(reverseBrightnessMultiplier) : normalIntensity;
        // Fade only the normal red taillight streak in with distance. Near the
        // camera the stock red tail lamps remain, but the custom long red line is
        // hidden; it then grows smoothly instead of popping in at one distance.
        const float redTailDistanceFade = SmootherStep(
            s_RearTailDistanceFadeStart, s_RearTailDistanceFadeEnd,
            cameraDistance);
        const float distanceFade = isReversing ? 1.0f : redTailDistanceFade;
        const float intensity = streakBrightness * rearVisibility * distanceFade;

        // Cross-fade the compact side-view corona into the rear streak.
        const float roundEntryFade = SmootherStep(
            s_RearRoundAngleEntryStart, s_RearRoundAngleEntryEnd, cameraBehindness);
        const float roundExitFade = 1.0f - SmootherStep(
            s_RearRoundAngleExitStart, s_RearRoundAngleExitEnd, cameraBehindness);
        const float roundFade = rearLightsEnabled
            ? roundEntryFade * roundExitFade
            : 0.0f;
        const float reverseLampSizeScale = isReversing && isBike
            ? s_BikeReverseCoreSizeMultiplier : 1.0f;
        RegisterRearLampCore(baseCoronaId + 0x81u, leftLightPosition, streakColour, coronaAngle,
                             roundFade, reverseLampSizeScale, isReversing,
                             reverseBrightnessMultiplier);
        RegisterRearLampCore(baseCoronaId + 0x82u, rightLightPosition, streakColour, coronaAngle,
                             roundFade, reverseLampSizeScale, isReversing,
                             reverseBrightnessMultiplier);

        // Do not register another long native corona for ordinary tail lamps.
        // The stock lamps remain intact; the custom layer draws the elongated line
        // from the rear view and the small circular replacement near side angles.
        if (!rearLightsEnabled || rearVisibility <= 0.001f)
        {
            ClearStreakHistory(leftStreakId);
            ClearStreakHistory(rightStreakId);
            return;
        }

        const float streakSizeScale = isReversing && isBike
            ? s_BikeReverseStreakSizeMultiplier : 1.0f;
        UpdateStreakHistory(leftStreakId, leftLightPosition, streakColour, intensity,
                              true, rearVisibilityFade, streakSizeScale, isReversing);
        UpdateStreakHistory(rightStreakId, rightLightPosition, streakColour, intensity,
                              true, rearVisibilityFade, streakSizeScale, isReversing);
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

        CVehicleModelInfo* modelInfo = static_cast<CVehicleModelInfo*>(
            CModelInfo::GetModelInfo(vehicle->m_nModelIndex));

        bool leftLightWorking = true;
        bool rightLightWorking = true;
        GetHeadlightWorkingState(vehicle, modelInfo, leftLightWorking, rightLightWorking);

        CVector localLightPosition(DEFAULT_HEADLIGHT_X, DEFAULT_HEADLIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector leftLightPosition, rightLightPosition;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_HEADLIGHTS,
                                 localLightPosition, leftLightPosition, rightLightPosition);

        float cameraDistance = 0.0f;
        const float cameraBehindness = GetCameraBehindness(vehicle, cameraDistance);
        const float coreDistanceFade = 1.0f - SmootherStep(
            s_FrontCoreDistanceFadeStart, s_FrontCoreDistanceFadeEnd, cameraDistance);
        const unsigned int baseCoronaId = static_cast<unsigned int>(
            reinterpret_cast<std::uintptr_t>(vehicle));
        const float coronaAngle = std::acos(cameraBehindness);
        const HeadlightColour colour = GetHeadlightColour(vehicle);
        const bool isReversing = IsVehicleReversing(vehicle);
        const bool isBike = modelInfo && modelInfo->m_nVehicleType == VEHICLE_BIKE;
        const bool isQuadBike = vehicle->m_nModelIndex == QUADBIKE_MODEL_ID;
        const float reverseBrightnessMultiplier = (isBike || isQuadBike)
            ? Clamp01(s_BikeReverseBrightnessMultiplier) : 1.0f;

        CVector localRearLightPosition(-DEFAULT_HEADLIGHT_X, DEFAULT_REAR_LIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector rearLeftLightPosition, rearRightLightPosition;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_REAR_LIGHTS,
                                 localRearLightPosition, rearLeftLightPosition, rearRightLightPosition);

        if (vehicle->bLightsOn)
        {
            // Fade front streaks smoothly as the camera moves toward the side.
            const float frontVisibilityFade = 1.0f - SmootherStep(
                s_FrontAngleFadeStart, s_FrontAngleFadeEnd, cameraBehindness);
            // The extra fade limits screen-space lines crossing the body.
            const float frontVisibility = frontVisibilityFade * frontVisibilityFade;
            const float rawIntensity = -0.5f * cameraBehindness + 0.3f;
            const float baseIntensity = Clamp01(rawIntensity);
            const float intensity = baseIntensity * frontVisibility;
            // Match damage state to the dummy side; models can store either side first.
            const bool firstLampIsVehicleLeft = localLightPosition.x < 0.0f;
            const bool firstLightWorking = firstLampIsVehicleLeft
                ? leftLightWorking : rightLightWorking;
            const bool secondLightWorking = firstLampIsVehicleLeft
                ? rightLightWorking : leftLightWorking;

            // Keep the two physical lamps independent: a damaged lamp clears only
            // its own streak and core, not the one on the opposite side.
            if (firstLightWorking)
                UpdateStreakHistory(baseCoronaId + 0x71u, leftLightPosition, colour, intensity,
                                    false, frontVisibilityFade, 1.0f, false);
            else
                ClearStreakHistory(baseCoronaId + 0x71u);

            if (secondLightWorking)
                UpdateStreakHistory(baseCoronaId + 0x72u, rightLightPosition, colour, intensity,
                                    false, frontVisibilityFade, 1.0f, false);
            else
                ClearStreakHistory(baseCoronaId + 0x72u);

            // Keep the round core's angle and distance fades narrower than the streak.
            const float coreAngleFade = 1.0f - SmootherStep(
                s_FrontCoreAngleFadeStart, s_FrontCoreAngleFadeEnd, cameraBehindness);
            const float coreFade = coreAngleFade * coreDistanceFade;

            if (firstLightWorking && coreFade > 0.001f)
                RegisterHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, colour, 1.2f, coronaAngle, coreFade);
            else
                HideHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, coronaAngle);

            if (secondLightWorking && coreFade > 0.001f)
                RegisterHeadlightCore(baseCoronaId + 0x74u, rightLightPosition, colour, 1.2f, coronaAngle, coreFade);
            else
                HideHeadlightCore(baseCoronaId + 0x74u, rightLightPosition, coronaAngle);
        }
        else
        {
            ClearStreakHistory(baseCoronaId + 0x71u);
            ClearStreakHistory(baseCoronaId + 0x72u);
            HideHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, coronaAngle);
            HideHeadlightCore(baseCoronaId + 0x74u, rightLightPosition, coronaAngle);
        }

        // Keep stock rear lamps and add the custom screen-space layer.
        ProcessRearLights(vehicle, isReversing, isBike, reverseBrightnessMultiplier,
                          cameraBehindness, cameraDistance, baseCoronaId, coronaAngle,
                          rearLeftLightPosition, rearRightLightPosition);

        // Reverse lamps are independent of the headlight switch.
        ProcessReverseLights(isReversing, isBike, reverseBrightnessMultiplier, cameraBehindness,
                             coreDistanceFade, baseCoronaId, coronaAngle,
                             rearLeftLightPosition, rearRightLightPosition);
    }

    void ResetStreakDebugSettings()
    {
        s_CustomStreakEnabled = 1;
        s_FrontStreakLengthScale = 1.0f;
        s_FrontStreakHeightScale = 1.0f;
        s_FrontStreakBrightness = 1.0f;
        s_FrontCoreSizeScale = 1.0f;
        s_FrontCoreBrightness = 0.65f;
        s_RearStreakLengthScale = 1.0f;
        s_RearStreakHeightScale = 1.0f;
        s_RearStreakBrightness = 1.0f;
        s_RearRoundCoreSizeScale = 1.0f;
        s_RearRoundCoreBrightness = 1.0f;
        s_ReverseStreakLengthScale = 1.0f;
        s_ReverseStreakBrightness = 1.0f;
        s_ReverseCoreSizeScale = 1.0f;
        s_ReverseCoreBrightness = 1.0f;
        s_BikeReverseStreakSizeMultiplier = 0.55f;
        s_BikeReverseCoreSizeMultiplier = 0.60f;
        s_BikeReverseBrightnessMultiplier = 0.35f;
        s_SideAngleExponent = 4.0f;
        s_FrontAngleFadeStart = -0.88f;
        s_FrontAngleFadeEnd = -0.25f;
        s_FrontCoreAngleFadeStart = -0.995f;
        s_FrontCoreAngleFadeEnd = -0.88f;
        s_FrontCoreDistanceFadeStart = 23.0f;
        s_FrontCoreDistanceFadeEnd = 32.0f;
        s_ReverseCoreAngleFadeStart = 0.80f;
        s_ReverseCoreAngleFadeEnd = 0.97f;
        s_RearTailDistanceFadeStart = 9.0f;
        s_RearTailDistanceFadeEnd = 23.0f;
        s_StreakDistanceFadeStart = 25.0f;
        s_StreakDistanceFadeEnd = 40.0f;
        s_RearRoundAngleEntryStart = -0.20f;
        s_RearRoundAngleEntryEnd = 0.12f;
        s_RearRoundAngleExitStart = 0.28f;
        s_RearRoundAngleExitEnd = 0.78f;

        // Treat the INI as user defaults; reset/reload restores them after the Debug Menu edits.
        LoadPluginConfig();
    }

    void RegisterStreakDebugMenu()
    {
        // Load config independently of whether the optional Debug Menu API is available.
        LoadPluginConfig();
        if (s_DebugMenuRegistered || !DebugMenuLoad())
            return;

        // The official API loader marks itself loaded even if an export is missing;
        // verify every function used below before registering any entries.
        if (!gDebugMenuAPI.addint32 || !gDebugMenuAPI.addfloat32 ||
            !gDebugMenuAPI.addcmd || !gDebugMenuAPI.setwrap)
            return;

        static const char* toggleStrings[] = { "Off", "On" };
        DebugMenuEntry* toggle = DebugMenuAddVar(
            "SA Streaks|General", "Custom streak lines",
            &s_CustomStreakEnabled, nullptr, 1, 0, 1, toggleStrings);
        if (toggle)
            DebugMenuEntrySetWrap(toggle, true);
        DebugMenuAddCmd("SA Streaks|General", "Reload INI / reset values", ResetStreakDebugSettings);

        DebugMenuAddVar("SA Streaks|Front", "Streak length", &s_FrontStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Front", "Streak height", &s_FrontStreakHeightScale,
                        nullptr, 0.05f, 0.50f, 2.00f);
        DebugMenuAddVar("SA Streaks|Front", "Streak brightness", &s_FrontStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Front", "Round core size", &s_FrontCoreSizeScale,
                        nullptr, 0.05f, 0.40f, 1.80f);
        DebugMenuAddVar("SA Streaks|Front", "Round core brightness", &s_FrontCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Front", "Angle fade start", &s_FrontAngleFadeStart,
                        nullptr, 0.02f, -0.99f, -0.30f);
        DebugMenuAddVar("SA Streaks|Front", "Angle fade end", &s_FrontAngleFadeEnd,
                        nullptr, 0.02f, -0.25f, 0.50f);
        DebugMenuAddVar("SA Streaks|Front", "Core angle start", &s_FrontCoreAngleFadeStart,
                        nullptr, 0.005f, -1.0f, -0.905f);
        DebugMenuAddVar("SA Streaks|Front", "Core angle end", &s_FrontCoreAngleFadeEnd,
                        nullptr, 0.005f, -0.90f, -0.50f);
        DebugMenuAddVar("SA Streaks|Front", "Core distance start", &s_FrontCoreDistanceFadeStart,
                        nullptr, 1.0f, 5.0f, 30.0f);
        DebugMenuAddVar("SA Streaks|Front", "Core distance end", &s_FrontCoreDistanceFadeEnd,
                        nullptr, 1.0f, 31.0f, 45.0f);

        DebugMenuAddVar("SA Streaks|Rear", "Streak length", &s_RearStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Rear", "Streak height", &s_RearStreakHeightScale,
                        nullptr, 0.05f, 0.50f, 2.00f);
        DebugMenuAddVar("SA Streaks|Rear", "Streak brightness", &s_RearStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Rear", "Side core size", &s_RearRoundCoreSizeScale,
                        nullptr, 0.05f, 0.20f, 2.00f);
        DebugMenuAddVar("SA Streaks|Rear", "Side core brightness", &s_RearRoundCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Distance fade start", &s_RearTailDistanceFadeStart,
                        nullptr, 1.0f, 2.0f, 20.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Distance fade end", &s_RearTailDistanceFadeEnd,
                        nullptr, 1.0f, 21.0f, 35.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry start", &s_RearRoundAngleEntryStart,
                        nullptr, 0.02f, -0.50f, 0.10f);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry end", &s_RearRoundAngleEntryEnd,
                        nullptr, 0.01f, 0.11f, 0.19f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit start", &s_RearRoundAngleExitStart,
                        nullptr, 0.02f, 0.20f, 0.70f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit end", &s_RearRoundAngleExitEnd,
                        nullptr, 0.02f, 0.71f, 0.99f);

        DebugMenuAddVar("SA Streaks|Reverse", "Streak length", &s_ReverseStreakLengthScale,
                        nullptr, 0.05f, 0.25f, 2.50f);
        DebugMenuAddVar("SA Streaks|Reverse", "Streak brightness", &s_ReverseStreakBrightness,
                        nullptr, 0.05f, 0.0f, 1.50f);
        DebugMenuAddVar("SA Streaks|Reverse", "Round core size", &s_ReverseCoreSizeScale,
                        nullptr, 0.05f, 0.40f, 1.60f);
        DebugMenuAddVar("SA Streaks|Reverse", "Round core brightness", &s_ReverseCoreBrightness,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Reverse", "Bike streak size", &s_BikeReverseStreakSizeMultiplier,
                        nullptr, 0.05f, 0.10f, 1.00f);
        DebugMenuAddVar("SA Streaks|Reverse", "Bike core size", &s_BikeReverseCoreSizeMultiplier,
                        nullptr, 0.05f, 0.10f, 1.00f);
        DebugMenuAddVar("SA Streaks|Reverse", "Bike/quad brightness", &s_BikeReverseBrightnessMultiplier,
                        nullptr, 0.05f, 0.0f, 1.0f);
        DebugMenuAddVar("SA Streaks|Reverse", "Core angle start", &s_ReverseCoreAngleFadeStart,
                        nullptr, 0.01f, 0.50f, 0.95f);
        DebugMenuAddVar("SA Streaks|Reverse", "Core angle end", &s_ReverseCoreAngleFadeEnd,
                        nullptr, 0.005f, 0.96f, 1.00f);

        DebugMenuAddVar("SA Streaks|Advanced", "Side angle exponent", &s_SideAngleExponent,
                        nullptr, 0.25f, 2.0f, 8.0f);
        DebugMenuAddVar("SA Streaks|Advanced", "Streak distance start", &s_StreakDistanceFadeStart,
                        nullptr, 1.0f, 5.0f, 38.0f);
        DebugMenuAddVar("SA Streaks|Advanced", "Streak distance end", &s_StreakDistanceFadeEnd,
                        nullptr, 1.0f, 39.0f, 50.0f);

        s_DebugMenuRegistered = true;
    }

    struct Bootstrap
    {
        Bootstrap()
        {
            // There are at most a few streak IDs per rendered vehicle. Reserve
            // ahead of time to avoid hash-table reallocation during render events.
            s_StreakHistoryById.reserve(512);
            plugin::Events::initGameEvent += RegisterStreakDebugMenu;
            plugin::Events::vehicleRenderEvent += ProcessVehicleLights;
            plugin::Events::drawingEvent += RenderStreaks;
        }
    } s_PluginBootstrap;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        s_PluginModuleHandle = module;

    return TRUE;
}
