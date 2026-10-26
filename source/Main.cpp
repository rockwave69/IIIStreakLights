#include "plugin.h"
#include "CCamera.h"
#include "CCoronas.h"
#include "CSprite.h"
#include "CTimer.h"
#include "CModelInfo.h"
#include "CPools.h"
#include "CVehicle.h"
#include "CPed.h"
#include "CAutomobile.h"
#include "CBike.h"
#include "CVehicleModelInfo.h"
#include "CVector.h"
#include "CShadows.h"
#include "CPointLights.h"
#include "common.h"
#include "debugmenu_public.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

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
    constexpr int TRACTOR_MODEL_ID = 531; // Tractor uses a single centered rear projection.
    constexpr int HARVESTER_MODEL_ID = 532; // Combine Harvester has no physical rear lamps.
    // One projection is used per vehicle. Paired lamps use a centered full-size
    // footprint; single-lamp vehicles use a smaller, compact footprint.
    constexpr float PAIRED_REAR_PROJECTION_SCALE = 1.0f;
    constexpr float SINGLE_REAR_PROJECTION_SCALE = 0.90f;
    // Pedal bicycles in the original GTA San Andreas vehicle model list.
    constexpr int BMX_MODEL_ID = 481;
    constexpr int BIKE_MODEL_ID = 509;
    constexpr int MOUNTAIN_BIKE_MODEL_ID = 510;
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
        std::uint8_t m_nRed;
        std::uint8_t m_nGreen;
        std::uint8_t m_nBlue;
    };

    // SA does not expose re3's STREAK_ON flag, so keep a short screen-space history.
    constexpr int HEADLIGHT_STREAK_HISTORY = 6;
    constexpr float HEADLIGHT_STREAK_HALF_HEIGHT = 7.0f;
    constexpr float REAR_STREAK_HALF_HEIGHT = 7.0f;
    constexpr float HEADLIGHT_STREAK_BASE_HALF_WIDTH = 220.0f;
    constexpr float HEADLIGHT_STREAK_MAX_HALF_WIDTH = 440.0f;
    constexpr float HEADLIGHT_STREAK_BASE_ALPHA = 74.0f;
    constexpr float REAR_STREAK_BASE_HALF_WIDTH = 125.0f;
    constexpr float REAR_STREAK_MAX_HALF_WIDTH = 205.0f;
    constexpr float REAR_STREAK_BASE_ALPHA = 68.0f;
    // Keep a short streak at oblique angles, then grow it smoothly as the camera
    // turns toward the lamps. The angle is measured in the horizontal plane.
    constexpr float HEADLIGHT_STREAK_SIDE_FADE_END = 0.55f;
    constexpr float STREAK_MIN_SIDE_WIDTH_SCALE = 0.14f;

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
    // White reverse-light effects are enabled by default for standard automobiles only.
    // The INI and Debug Menu can disable them; other vehicle types never use white reverse effects.
    std::int32_t s_WhiteReverseLightsEnabled = 1;

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
    // Rear ground projection style: 0 = off, 1 = Vice City, 2 = LCS-style (default).
    // Projection style is independent of the rear streak distance-visibility mode below.
    std::int32_t s_RearProjectionStyle = 2;
    float s_RearProjectionDrawDistance = 60.0f;
    // Projection offsets are measured backward from the rear-lamp anchor.
    // Single-lamp vehicles retain a separately configurable offset.
    float s_CarRearProjectionOffset = 0.20f;
    float s_SingleRearProjectionOffset = 0.20f;
    std::vector<int> s_SingleRearProjectionModelIds;
    // 0 = fade out nearby (distance-only look, default); 1 = keep rear streaks visible nearby (VC-like).
    std::int32_t s_RearStreakVisibilityStyle = 0;
    float s_StreakDistanceFadeStart = 25.0f;
    // Also acts as the hard culling range; default preserves the existing 40-unit limit.
    float s_StreakDrawDistance = 40.0f;
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

        wchar_t* end = nullptr;
        const float number = std::wcstof(text, &end);
        if (end == text || !std::isfinite(number))
            return false;

        while (*end && std::iswspace(*end))
            ++end;
        if (*end != L'\0')
            return false;

        result = number != 0.0f;
        return true;
    }

    bool ReadIniModelIdList(const std::wstring& configPath, const wchar_t* section,
                            const wchar_t* key, std::vector<int>& modelIds)
    {
        wchar_t text[2048] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", text,
            static_cast<DWORD>(sizeof(text) / sizeof(text[0])), configPath.c_str());
        if (length == 0)
            return false;

        std::vector<int> parsedIds;
        const wchar_t* cursor = text;
        while (*cursor)
        {
            while (*cursor && (std::iswspace(*cursor) || *cursor == L',' ||
                               *cursor == L';' || *cursor == L'|'))
                ++cursor;
            if (!*cursor)
                break;

            wchar_t* end = nullptr;
            const long value = std::wcstol(cursor, &end, 10);
            if (end == cursor)
            {
                while (*cursor && *cursor != L',' && *cursor != L';' &&
                       *cursor != L'|' && !std::iswspace(*cursor))
                    ++cursor;
                continue;
            }

            if (value > 0 && value <= static_cast<long>(std::numeric_limits<int>::max()))
            {
                const int modelId = static_cast<int>(value);
                if (std::find(parsedIds.begin(), parsedIds.end(), modelId) == parsedIds.end())
                    parsedIds.push_back(modelId);
            }
            cursor = end;
        }

        modelIds.swap(parsedIds);
        return true;
    }

    void ReadIniScale(const std::wstring& configPath, const wchar_t* section,
                        const wchar_t* key, float& setting,
                        float minimumValue, float maximumValue)
    {
        float scaleValue = 0.0f;
        if (ReadIniNumber(configPath, section, key, scaleValue))
        {
            scaleValue = std::max(minimumValue, std::min(maximumValue, scaleValue));
            setting = scaleValue / 100.0f;
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

        ReadIniScale(configPath, L"Front", L"StreakLength", s_FrontStreakLengthScale, 25.0f, 250.0f);
        ReadIniScale(configPath, L"Front", L"StreakHeight", s_FrontStreakHeightScale, 50.0f, 200.0f);
        ReadIniScale(configPath, L"Front", L"StreakBrightness", s_FrontStreakBrightness, 0.0f, 150.0f);
        ReadIniScale(configPath, L"Front", L"CoreSize", s_FrontCoreSizeScale, 40.0f, 180.0f);
        ReadIniScale(configPath, L"Front", L"CoreBrightness", s_FrontCoreBrightness, 0.0f, 100.0f);
        ReadIniAngleDegrees(configPath, L"Front", L"AngleFadeStartDegrees", s_FrontAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Front", L"AngleFadeEndDegrees", s_FrontAngleFadeEnd);
        ReadIniAngleDegrees(configPath, L"Front", L"CoreAngleFadeStartDegrees", s_FrontCoreAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Front", L"CoreAngleFadeEndDegrees", s_FrontCoreAngleFadeEnd);
        ReadIniDistance(configPath, L"Front", L"CoreDistanceFadeStart", s_FrontCoreDistanceFadeStart, 5.0f, 30.0f);
        ReadIniDistance(configPath, L"Front", L"CoreDistanceFadeEnd", s_FrontCoreDistanceFadeEnd, 31.0f, 45.0f);

        ReadIniScale(configPath, L"Rear", L"StreakLength", s_RearStreakLengthScale, 25.0f, 250.0f);
        ReadIniScale(configPath, L"Rear", L"StreakHeight", s_RearStreakHeightScale, 50.0f, 200.0f);
        ReadIniScale(configPath, L"Rear", L"StreakBrightness", s_RearStreakBrightness, 0.0f, 150.0f);
        ReadIniScale(configPath, L"Rear", L"SideCoreSize", s_RearRoundCoreSizeScale, 20.0f, 200.0f);
        ReadIniScale(configPath, L"Rear", L"SideCoreBrightness", s_RearRoundCoreBrightness, 0.0f, 100.0f);
        ReadIniDistance(configPath, L"Rear", L"DistanceFadeStart", s_RearTailDistanceFadeStart, 2.0f, 20.0f);
        ReadIniDistance(configPath, L"Rear", L"DistanceFadeEnd", s_RearTailDistanceFadeEnd, 21.0f, 35.0f);
        ReadIniDistance(configPath, L"Rear", L"RearProjectionDrawDistance",
                        s_RearProjectionDrawDistance, 15.0f, 200.0f);
        ReadIniDistance(configPath, L"Rear", L"RearProjectionOffset", s_CarRearProjectionOffset, 0.0f, 10.0f);
        ReadIniDistance(configPath, L"Rear", L"SingleLampProjectionOffset", s_SingleRearProjectionOffset, 0.0f, 15.0f);
        // Tractor (531), bikes and Quadbike have built-in single-projection handling.
        // This list additionally enables it for custom model IDs.
        s_SingleRearProjectionModelIds.clear();
        ReadIniModelIdList(configPath, L"Rear", L"SingleProjectionModelIDs", s_SingleRearProjectionModelIds);
        // Projection style: 0 = off, 1 = Vice City, 2 = LCS-style.
        // Keep compatibility with older INI files that only had GroundProjectionEnabled.
        float rearProjectionStyleValue = static_cast<float>(s_RearProjectionStyle);
        if (ReadIniNumber(configPath, L"Rear", L"RearProjectionStyle", rearProjectionStyleValue))
        {
            rearProjectionStyleValue = std::max(0.0f, std::min(2.0f, rearProjectionStyleValue));
            s_RearProjectionStyle = static_cast<std::int32_t>(std::lround(rearProjectionStyleValue));
        }
        else
        {
            bool legacyGroundProjectionEnabled = s_RearProjectionStyle != 0;
            if (ReadIniBool(configPath, L"Rear", L"GroundProjectionEnabled", legacyGroundProjectionEnabled))
                s_RearProjectionStyle = legacyGroundProjectionEnabled ? 2 : 0;
        }
        float rearStreakVisibilityStyleValue = static_cast<float>(s_RearStreakVisibilityStyle);
        if (ReadIniNumber(configPath, L"Rear", L"RearStreakVisibilityStyle", rearStreakVisibilityStyleValue))
        {
            rearStreakVisibilityStyleValue = std::max(0.0f, std::min(1.0f, rearStreakVisibilityStyleValue));
            s_RearStreakVisibilityStyle = static_cast<std::int32_t>(std::lround(rearStreakVisibilityStyleValue));
        }

        ReadIniAngleDegrees(configPath, L"Rear", L"RoundEntryAngleStartDegrees", s_RearRoundAngleEntryStart);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundEntryAngleEndDegrees", s_RearRoundAngleEntryEnd);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundExitAngleStartDegrees", s_RearRoundAngleExitStart);
        ReadIniAngleDegrees(configPath, L"Rear", L"RoundExitAngleEndDegrees", s_RearRoundAngleExitEnd);

        bool whiteReverseLightsEnabled = s_WhiteReverseLightsEnabled != 0;
        if (ReadIniBool(configPath, L"Reverse", L"WhiteLightsEnabled", whiteReverseLightsEnabled))
            s_WhiteReverseLightsEnabled = whiteReverseLightsEnabled ? 1 : 0;

        ReadIniScale(configPath, L"Reverse", L"StreakLength", s_ReverseStreakLengthScale, 25.0f, 250.0f);
        ReadIniScale(configPath, L"Reverse", L"StreakBrightness", s_ReverseStreakBrightness, 0.0f, 150.0f);
        ReadIniScale(configPath, L"Reverse", L"CoreSize", s_ReverseCoreSizeScale, 40.0f, 160.0f);
        ReadIniScale(configPath, L"Reverse", L"CoreBrightness", s_ReverseCoreBrightness, 0.0f, 100.0f);
        ReadIniScale(configPath, L"Reverse", L"BikeStreakSize", s_BikeReverseStreakSizeMultiplier, 10.0f, 100.0f);
        ReadIniScale(configPath, L"Reverse", L"BikeCoreSize", s_BikeReverseCoreSizeMultiplier, 10.0f, 100.0f);
        ReadIniScale(configPath, L"Reverse", L"BikeQuadBrightness", s_BikeReverseBrightnessMultiplier, 0.0f, 100.0f);
        ReadIniAngleDegrees(configPath, L"Reverse", L"CoreAngleFadeStartDegrees", s_ReverseCoreAngleFadeStart);
        ReadIniAngleDegrees(configPath, L"Reverse", L"CoreAngleFadeEndDegrees", s_ReverseCoreAngleFadeEnd);

        ReadIniNumber(configPath, L"Advanced", L"SideAngleExponent", s_SideAngleExponent);
        s_SideAngleExponent = std::max(2.0f, std::min(8.0f, s_SideAngleExponent));
        ReadIniDistance(configPath, L"Advanced", L"StreakDistanceFadeStart", s_StreakDistanceFadeStart, 5.0f, 190.0f);
        // Backward compatibility: old INIs used StreakDistanceFadeEnd for the culling limit.
        ReadIniDistance(configPath, L"Advanced", L"StreakDistanceFadeEnd", s_StreakDrawDistance, 10.0f, 200.0f);
        // New setting overrides the legacy key when both are present.
        ReadIniDistance(configPath, L"Advanced", L"StreakDrawDistance", s_StreakDrawDistance, 10.0f, 200.0f);
        if (s_StreakDistanceFadeStart >= s_StreakDrawDistance)
            s_StreakDistanceFadeStart = std::max(5.0f, s_StreakDrawDistance * 0.65f);

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
        float m_fScreenX = 0.0f;
        float m_fScreenY = 0.0f;
        float m_fDepth = 0.0f;
        float m_fReciprocalDepth = 0.0f;
        std::uint8_t m_nRed = 255;
        std::uint8_t m_nGreen = 255;
        std::uint8_t m_nBlue = 255;
        float m_fIntensity = 1.0f;
        // Independent angular visibility used to scale streak length under oblique views.
        float m_fAngleFade = 1.0f;
        bool m_bValid = false;
    };

    struct StreakHistory
    {
        std::array<StreakSample, HEADLIGHT_STREAK_HISTORY> m_aSamples{};
        unsigned int m_nLastUpdatedFrame = 0xFFFFFFFFu;
        unsigned int m_nLastSeenFrame = 0;
        int m_nSampleCount = 0;
        bool m_bActive = false;
        bool m_bIsRear = false;
        bool m_bIsReverse = false;
        float m_fSizeScale = 1.0f;
    };

    std::unordered_map<unsigned int, StreakHistory> s_StreakHistoryById;

    unsigned int s_RearProjectionPassFrame = 0xFFFFFFFFu;

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

        // Cull the custom streak at the user-configurable depth limit and fade it
        // smoothly before that cutoff. screenPosition.z is camera-space depth.
        if (screenPosition.z <= 0.5f || screenPosition.z >= s_StreakDrawDistance)
            return false;

        sample.m_fScreenX = screenPosition.x;
        sample.m_fScreenY = screenPosition.y;
        sample.m_fDepth = screenPosition.z;
        sample.m_fReciprocalDepth = 1.0f / screenPosition.z;
        // Keep colour separate from intensity. Applying intensity to both RGB
        // and alpha would square the fade and make the transition look abrupt.
        sample.m_nRed = colour.m_nRed;
        sample.m_nGreen = colour.m_nGreen;
        sample.m_nBlue = colour.m_nBlue;
        // Keep the fade interval valid if the Debug Menu changes draw distance
        // below the configured fade-start value.
        const float distanceFadeStart = std::min(
            s_StreakDistanceFadeStart, s_StreakDrawDistance * 0.90f);
        const float distanceFade = 1.0f - SmootherStep(
            distanceFadeStart, s_StreakDrawDistance, screenPosition.z);
        sample.m_fIntensity = Clamp01(intensity) * distanceFade;
        sample.m_fAngleFade = Clamp01(angleFade);
        sample.m_bValid = sample.m_fReciprocalDepth > 0.0f && sample.m_fIntensity > 0.001f;
        return sample.m_bValid;
    }

    void UpdateStreakHistory(unsigned int coronaId, const CVector& worldPosition,
                             const HeadlightColour& colour, float intensity,
                             bool isRear, float angleFade,
                             float sizeScale, bool isReverse)
    {
        if (!s_CustomStreakEnabled)
            return;

        StreakHistory& history = s_StreakHistoryById[coronaId];
        history.m_bIsRear = isRear;
        history.m_bIsReverse = isReverse;
        history.m_fSizeScale = std::max(0.1f, std::min(1.0f, sizeScale));
        const unsigned int frame = CTimer::m_FrameCounter;
        StreakSample current;

        history.m_nLastSeenFrame = frame;
        history.m_bActive = true;

        if (!ProjectLightToScreen(worldPosition, current, colour, intensity, angleFade))
        {
            history.m_bActive = false;
            history.m_nSampleCount = 0;
            return;
        }

        if (history.m_nLastUpdatedFrame != frame)
        {
            for (int i = HEADLIGHT_STREAK_HISTORY - 1; i > 0; --i)
                history.m_aSamples[i] = history.m_aSamples[i - 1];

            if (history.m_nSampleCount < HEADLIGHT_STREAK_HISTORY)
                ++history.m_nSampleCount;

            history.m_nLastUpdatedFrame = frame;
        }

        history.m_aSamples[0] = current;
    }

    void ClearStreakHistory(unsigned int coronaId)
    {
        auto it = s_StreakHistoryById.find(coronaId);
        if (it != s_StreakHistoryById.end())
        {
            it->second.m_bActive = false;
            it->second.m_nSampleCount = 0;
        }
    }

    // Draw immediately instead of relying on the shared sprite buffer. The
    // stock streak raster is still used, but every history sample is emitted as
    // a visible screen-space flare. This mirrors re3's per-corona history while
    // avoiding buffer state/order problems in a Plugin-SDK drawing callback.
    void RenderStreakSprite(const StreakSample& sample, float halfWidth,
                            float halfHeight, std::uint8_t red,
                            std::uint8_t green, std::uint8_t blue,
                            short intensity)
    {
        if (!sample.m_bValid || sample.m_fReciprocalDepth <= 0.0f || intensity <= 0)
            return;

        // SA's sprite implementation scales RGB by the short intensity value;
        // the byte after rhw is vertex alpha. Keep vertex alpha fully enabled
        // for additive blending, fade RGB through intensity, and use default UVs.
        CSprite::RenderOneXLUSprite(
            sample.m_fScreenX, sample.m_fScreenY, sample.m_fDepth,
            halfWidth, halfHeight,
            red, green, blue,
            intensity, sample.m_fReciprocalDepth,
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
            if (frame - history.m_nLastSeenFrame > 1u)
            {
                it = s_StreakHistoryById.erase(it);
                continue;
            }

            if (!history.m_bActive || history.m_nSampleCount < 1 || !history.m_aSamples[0].m_bValid)
            {
                ++it;
                continue;
            }

            const StreakSample& current = history.m_aSamples[0];
            const float currentIntensity = Clamp01(current.m_fIntensity);
            if (currentIntensity <= 0.001f)
            {
                ++it;
                continue;
            }

            // Draw one main sprite; repeated full-size samples would over-brighten
            // a stationary lamp under additive blending.
            const float baseHalfWidth = history.m_bIsRear
                ? REAR_STREAK_BASE_HALF_WIDTH : HEADLIGHT_STREAK_BASE_HALF_WIDTH;
            const float maxHalfWidth = history.m_bIsRear
                ? REAR_STREAK_MAX_HALF_WIDTH : HEADLIGHT_STREAK_MAX_HALF_WIDTH;
            const float streakLengthScale = !history.m_bIsRear
                ? s_FrontStreakLengthScale
                : (history.m_bIsReverse ? s_ReverseStreakLengthScale : s_RearStreakLengthScale);
            const float streakHeightScale = history.m_bIsRear
                ? s_RearStreakHeightScale : s_FrontStreakHeightScale;
            const float halfHeight = (history.m_bIsRear
                ? REAR_STREAK_HALF_HEIGHT : HEADLIGHT_STREAK_HALF_HEIGHT) *
                history.m_fSizeScale * streakHeightScale;
            const float rawHalfWidth = (baseHalfWidth +
                (maxHalfWidth - baseHalfWidth) * currentIntensity) *
                history.m_fSizeScale * streakLengthScale;
            // Keep a short line at oblique angles instead of collapsing to zero.
            // Smooth the growth curve so the streak progressively lengthens as the
            // camera turns toward the lamp; the menu exponent controls its midpoint.
            const float angleFacing = Clamp01(current.m_fAngleFade);
            const float angleExponent = std::max(0.5f, s_SideAngleExponent * 0.25f);
            const float angleProgress = std::pow(angleFacing, angleExponent);
            const float easedAngleProgress = SmootherStep(0.0f, 1.0f, angleProgress);
            const float angleWidthScale = STREAK_MIN_SIDE_WIDTH_SCALE +
                (1.0f - STREAK_MIN_SIDE_WIDTH_SCALE) * easedAngleProgress;
            const float angleHeightScale = 0.72f + 0.28f * easedAngleProgress;
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
            const float baseAlpha = history.m_bIsRear
                ? REAR_STREAK_BASE_ALPHA : HEADLIGHT_STREAK_BASE_ALPHA;
            const float streakBrightness = !history.m_bIsRear
                ? s_FrontStreakBrightness
                : (history.m_bIsReverse ? s_ReverseStreakBrightness : s_RearStreakBrightness);
            const short mainAlpha = static_cast<short>(std::min(
                255.0f, baseAlpha * currentIntensity * streakBrightness));
            RenderStreakSprite(current, mainHalfWidth, mainHalfHeight,
                               current.m_nRed, current.m_nGreen, current.m_nBlue, mainAlpha);

            // Add faint history echoes only while the lamp moves on-screen.
            int oldestValidIndex = -1;
            for (int i = history.m_nSampleCount - 1; i >= 1; --i)
            {
                if (history.m_aSamples[i].m_bValid)
                {
                    oldestValidIndex = i;
                    break;
                }
            }

            if (oldestValidIndex >= 1)
            {
                const StreakSample& oldest = history.m_aSamples[oldestValidIndex];
                const float dx = current.m_fScreenX - oldest.m_fScreenX;
                const float dy = current.m_fScreenY - oldest.m_fScreenY;
                const float motionSquared = dx * dx + dy * dy;

                if (motionSquared > 9.0f && currentIntensity > 0.12f &&
                    current.m_fAngleFade > 0.18f && widthFade > 0.08f)
                {
                    const int echoCount = std::min(4, oldestValidIndex);
                    for (int echo = 1; echo <= echoCount; ++echo)
                    {
                        const float t = static_cast<float>(echo) / (echoCount + 1.0f);
                        StreakSample interpolated = current;
                        interpolated.m_fScreenX = current.m_fScreenX + (oldest.m_fScreenX - current.m_fScreenX) * t;
                        interpolated.m_fScreenY = current.m_fScreenY + (oldest.m_fScreenY - current.m_fScreenY) * t;
                        interpolated.m_fDepth = current.m_fDepth + (oldest.m_fDepth - current.m_fDepth) * t;
                        interpolated.m_fReciprocalDepth = current.m_fReciprocalDepth +
                            (oldest.m_fReciprocalDepth - current.m_fReciprocalDepth) * t;
                        const float echoBaseAlpha = history.m_bIsRear ? 20.0f : 24.0f;
                        const short echoAlpha = static_cast<short>(std::min(
                            255.0f, echoBaseAlpha * currentIntensity * streakBrightness * (1.0f - t)));
                        RenderStreakSprite(interpolated,
                            std::max(1.0f, mainHalfWidth * 0.18f),
                            (history.m_bIsRear ? 3.6f : 4.4f) * angleWidthScale *
                                angleHeightScale * history.m_fSizeScale,
                            interpolated.m_nRed, interpolated.m_nGreen, interpolated.m_nBlue,
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

    CVector TransformLocalPosition(const CMatrix& matrix, const CVector& origin,
                                   const CVector& localPosition)
    {
        return CVector(
            origin.x + matrix.right.x * localPosition.x +
                matrix.up.x * localPosition.y + matrix.at.x * localPosition.z,
            origin.y + matrix.right.y * localPosition.x +
                matrix.up.y * localPosition.y + matrix.at.y * localPosition.z,
            origin.z + matrix.right.z * localPosition.x +
                matrix.up.z * localPosition.y + matrix.at.z * localPosition.z
        );
    }

    // Forward declarations required by the lean-aware light position transform.
    bool IsMotorcycleModel(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo);
    bool UsesSingleLampPosition(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo,
                               int dummyIndex);

    // SA renders leaning motorcycles from CBike::m_mLeanMatrix. The base CVehicle
    // matrix does not contain that rendered roll, so transforming a lamp dummy only
    // through m_matrix makes our screen-space streak drift away from the actual lamp
    // when the rider turns. Match the game's lean-aware transform: transform the
    // local model dummy by m_mLeanMatrix, then add the vehicle world position.
    CVector TransformVehicleLightPosition(CVehicle* vehicle,
                                          CVehicleModelInfo* modelInfo,
                                          const CVector& localPosition)
    {
        if (vehicle && vehicle->m_matrix && IsMotorcycleModel(vehicle, modelInfo))
        {
            auto* bike = static_cast<CBike*>(vehicle);
            bike->CalculateLeanMatrix();
            const CMatrix& leanMatrix = bike->m_mLeanMatrix;
            const CVector& origin = vehicle->m_matrix->pos;
            return TransformLocalPosition(leanMatrix, origin, localPosition);
        }

        const CMatrix& matrix = *vehicle->m_matrix;
        return TransformLocalPosition(matrix, matrix.pos, localPosition);
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
        bool hasModelDummy = false;
        if (modelInfo && modelInfo->m_pVehicleStruct)
        {
            const CVector dummyPos = modelInfo->m_pVehicleStruct->m_avDummyPos[dummyIndex];
            if (IsPositionSet(dummyPos))
            {
                localLightPosition = dummyPos;
                hasModelDummy = true;

                // A few models provide a dummy with height/lateral data but no
                // useful front/back offset. Do not collapse front and rear sources
                // onto the vehicle origin in that case; retain the matching fallback.
                if (std::fabs(localLightPosition.y) < 0.25f)
                {
                    localLightPosition.y = dummyIndex == DUMMY_REAR_LIGHTS
                        ? DEFAULT_REAR_LIGHT_Y : DEFAULT_HEADLIGHT_Y;
                }
            }
        }

        const bool singleLampPosition = UsesSingleLampPosition(vehicle, modelInfo, dummyIndex);
        if (singleLampPosition)
        {
            if (!hasModelDummy)
                localLightPosition.x = 0.0f;
        }
        else if (std::fabs(localLightPosition.x) < 0.25f)
        {
            // Some model dummies mark the centre of a paired lamp assembly.
            // A mirrored centre coordinate creates two streaks at exactly the same
            // screen point, which looks detached from the physical lamps. Use the
            // established fallback half-spacing only when the dummy has no lateral offset.
            localLightPosition.x = dummyIndex == DUMMY_REAR_LIGHTS
                ? -DEFAULT_HEADLIGHT_X : DEFAULT_HEADLIGHT_X;
        }

        leftLightPosition = TransformVehicleLightPosition(vehicle, modelInfo, localLightPosition);
        if (singleLampPosition)
        {
            // Single-lamp vehicles use the model's real dummy once. Do not invent
            // a mirrored source: it separates from the lamp during lean and steering.
            rightLightPosition = leftLightPosition;
        }
        else
        {
            rightLightPosition = TransformVehicleLightPosition(
                vehicle, modelInfo, MirrorHeadlightPosition(localLightPosition));
        }
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

    // Camera pitch must not change a streak's angular size. Calculate only the
    // horizontal camera-to-vehicle direction; distance fades still use 3D distance.
    float GetCameraHorizontalBehindness(const CVehicle* vehicle)
    {
        const CVector& cameraPosition = TheCamera.m_mCameraMatrix.pos;
        const CVector& vehiclePosition = vehicle->m_matrix->pos;
        const CVector& forward = vehicle->m_matrix->up;

        const float toVehicleX = vehiclePosition.x - cameraPosition.x;
        const float toVehicleY = vehiclePosition.y - cameraPosition.y;
        const float horizontalDistance = std::sqrt(
            toVehicleX * toVehicleX + toVehicleY * toVehicleY);
        const float forwardLength = std::sqrt(
            forward.x * forward.x + forward.y * forward.y);
        if (horizontalDistance <= 0.0001f || forwardLength <= 0.0001f)
            return 0.0f;

        const float behindness =
            (toVehicleX * forward.x + toVehicleY * forward.y) /
            (horizontalDistance * forwardLength);
        return std::max(-1.0f, std::min(1.0f, behindness));
    }

    // Measure how directly the camera faces this specific lamp. The horizontal
    // dot product is deliberately kept separate from the lamp's visibility/alpha
    // fade: it controls progressive streak size from head-on through broadside,
    // using the same smooth curve at every camera elevation.
    float GetStreakFacingFactor(const CVehicle* vehicle, const CVector& lightPosition,
                                bool isRearLamp)
    {
        if (!vehicle || !vehicle->m_matrix)
            return 0.0f;

        const CVector& cameraPosition = TheCamera.m_mCameraMatrix.pos;
        const CVector& forward = vehicle->m_matrix->up;
        const float toCameraX = cameraPosition.x - lightPosition.x;
        const float toCameraY = cameraPosition.y - lightPosition.y;
        const float horizontalDistance = std::sqrt(
            toCameraX * toCameraX + toCameraY * toCameraY);
        const float forwardLength = std::sqrt(
            forward.x * forward.x + forward.y * forward.y);
        if (horizontalDistance <= 0.0001f || forwardLength <= 0.0001f)
            return 0.0f;

        float facing = (toCameraX * forward.x + toCameraY * forward.y) /
                       (horizontalDistance * forwardLength);
        if (isRearLamp)
            facing = -facing;
        return Clamp01(facing);
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
                             const HeadlightColour& colour, std::uint8_t alpha,
                             float size, float farClip, eCoronaType coronaType,
                             float coronaAngle)
    {
        CCoronas::RegisterCorona(
            coronaId, nullptr,
            colour.m_nRed, colour.m_nGreen, colour.m_nBlue, alpha,
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
        const std::uint8_t alpha = static_cast<std::uint8_t>(scaledAlpha);
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
                            static_cast<std::uint8_t>(scaledAlpha),
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
                            static_cast<std::uint8_t>(scaledAlpha),
                            0.30f * sizeScale * coreSizeScale,
                            HEADLIGHT_CORE_FAR_CLIP, CORONATYPE_HEADLIGHT, coronaAngle);
    }

    void ProcessRearLights(CVehicle* vehicle, bool isReversing, bool isBike,
                          bool isSingleRearLamp, float reverseBrightnessMultiplier,
                          float cameraBehindness, float cameraDistance,
                          unsigned int baseCoronaId, float coronaAngle,
                          const CVector& leftLightPosition, const CVector& rightLightPosition)
    {
        const unsigned int leftStreakId = baseCoronaId + 0x7Du;
        const unsigned int rightStreakId = baseCoronaId + 0x7Eu;
        const float horizontalBehindness = GetCameraHorizontalBehindness(vehicle);
        // Keep rear streaks visible at broadside. The per-lamp facing factor
        // below controls size; this smooth envelope controls only visibility.
        const float rearStreakVisibility = SmootherStep(-0.75f, 0.90f, horizontalBehindness);

        const bool rearLightsEnabled = vehicle->bLightsOn || isReversing;
        const float normalIntensity = Clamp01(0.38f + 0.30f * horizontalBehindness);

        // The streak and side-view corona share the same colour transition.
        const HeadlightColour streakColour = isReversing
            ? HeadlightColour{ 205, 220, 250 }
            : HeadlightColour{ 255, 24, 12 };
        const float streakBrightness = isReversing
            ? 0.60f * Clamp01(reverseBrightnessMultiplier) : normalIntensity;
        // The distance option affects both red and white streaks. Keep reverse
        // streak activation independent from the red-tail broadside visibility gate.
        const float rearTailDistanceFade = SmootherStep(
            s_RearTailDistanceFadeStart, s_RearTailDistanceFadeEnd,
            cameraDistance);
        const float distanceFade = s_RearStreakVisibilityStyle != 0
            ? 1.0f : rearTailDistanceFade;
        const float redStreakIntensity = streakBrightness * rearStreakVisibility * distanceFade;
        // White reverse streaks must remain visible at close range. The general
        // RearStreakVisibilityStyle is for red tail-lamp streaks; reverse streaks
        // still respect StreakDrawDistance and fade out before its depth cutoff.
        const float reverseStreakDistanceFade = 1.0f;
        const float reverseStreakIntensity = streakBrightness * reverseStreakDistanceFade;

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
        if (isSingleRearLamp)
        {
            // Motorcycles and single-rear-lamp models use the real centered dummy once.
            // Do not mirror it into two artificial streak sources.
            RegisterRearLampCore(baseCoronaId + 0x81u, leftLightPosition, streakColour, coronaAngle,
                                 roundFade, reverseLampSizeScale, isReversing,
                                 reverseBrightnessMultiplier);
            RegisterRearLampCore(baseCoronaId + 0x82u, rightLightPosition, streakColour, coronaAngle,
                                 0.0f, reverseLampSizeScale, isReversing,
                                 reverseBrightnessMultiplier);

            if (!rearLightsEnabled || (!isReversing && rearStreakVisibility <= 0.001f))
            {
                ClearStreakHistory(leftStreakId);
                ClearStreakHistory(rightStreakId);
                return;
            }

            const float streakSizeScale = isReversing && isBike
                ? s_BikeReverseStreakSizeMultiplier : 1.0f;
            const float leftFacing = GetStreakFacingFactor(vehicle, leftLightPosition, true);
            const float streakIntensity = isReversing
                ? reverseStreakIntensity : redStreakIntensity;
            UpdateStreakHistory(leftStreakId, leftLightPosition, streakColour, streakIntensity,
                                true, leftFacing, streakSizeScale, isReversing);
            ClearStreakHistory(rightStreakId);
            return;
        }

        RegisterRearLampCore(baseCoronaId + 0x81u, leftLightPosition, streakColour, coronaAngle,
                             roundFade, reverseLampSizeScale, isReversing,
                             reverseBrightnessMultiplier);
        RegisterRearLampCore(baseCoronaId + 0x82u, rightLightPosition, streakColour, coronaAngle,
                             roundFade, reverseLampSizeScale, isReversing,
                             reverseBrightnessMultiplier);

        // Do not register another long native corona for ordinary tail lamps.
        // The stock lamps remain intact; the custom layer draws the elongated line
        // from the rear view and the small circular replacement near side angles.
        if (!rearLightsEnabled || (!isReversing && rearStreakVisibility <= 0.001f))
        {
            ClearStreakHistory(leftStreakId);
            ClearStreakHistory(rightStreakId);
            return;
        }

        const float streakSizeScale = isReversing && isBike
            ? s_BikeReverseStreakSizeMultiplier : 1.0f;
        const float leftFacing = GetStreakFacingFactor(vehicle, leftLightPosition, true);
        const float rightFacing = GetStreakFacingFactor(vehicle, rightLightPosition, true);
        const float streakIntensity = isReversing
            ? reverseStreakIntensity : redStreakIntensity;
        UpdateStreakHistory(leftStreakId, leftLightPosition, streakColour, streakIntensity,
                              true, leftFacing, streakSizeScale, isReversing);
        UpdateStreakHistory(rightStreakId, rightLightPosition, streakColour, streakIntensity,
                              true, rightFacing, streakSizeScale, isReversing);
    }

    // Disable custom rear/reverse-light effects only for pedal bicycles.
    // Register zero-alpha coronas each frame so previously visible effects vanish immediately.
    void HideRearLightEffects(unsigned int baseCoronaId, float coronaAngle,
                              const CVector& leftLightPosition, const CVector& rightLightPosition)
    {
        ClearStreakHistory(baseCoronaId + 0x7Du);
        ClearStreakHistory(baseCoronaId + 0x7Eu);

        RegisterReverseLightCore(baseCoronaId + 0x79u, leftLightPosition, coronaAngle, 0.0f, 1.0f, 1.0f);
        RegisterReverseLightCore(baseCoronaId + 0x7Au, rightLightPosition, coronaAngle, 0.0f, 1.0f, 1.0f);

        const HeadlightColour hiddenColour{ 0, 0, 0 };
        RegisterRearLampCore(baseCoronaId + 0x81u, leftLightPosition, hiddenColour, coronaAngle,
                             0.0f, 1.0f, false, 1.0f);
        RegisterRearLampCore(baseCoronaId + 0x82u, rightLightPosition, hiddenColour, coronaAngle,
                             0.0f, 1.0f, false, 1.0f);
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

    bool IsPedalBicycleModel(int modelId)
    {
        return modelId == BMX_MODEL_ID ||
               modelId == BIKE_MODEL_ID ||
               modelId == MOUNTAIN_BIKE_MODEL_ID;
    }

    bool HasNoCustomRearLights(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo)
    {
        if (!vehicle)
            return true;

        return vehicle->m_nModelIndex == HARVESTER_MODEL_ID ||
               IsPedalBicycleModel(vehicle->m_nModelIndex) ||
               (modelInfo && modelInfo->m_nVehicleType == VEHICLE_BMX);
    }

    bool IsMotorcycleModel(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo)
    {
        return vehicle && modelInfo &&
            modelInfo->m_nVehicleType == VEHICLE_BIKE &&
            !IsPedalBicycleModel(vehicle->m_nModelIndex);
    }

    bool IsQuadBikeModel(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo)
    {
        return vehicle && ((modelInfo && modelInfo->m_nVehicleType == VEHICLE_QUAD) ||
                           vehicle->m_nModelIndex == QUADBIKE_MODEL_ID);
    }

    bool IsConfiguredSingleProjectionModel(int modelId)
    {
        return std::find(s_SingleRearProjectionModelIds.begin(),
                         s_SingleRearProjectionModelIds.end(), modelId) !=
               s_SingleRearProjectionModelIds.end();
    }

    bool IsSingleRearLampModel(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo)
    {
        return vehicle && !HasNoCustomRearLights(vehicle, modelInfo) &&
            (IsMotorcycleModel(vehicle, modelInfo) || IsQuadBikeModel(vehicle, modelInfo) ||
             vehicle->m_nModelIndex == TRACTOR_MODEL_ID ||
             IsConfiguredSingleProjectionModel(vehicle->m_nModelIndex));
    }

    bool UsesSingleLampPosition(const CVehicle* vehicle, const CVehicleModelInfo* modelInfo,
                               int dummyIndex)
    {
        if (IsMotorcycleModel(vehicle, modelInfo))
            return true; // Single centered front and rear lamp on motorcycles.

        return dummyIndex == DUMMY_REAR_LIGHTS && IsSingleRearLampModel(vehicle, modelInfo);
    }

    // Use the flags GTA itself assigns to the visible rear lamp components.
    // bLightsOn is only a switch/request flag and m_fBreakPedal can remain
    // non-zero during an exit animation, so neither is sufficient on its own.
    bool AreRearLampRenderFlagsActive(const CVehicle* vehicle)
    {
        return vehicle &&
            (vehicle->m_renderLights.m_bLeftRear || vehicle->m_renderLights.m_bRightRear);
    }

    // Vehicle flags can be late or incomplete for bike exit animations. Check both
    // the vehicle transition flags and the driver's own state to avoid a one-frame
    // ground projection while the player is getting off a motorcycle.
    bool IsVehicleInExitTransition(const CVehicle* vehicle)
    {
        if (!vehicle)
            return false;

        if (vehicle->m_nGettingOutFlags != 0)
            return true;

        const CPed* driver = vehicle->m_pDriver;
        return driver &&
            (driver->m_ePedState == PEDSTATE_EXIT_CAR ||
             driver->bHasJustLeftCar ||
             !driver->bInVehicle ||
             driver->m_pVehicle != vehicle);
    }

    // Store the shadow directly instead of using StoreCarLightShadow: the native
    // helper applies camera-distance/view-direction tests and a cached static-shadow
    // path for stationary NPCs. The global pass below registers it independent of
    // whether the vehicle body itself is rendered from the current camera angle.
    // Keep red rear projections at the same base intensity as the softened white reverse projection.
    constexpr short REAR_PROJECTION_INTENSITY = 96;
    constexpr short WHITE_REVERSE_PROJECTION_INTENSITY = REAR_PROJECTION_INTENSITY;
    constexpr std::size_t MAX_REAR_PROJECTION_SHADOWS_PER_FRAME = 24;
    std::size_t s_RearProjectionShadowCount = 0;

    bool StoreRearProjection(RwTexture* texture, const CVector& shadowPosition,
                             float frontX, float frontY, float sideX, float sideY,
                             std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                             short baseIntensity)
    {
        if (!texture || !TheCamera.m_pRwCamera ||
            s_RearProjectionShadowCount >= MAX_REAR_PROJECTION_SHADOWS_PER_FRAME)
            return false;

        const CVector& cameraPosition = TheCamera.m_mCameraMatrix.pos;
        const float dx = shadowPosition.x - cameraPosition.x;
        const float dy = shadowPosition.y - cameraPosition.y;
        const float dz = shadowPosition.z - cameraPosition.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        const float drawDistance = std::max(15.0f, s_RearProjectionDrawDistance);
        const float maxDistanceSquared = drawDistance * drawDistance;
        if (distanceSquared >= maxDistanceSquared)
            return false;

        // Fade smoothly only over the last 30% of the configured range. This
        // gives projections a substantially longer range while keeping their
        // near-camera brightness stable and avoids directional camera cutoffs.
        const float cameraDistance = std::sqrt(distanceSquared);
        const float fadeStart = drawDistance * 0.70f;
        const float distanceFade = 1.0f - SmootherStep(
            fadeStart, drawDistance, cameraDistance);
        const short intensity = static_cast<short>(std::lround(
            static_cast<float>(baseIntensity) * Clamp01(distanceFade)));
        if (intensity <= 0)
            return false;

        // Match the native dynamic shadow's parameters, but enable building
        // surfaces for NPC vehicles as well as the player's vehicle.
        CShadows::StoreShadowToBeRendered(
            SHADOW_ADDITIVE, texture, &shadowPosition,
            frontX, frontY, sideX, sideY,
            intensity, red, green, blue,
            6.0f, false, 1.0f, nullptr, true);
        ++s_RearProjectionShadowCount;
        return true;
    }

    // Register one rear-ground projection per vehicle, not one per lamp. Paired
    // rear lamps share a centered projection; single-lamp vehicles use the compact
    // VC texture even when the overall projection style is set to LCS.
    void RegisterRearGroundLighting(CVehicle* vehicle, CVehicleModelInfo* modelInfo,
                                    bool rearLampsPhysicallyLit, bool whiteReverseLightsActive,
                                    const CVector& leftRearLightPosition,
                                    const CVector& rightRearLightPosition)
    {
        if (s_RearProjectionStyle == 0 || !vehicle || !vehicle->m_matrix || !modelInfo)
            return;

        const int modelId = vehicle->m_nModelIndex;
        if (HasNoCustomRearLights(vehicle, modelInfo))
            return;

        const bool isRoadVehicle =
            modelInfo->m_nVehicleType == VEHICLE_AUTOMOBILE ||
            modelInfo->m_nVehicleType == VEHICLE_MTRUCK;
        const bool isMotorcycle = IsMotorcycleModel(vehicle, modelInfo);
        const bool isQuadBike = IsQuadBikeModel(vehicle, modelInfo);
        const bool isCustomProjectionModel = IsConfiguredSingleProjectionModel(modelId);
        if (!isRoadVehicle && !isMotorcycle && !isQuadBike &&
            modelId != TRACTOR_MODEL_ID && !isCustomProjectionModel)
            return;

        bool leftRearLightWorking = true;
        bool rightRearLightWorking = true;
        if (isRoadVehicle)
        {
            CAutomobile* automobile = static_cast<CAutomobile*>(vehicle);
            leftRearLightWorking =
                automobile->m_damageManager.GetLightStatus(LIGHT_REAR_LEFT) == 0;
            rightRearLightWorking =
                automobile->m_damageManager.GetLightStatus(LIGHT_REAR_RIGHT) == 0;
        }

        const bool redRearLampsLit = rearLampsPhysicallyLit &&
            (leftRearLightWorking || rightRearLightWorking);
        const bool whiteReverseProjection =
            s_RearProjectionStyle == 2 && whiteReverseLightsActive;
        if (!redRearLampsLit && !whiteReverseProjection)
            return;

        const CVector& vehicleForward = vehicle->m_matrix->up;
        const float forwardLength = std::sqrt(
            vehicleForward.x * vehicleForward.x + vehicleForward.y * vehicleForward.y);
        if (forwardLength <= 0.0001f)
            return;

        const float separationX = leftRearLightPosition.x - rightRearLightPosition.x;
        const float separationY = leftRearLightPosition.y - rightRearLightPosition.y;
        const float separationZ = leftRearLightPosition.z - rightRearLightPosition.z;
        const float separationSquared = separationX * separationX +
            separationY * separationY + separationZ * separationZ;
        const bool hasPairedLampSources = separationSquared > 0.09f;
        const bool isSingleRearLamp = !hasPairedLampSources ||
            IsSingleRearLampModel(vehicle, modelInfo) || isCustomProjectionModel;
        const float projectionScale = isSingleRearLamp
            ? SINGLE_REAR_PROJECTION_SCALE : PAIRED_REAR_PROJECTION_SCALE;
        const float rearOffset = isSingleRearLamp
            ? s_SingleRearProjectionOffset : s_CarRearProjectionOffset;

        // Average the two physical lamp positions into one central anchor. This
        // restores the previous single-shadow-per-vehicle behavior and prevents
        // twin projections from separating or producing excessive brightness.
        CVector projectionAnchor(
            (leftRearLightPosition.x + rightRearLightPosition.x) * 0.5f,
            (leftRearLightPosition.y + rightRearLightPosition.y) * 0.5f,
            (leftRearLightPosition.z + rightRearLightPosition.z) * 0.5f);
        const float forwardX = vehicleForward.x / forwardLength;
        const float forwardY = vehicleForward.y / forwardLength;
        projectionAnchor.x -= rearOffset * forwardX;
        projectionAnchor.y -= rearOffset * forwardY;

        // Each projection style uses its own texture regardless of vehicle type:
        // Vice City uses the compact round shad_exp texture; LCS uses the directional
        // headlight texture, with headlight1 reserved for single-lamp vehicles.
        const bool useViceCityProjection = s_RearProjectionStyle == 1;
        RwTexture* projectionTexture = nullptr;
        if (useViceCityProjection)
        {
            projectionTexture = gpShadowExplosionTex;
        }
        else if (isSingleRearLamp)
        {
            projectionTexture = gpShadowHeadLightsTex2
                ? gpShadowHeadLightsTex2 : gpShadowHeadLightsTex;
        }
        else
        {
            projectionTexture = gpShadowHeadLightsTex;
        }
        if (!projectionTexture)
            return;

        float halfLength;
        float halfWidth;
        if (useViceCityProjection)
        {
            // Round Vice City texture/footprint for cars, motorcycles, Quadbike,
            // tractor and configured single-lamp models alike.
            halfLength = 3.0f * projectionScale;
            halfWidth = 3.0f * projectionScale;
        }
        else
        {
            // The same front/side proportions as the LCS headlight beam. For
            // a single lamp, SINGLE_REAR_PROJECTION_SCALE makes this ~10% smaller.
            halfLength = 3.1f * projectionScale;
            halfWidth = 2.4f * projectionScale;
        }

        const float frontX = -halfLength * forwardX;
        const float frontY = -halfLength * forwardY;
        const float sideX = halfWidth * forwardY;
        const float sideY = -halfWidth * forwardX;
        // StoreShadowToBeRendered expects the rectangle centre. Shift it by one
        // half-length so the nearest edge remains at the reduced lamp offset.
        CVector projectionCenter = projectionAnchor;
        projectionCenter.x += frontX;
        projectionCenter.y += frontY;

        if (whiteReverseProjection)
        {
            StoreRearProjection(
                projectionTexture, projectionCenter,
                frontX, frontY, sideX, sideY,
                45, 45, 45, WHITE_REVERSE_PROJECTION_INTENSITY);
        }
        else if (redRearLampsLit)
        {
            StoreRearProjection(
                projectionTexture, projectionCenter,
                frontX, frontY, sideX, sideY,
                35, 0, 0, REAR_PROJECTION_INTENSITY);
        }
    }

    // Keep the player's red point light in the vehicle render callback, where
    // CPointLights is being populated for the current render frame.
    void RegisterPlayerRearPointLight(CVehicle* vehicle, CVehicleModelInfo* modelInfo,
                                      bool rearLampsPhysicallyLit)
    {
        if (s_RearProjectionStyle == 0 || !vehicle || !vehicle->m_matrix ||
            vehicle != FindPlayerVehicle() || !rearLampsPhysicallyLit ||
            !modelInfo || HasNoCustomRearLights(vehicle, modelInfo))
            return;

        const bool isRoadVehicle =
            modelInfo->m_nVehicleType == VEHICLE_AUTOMOBILE ||
            modelInfo->m_nVehicleType == VEHICLE_MTRUCK;
        const bool isQuadBike =
            modelInfo->m_nVehicleType == VEHICLE_QUAD ||
            vehicle->m_nModelIndex == QUADBIKE_MODEL_ID;
        const bool isMotorcycle = modelInfo->m_nVehicleType == VEHICLE_BIKE && !isQuadBike;
        if (!isRoadVehicle && !isMotorcycle && !isQuadBike)
            return;

        if (isRoadVehicle)
        {
            CAutomobile* automobile = static_cast<CAutomobile*>(vehicle);
            if (automobile->m_damageManager.GetLightStatus(LIGHT_REAR_LEFT) != 0 &&
                automobile->m_damageManager.GetLightStatus(LIGHT_REAR_RIGHT) != 0)
                return;
        }

        const CVector& forward = vehicle->m_matrix->up;
        const CVector pointLightPosition(
            vehicle->m_matrix->pos.x - 4.0f * forward.x,
            vehicle->m_matrix->pos.y - 4.0f * forward.y,
            vehicle->m_matrix->pos.z - 4.0f * forward.z);
        const bool braking = vehicle->m_fBreakPedal > 0.0f;
        CPointLights::AddLight(
            PLTYPE_POINTLIGHT, pointLightPosition, CVector(0.0f, 0.0f, 0.0f),
            braking ? 10.0f : 7.0f,
            braking ? 1.0f : 0.6f, 0.0f, 0.0f,
            0, false, nullptr);
    }

    void ProcessRearGroundProjectionForVehicle(CVehicle* vehicle)
    {
        if (s_RearProjectionStyle == 0 || !vehicle || !vehicle->m_matrix)
            return;

        CVehicleModelInfo* modelInfo = static_cast<CVehicleModelInfo*>(
            CModelInfo::GetModelInfo(vehicle->m_nModelIndex));
        if (!modelInfo || HasNoCustomRearLights(vehicle, modelInfo))
            return;

        const bool isRoadVehicle =
            modelInfo->m_nVehicleType == VEHICLE_AUTOMOBILE ||
            modelInfo->m_nVehicleType == VEHICLE_MTRUCK;
        const bool isPedalBicycle = IsPedalBicycleModel(vehicle->m_nModelIndex) ||
            modelInfo->m_nVehicleType == VEHICLE_BMX;
        const bool isHarvester = vehicle->m_nModelIndex == HARVESTER_MODEL_ID;
        const bool vehicleIsExiting = IsVehicleInExitTransition(vehicle);
        const bool isReversing = IsVehicleReversing(vehicle);
        const bool showWhiteReverseLights = isRoadVehicle && !isHarvester &&
            isReversing && s_WhiteReverseLightsEnabled != 0 && !vehicleIsExiting;

        const CPed* driver = vehicle->m_pDriver;
        const bool driverActuallyInVehicle = driver && driver->bInVehicle &&
            driver->m_pVehicle == vehicle &&
            driver->m_ePedState != PEDSTATE_EXIT_CAR &&
            !driver->bHasJustLeftCar;
        const bool driverActivelyBraking = driverActuallyInVehicle &&
            !vehicleIsExiting && vehicle->m_fBreakPedal > 0.05f;

        // This pass also visits vehicles whose bodies are outside the camera frustum.
        // Do not use m_renderLights here: the game updates those render-only flags in
        // the entity render path, which would reintroduce view-angle dependent culling.
        // Keep the projection while the lamp switch is genuinely on, even during a
        // dismount animation. Only the brake-only path is blocked during exit so a
        // stale brake-pedal value cannot create a daytime projection.
        const bool rearLampsPhysicallyLit = !isPedalBicycle && !isHarvester &&
            (vehicle->bLightsOn || driverActivelyBraking);

        CVector localRearLightPosition(
            -DEFAULT_HEADLIGHT_X, DEFAULT_REAR_LIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector rearLeftLightPosition, rearRightLightPosition;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_REAR_LIGHTS,
                                 localRearLightPosition,
                                 rearLeftLightPosition, rearRightLightPosition);

        RegisterRearGroundLighting(vehicle, modelInfo,
                                   rearLampsPhysicallyLit, showWhiteReverseLights,
                                   rearLeftLightPosition, rearRightLightPosition);
    }

    // Run before the scene render and walk the vehicle pool instead of relying on
    // vehicleRenderEvent. The projection itself is then filtered by distance only,
    // not whether the vehicle body was selected for rendering from the current angle.
    void ProcessRearGroundProjections()
    {
        if (s_RearProjectionStyle == 0 || !TheCamera.m_pRwCamera ||
            !CPools::ms_pVehiclePool)
            return;

        const unsigned int frame = CTimer::m_FrameCounter;
        if (s_RearProjectionPassFrame == frame)
            return;

        s_RearProjectionPassFrame = frame;
        s_RearProjectionShadowCount = 0;

        struct VehicleCandidate
        {
            CVehicle* vehicle;
            float distanceSquared;
        };

        const CVector& cameraPosition = TheCamera.m_mCameraMatrix.pos;
        const float searchDistance = std::max(15.0f, s_RearProjectionDrawDistance) + 10.0f;
        const float searchDistanceSquared = searchDistance * searchDistance;
        std::vector<VehicleCandidate> candidates;
        candidates.reserve(32);

        auto* vehiclePool = CPools::ms_pVehiclePool;
        for (int index = 0; index < vehiclePool->m_nSize; ++index)
        {
            CVehicle* vehicle = vehiclePool->GetAt(index);
            if (!vehicle || !vehicle->m_matrix)
                continue;

            const CVector& position = vehicle->m_matrix->pos;
            const float dx = position.x - cameraPosition.x;
            const float dy = position.y - cameraPosition.y;
            const float dz = position.z - cameraPosition.z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared >= searchDistanceSquared)
                continue;

            candidates.push_back({ vehicle, distanceSquared });
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const VehicleCandidate& lhs, const VehicleCandidate& rhs)
            {
                return lhs.distanceSquared < rhs.distanceSquared;
            });

        // CShadows has a fixed 48-entry per-frame buffer. Limit custom rear-light
        // projection entries (not vehicles), leaving room for native world shadows.
        for (const VehicleCandidate& candidate : candidates)
        {
            if (s_RearProjectionShadowCount >= MAX_REAR_PROJECTION_SHADOWS_PER_FRAME)
                break;
            ProcessRearGroundProjectionForVehicle(candidate.vehicle);
        }
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
        const int modelId = vehicle->m_nModelIndex;
        const bool isRoadVehicle = modelInfo &&
            (modelInfo->m_nVehicleType == VEHICLE_AUTOMOBILE ||
             modelInfo->m_nVehicleType == VEHICLE_MTRUCK);
        const bool isBike = modelInfo && modelInfo->m_nVehicleType == VEHICLE_BIKE;
        const bool isQuadBike =
            (modelInfo && modelInfo->m_nVehicleType == VEHICLE_QUAD) ||
            modelId == QUADBIKE_MODEL_ID;
        const bool isPedalBicycle = IsPedalBicycleModel(modelId) ||
            (modelInfo && modelInfo->m_nVehicleType == VEHICLE_BMX);
        const bool isHarvester = modelId == HARVESTER_MODEL_ID;
        const bool vehicleIsExiting = IsVehicleInExitTransition(vehicle);
        // Trucks and construction vehicles are classed as MTRUCK by the game;
        // allow the same optional white reverse streaks as other road vehicles.
        const bool showWhiteReverseLights =
            isRoadVehicle && !isHarvester && isReversing &&
            s_WhiteReverseLightsEnabled != 0 && !vehicleIsExiting;

        // A spawned/started vehicle may retain rear render flags while its lights
        // are switched off. Require the light switch for steady tail-lamp projection.
        // During an exit animation the render-only flags can clear early; the actual
        // light switch remains authoritative so illuminated lamps do not pop out.
        const bool rearLampRenderFlagsActive = AreRearLampRenderFlagsActive(vehicle);
        const bool tailLampsSwitchedOn = vehicle->bLightsOn &&
            (rearLampRenderFlagsActive || !isRoadVehicle || vehicleIsExiting);
        const CPed* driver = vehicle->m_pDriver;
        const bool driverActuallyInVehicle = driver && driver->bInVehicle &&
            driver->m_pVehicle == vehicle &&
            driver->m_ePedState != PEDSTATE_EXIT_CAR &&
            !driver->bHasJustLeftCar;
        const bool driverActivelyBraking = driverActuallyInVehicle &&
            !vehicleIsExiting && vehicle->m_fBreakPedal > 0.05f;
        // Do not blanket-disable rear effects during dismount: steady lamps can
        // remain on. The brake-only path is already guarded by driverActivelyBraking.
        const bool rearLampsPhysicallyLit = !isPedalBicycle && !isHarvester &&
            (tailLampsSwitchedOn || driverActivelyBraking);
        const float reverseBrightnessMultiplier = (isBike || isQuadBike)
            ? Clamp01(s_BikeReverseBrightnessMultiplier) : 1.0f;

        CVector localRearLightPosition(-DEFAULT_HEADLIGHT_X, DEFAULT_REAR_LIGHT_Y, DEFAULT_HEADLIGHT_Z);
        CVector rearLeftLightPosition, rearRightLightPosition;
        GetVehicleLightPositions(vehicle, modelInfo, DUMMY_REAR_LIGHTS,
                                 localRearLightPosition, rearLeftLightPosition, rearRightLightPosition);

        // Ground projections are registered by ProcessRearGroundProjections from the
        // vehicle pool; this render callback only updates screen-space streaks/coronas.
        RegisterPlayerRearPointLight(vehicle, modelInfo, rearLampsPhysicallyLit);

        if (vehicle->bLightsOn)
        {
            // Use horizontal camera direction for streak visibility and size so
            // camera elevation cannot distort the response. Keep a short streak
            // visible at broadside and smoothly fade it out beyond that angle.
            const float streakAngleFadeEnd = std::max(
                s_FrontAngleFadeEnd, HEADLIGHT_STREAK_SIDE_FADE_END);
            const float streakBehindness = GetCameraHorizontalBehindness(vehicle);
            const float frontVisibilityFade = 1.0f - SmootherStep(
                s_FrontAngleFadeStart, streakAngleFadeEnd, streakBehindness);
            const float rawIntensity = -0.5f * streakBehindness + 0.3f;
            const float baseIntensity = Clamp01(rawIntensity);
            // A single fade avoids multiplying the same angle attenuation twice.
            const float intensity = baseIntensity * frontVisibilityFade;
            const bool isMotorcycle = IsMotorcycleModel(vehicle, modelInfo);
            if (isMotorcycle)
            {
                const bool frontLampRenderActive =
                    vehicle->m_renderLights.m_bLeftFront || vehicle->m_renderLights.m_bRightFront;
                const bool frontLampWorking = (leftLightWorking || rightLightWorking) &&
                    frontLampRenderActive;
                if (frontLampWorking)
                {
                    const float lampFacing = GetStreakFacingFactor(vehicle, leftLightPosition, false);
                    UpdateStreakHistory(baseCoronaId + 0x71u, leftLightPosition, colour, intensity,
                                        false, lampFacing, 1.0f, false);
                }
                else
                    ClearStreakHistory(baseCoronaId + 0x71u);
                ClearStreakHistory(baseCoronaId + 0x72u);

                const float coreAngleFade = 1.0f - SmootherStep(
                    s_FrontCoreAngleFadeStart, s_FrontCoreAngleFadeEnd, cameraBehindness);
                const float coreFade = coreAngleFade * coreDistanceFade;
                if (frontLampWorking && coreFade > 0.001f)
                    RegisterHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, colour, 1.2f, coronaAngle, coreFade);
                else
                    HideHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, coronaAngle);
                HideHeadlightCore(baseCoronaId + 0x74u, rightLightPosition, coronaAngle);
            }
            else
            {
                // Match damage state and GTA's native per-lamp render flags to each dummy.
                const bool firstLampIsVehicleLeft = localLightPosition.x < 0.0f;
                bool firstLightWorking = firstLampIsVehicleLeft
                    ? leftLightWorking : rightLightWorking;
                bool secondLightWorking = firstLampIsVehicleLeft
                    ? rightLightWorking : leftLightWorking;

                const bool firstLampRenderActive = firstLampIsVehicleLeft
                    ? vehicle->m_renderLights.m_bLeftFront
                    : vehicle->m_renderLights.m_bRightFront;
                const bool secondLampRenderActive = firstLampIsVehicleLeft
                    ? vehicle->m_renderLights.m_bRightFront
                    : vehicle->m_renderLights.m_bLeftFront;

                firstLightWorking = firstLightWorking && firstLampRenderActive;
                secondLightWorking = secondLightWorking && secondLampRenderActive;

                // The model dummy can be on either side. Keep each world position,
                // damage flag, and corona/history ID together as one physical source.
                const CVector& firstLampPosition = leftLightPosition;
                const CVector& secondLampPosition = rightLightPosition;
                const unsigned int firstStreakId = baseCoronaId + 0x71u;
                const unsigned int secondStreakId = baseCoronaId + 0x72u;
                const unsigned int firstCoreId = baseCoronaId + 0x73u;
                const unsigned int secondCoreId = baseCoronaId + 0x74u;

                if (firstLightWorking)
                {
                    const float firstLampFacing = GetStreakFacingFactor(vehicle, firstLampPosition, false);
                    UpdateStreakHistory(firstStreakId, firstLampPosition, colour, intensity,
                                        false, firstLampFacing, 1.0f, false);
                }
                else
                {
                    ClearStreakHistory(firstStreakId);
                }

                if (secondLightWorking)
                {
                    const float secondLampFacing = GetStreakFacingFactor(vehicle, secondLampPosition, false);
                    UpdateStreakHistory(secondStreakId, secondLampPosition, colour, intensity,
                                        false, secondLampFacing, 1.0f, false);
                }
                else
                {
                    ClearStreakHistory(secondStreakId);
                }

                // Keep the round core's angle and distance fades narrower than the streak.
                const float coreAngleFade = 1.0f - SmootherStep(
                    s_FrontCoreAngleFadeStart, s_FrontCoreAngleFadeEnd, cameraBehindness);
                const float coreFade = coreAngleFade * coreDistanceFade;

                if (firstLightWorking && coreFade > 0.001f)
                    RegisterHeadlightCore(firstCoreId, firstLampPosition, colour, 1.2f, coronaAngle, coreFade);
                else
                    HideHeadlightCore(firstCoreId, firstLampPosition, coronaAngle);

                if (secondLightWorking && coreFade > 0.001f)
                    RegisterHeadlightCore(secondCoreId, secondLampPosition, colour, 1.2f, coronaAngle, coreFade);
                else
                    HideHeadlightCore(secondCoreId, secondLampPosition, coronaAngle);
            }
        }
        else
        {
            ClearStreakHistory(baseCoronaId + 0x71u);
            ClearStreakHistory(baseCoronaId + 0x72u);
            HideHeadlightCore(baseCoronaId + 0x73u, leftLightPosition, coronaAngle);
            HideHeadlightCore(baseCoronaId + 0x74u, rightLightPosition, coronaAngle);
        }

        if (isPedalBicycle || isHarvester)
        {
            // Pedal bicycles have no custom rear effects; the Combine Harvester
            // has no physical rear lamps, so hide red and white custom effects too.
            HideRearLightEffects(baseCoronaId, coronaAngle,
                                 rearLeftLightPosition, rearRightLightPosition);
        }
        else
        {
            // Keep stock rear lamps and add the custom screen-space layer.
            // When white reverse lights are disabled, retain normal red tail-light effects.
            const bool singleRearLamp = IsSingleRearLampModel(vehicle, modelInfo);
            ProcessRearLights(vehicle, showWhiteReverseLights, isBike, singleRearLamp,
                              reverseBrightnessMultiplier, cameraBehindness, cameraDistance,
                              baseCoronaId, coronaAngle, rearLeftLightPosition, rearRightLightPosition);

            // Optional white reverse cores are independent of the headlight switch.
            ProcessReverseLights(showWhiteReverseLights, isBike, reverseBrightnessMultiplier, cameraBehindness,
                                 coreDistanceFade, baseCoronaId, coronaAngle,
                                 rearLeftLightPosition, rearRightLightPosition);
        }
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
        s_WhiteReverseLightsEnabled = 1;
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
        s_RearProjectionStyle = 2;
        s_RearProjectionDrawDistance = 60.0f;
        s_CarRearProjectionOffset = 0.20f;
        s_SingleRearProjectionOffset = 0.20f;
        s_SingleRearProjectionModelIds.clear();
        s_RearStreakVisibilityStyle = 0;
        s_StreakDistanceFadeStart = 25.0f;
        s_StreakDrawDistance = 40.0f;
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
        static const char* rearProjectionStyleStrings[] = { "Off", "Vice City", "LCS" };
        DebugMenuEntry* rearProjectionStyleEntry = DebugMenuAddVar(
            "SA Streaks|Rear", "Ground projection style",
            &s_RearProjectionStyle, nullptr, 1, 0, 2, rearProjectionStyleStrings);
        if (rearProjectionStyleEntry)
            DebugMenuEntrySetWrap(rearProjectionStyleEntry, true);

        static const char* rearStreakVisibilityStrings[] = { "Distance only", "Visible nearby (VC)" };
        DebugMenuEntry* rearStreakVisibilityEntry = DebugMenuAddVar(
            "SA Streaks|Rear", "Streak visibility style",
            &s_RearStreakVisibilityStyle, nullptr, 1, 0, 1, rearStreakVisibilityStrings);
        if (rearStreakVisibilityEntry)
            DebugMenuEntrySetWrap(rearStreakVisibilityEntry, true);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry start", &s_RearRoundAngleEntryStart,
                        nullptr, 0.02f, -0.50f, 0.10f);
        DebugMenuAddVar("SA Streaks|Rear", "Round entry end", &s_RearRoundAngleEntryEnd,
                        nullptr, 0.01f, 0.11f, 0.19f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit start", &s_RearRoundAngleExitStart,
                        nullptr, 0.02f, 0.20f, 0.70f);
        DebugMenuAddVar("SA Streaks|Rear", "Round exit end", &s_RearRoundAngleExitEnd,
                        nullptr, 0.02f, 0.71f, 0.99f);

        DebugMenuEntry* whiteReverseLightsToggle = DebugMenuAddVar(
            "SA Streaks|Reverse", "White reverse lights",
            &s_WhiteReverseLightsEnabled, nullptr, 1, 0, 1, toggleStrings);
        if (whiteReverseLightsToggle)
            DebugMenuEntrySetWrap(whiteReverseLightsToggle, true);

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
        DebugMenuAddVar("SA Streaks|Advanced", "Streak draw distance", &s_StreakDrawDistance,
                        nullptr, 1.0f, 10.0f, 200.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Ground projection draw distance", &s_RearProjectionDrawDistance,
                        nullptr, 1.0f, 15.0f, 200.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Car projection offset", &s_CarRearProjectionOffset,
                        nullptr, 0.1f, 0.0f, 10.0f);
        DebugMenuAddVar("SA Streaks|Rear", "Single-lamp projection offset", &s_SingleRearProjectionOffset,
                        nullptr, 0.1f, 0.0f, 15.0f);

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
            plugin::Events::gameProcessEvent += ProcessRearGroundProjections;
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
