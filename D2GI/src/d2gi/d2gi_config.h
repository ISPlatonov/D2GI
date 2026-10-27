#pragma once


#include <cstdint>
#include <string>

enum class WINDOWMODE
{
	WINDOWED,
	BORDERLESS,
	FULLSCREEN,
};

enum class IMG_FORMAT
{
	IMG_PNG,
	IMG_JPG,
	IMG_BMP,
};

struct HookOptions
{
	bool m_bEnableHooks = true;
	bool m_bEnableUIHooks = true;
	bool m_bEnableMirrorsHooks = true;
	bool m_bEnableAffinityHooks = true;
};

class D2GIConfig
{
	static WINDOWMODE s_eWindowMode;
	static uint32_t   s_dwVideoWidth, s_dwVideoHeight;
	static uint32_t	  s_AnisotropyLevel, s_MSAALevel;
	static bool       s_bEnableVSync;
	static bool       s_bFixAlpha;
	static wchar_t    s_cScreenshotsPath[MAX_PATH];
	static IMG_FORMAT s_eImgFormat;
	static bool       s_bRemixDebugDraws;
	static bool       s_bRemixUntexturedColors;
	static bool       s_bRemixKeepDevice;
	static bool       s_bRemixPersistentHUD;
	static bool       s_bRemixManagedTextures;
	static bool       s_bRemixStaticBlitTextures;
	static bool       s_bRemixClearWindshield;
	static bool       s_bRemixSkipBackdrop;
	static bool       s_bRemixEmitCabRain;
	static int        s_nRemixSunStrength;
	static bool       s_bRemixSkyDome;
public:
	static HookOptions ReadFromFile();

	static WINDOWMODE GetWindowMode() { return s_eWindowMode; }
	static uint32_t GetVideoWidth();
	static uint32_t GetVideoHeight();
	static uint32_t AnisotropyLevel() { return s_AnisotropyLevel; }
	static uint32_t MSAALevel() { return s_MSAALevel; }
	static bool VSyncEnabled() { return s_bEnableVSync; }
	static bool FixAlphaEnabled() { return s_bFixAlpha; }
	static wchar_t* GetScreenshotsPath() { return s_cScreenshotsPath; }
	static IMG_FORMAT GetScreenshotsFormat() { return s_eImgFormat; }
	static bool RemixDebugDrawsEnabled() { return s_bRemixDebugDraws; }
	static bool RemixUntexturedColorsEnabled() { return s_bRemixUntexturedColors; }
	static bool RemixKeepDeviceEnabled() { return s_bRemixKeepDevice; }
	static bool RemixPersistentHUDEnabled() { return s_bRemixPersistentHUD; }
	static bool RemixManagedTexturesEnabled() { return s_bRemixManagedTextures; }
	static bool RemixStaticBlitTexturesEnabled() { return s_bRemixStaticBlitTextures; }
	static bool RemixClearWindshieldEnabled() { return s_bRemixClearWindshield; }
	static bool RemixSkipBackdropEnabled() { return s_bRemixSkipBackdrop; }
	static bool RemixEmitCabRainEnabled() { return s_bRemixEmitCabRain; }
	static int RemixSunStrength() { return s_nRemixSunStrength; }
	static bool RemixSkyDomeEnabled() { return s_bRemixSkyDome; }

	static std::basic_string<TCHAR> GetConfigFilePath();
};
