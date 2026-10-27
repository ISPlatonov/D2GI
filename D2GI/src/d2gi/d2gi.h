#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

#define NOMINMAX
#include <windows.h>

#include "d2gi_common.h"
#include "d2gi_ddraw.h"

#include "d2gi_blitter.h"
#include "d2gi_minimap.h"
#include "d2gi_strided_renderer.h"
#include "../common/frect.h"

#include <wrl/client.h>


enum RENDERSTATE
{
	RS_UNKNOWN,
	RS_BACKBUFFER_STREAMING,
	RS_PRIMARY_SURFACE_BLITTING,
	RS_BACKBUFFER_BLITTING,
	RS_3D_RENDERING,
};


class D2GISystemMemorySurface;
class D2GIPrimarySingleSurface;
class D2GIBackBufferSurface;
class D2GITexture;

struct MAT3X4;
struct FRECT;


class D2GI
{
	// Object ownership is inverted here - D2GIDirectDraw owns the outer D2GI,
	// so DO NOT touch m_pDirectDrawProxy from D2GI's destructor!
	D2GIDirectDraw* m_pDirectDrawProxy;

	D3D9::IDirect3D9* m_pD3D9;
	D3D9::IDirect3DDevice9* m_pDev;
	D3D9::IDirect3DTexture9* m_pBackBufferCopy;
	D3D9::IDirect3DSurface9* m_pBackBufferCopySurf;
	D3D9::IDirect3DSurface9* m_pDepthStencilSurf = nullptr;

	D3D9::IDirect3DSurface9* m_pMSAASurf = nullptr;

	HWND m_hWnd;
	WNDPROC m_pfnOriginalWndProc;
	DWORD m_dwOriginalWidth, m_dwOriginalHeight, m_dwOriginalBPP;

	DWORD m_dwForcedWidth, m_dwForcedHeight;
	FLOAT m_fAspectRatioScale, m_fWidthScale, m_fHeightScale;

	RENDERSTATE m_eRenderState;
	int32_t m_SceneBeginCount = 0;
	BOOL m_bColorKeyEnabled;
	D2GITexture* m_lpCurrentTextures[8];

	D2GIBlitter m_Blitter;
	D2GIStridedPrimitiveRenderer m_StridedRenderer;

	std::optional<std::pair<D3D9::D3DTEXTUREADDRESS, D3D9::D3DTEXTUREADDRESS>> m_UVOverride;

	D2GIMinimapRenderer m_MinimapRenderer;

	struct AnisotropyState {
		bool is_enabled = false;
		bool is_enabled_for_magnifying = false;
		bool is_enabled_for_minifying = false;
	} anisotropy_state_;

	uint32_t max_primitive_count_ = 0;

	// [REMIX] StaticBlitTextures=1: constant content for textures the game rewrites every frame
	std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9>> m_StaticBlitSources;
	VOID FillStaticBlitTexture(D2GITexture* pDst);

	// [REMIX] KeepDevice=1: one IDirect3D9 + device for the whole process.
	// The game recreates DirectDraw when entering/leaving 3D; RTX Remix's bridge
	// crashes intermittently when the D3D9 device is destroyed and created again.
	static D3D9::IDirect3D9* s_pKeptD3D9;
	static D3D9::IDirect3DDevice9* s_pKeptDev;
	// [REMIX] PersistentHUD=1: the game blits 2D HUD panels onto the back buffer only when
	// they change and relies on the buffer keeping them. RTX Remix builds every frame from
	// scratch, so the HUD flickers. Remember large blits and replay them on frames that skip them.
	struct PersistentBlit
	{
		Microsoft::WRL::ComPtr<D3D9::IDirect3DTexture9> pTexture;
		FRECT frtSrc, frtDst;
		bool bColorKey;
		uint32_t dwLastFrame;
	};
	std::vector<PersistentBlit> m_PersistentBlits;
	uint32_t m_dwHUDFrame = 0;
	bool m_bInMapDraw = false;
	VOID ReplayPersistentBlits();

	// [REMIX] DebugDraws=1: log every unique draw "signature" to d2gi_remix.log
	DWORD m_dbgStageState[2][32] = {};
	bool  m_dbgTexXformSet[2] = {};
	FLOAT m_dbgTexXform[2][16] = {};
	FILE* m_pRemixLog = nullptr;
	std::unordered_set<std::string> m_dbgSeenDraws;
	uint32_t m_dbgDrawCounter = 0;
	DWORD m_dwFogColor = 0;            // last D3DRENDERSTATE_FOGCOLOR, follows time of day and weather
	bool  m_bSunSetThisFrame = false;
	VOID SetRemixSun();
	uint32_t m_dbg3DFrame = 0;      // 3D frames presented, for the frame-order dump
	uint32_t m_dbgFrameDraw = 0;    // draw index inside the current frame

	// [REMIX] UntexturedColors=1: untextured draws get a 1x1 texture in their color,
	// because Remix renders geometry without an albedo texture as plain white.
	D3D7::D3DMATERIAL7 m_CurrentMaterial = {};
	DWORD m_dwDiffuseMaterialSource = 1; // D3DMCS_COLOR1
	bool  m_bInUntexturedFix = false;
	std::unordered_map<DWORD, Microsoft::WRL::ComPtr<D3D9::IDirect3DTexture9>> m_SolidColorTextures;
	std::vector<BYTE> m_UntexturedVertexScratch;
	D3D9::IDirect3DTexture9* GetSolidColorTexture(DWORD dwColor);
	BOOL IsBackdropDraw(DWORD dwFVF);
	// [REMIX] EmitCabRain: the cab frame draws drop.txr but never rain.txr. Remember the
	// world view matrix and the rain texture, and submit a few rain sheets before the gauges.
	D2GITexture* m_pRainTexture = nullptr;
	FLOAT m_WorldView[16] = {};
	bool m_bHaveWorldView = false;
	bool m_bSawDrop = false;
	bool m_bSawRain = false;
	bool m_bEmittedCabRain = false;
	VOID MaybeEmitCabRain();
	BOOL DrawUntexturedWithColor(D3D7::D3DPRIMITIVETYPE, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
		DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags, BOOL bSkyDome = FALSE);
	BOOL IsDomeShaped(DWORD dwFVF, BOOL bStrided, VOID* pVertexData, DWORD dwVertexCount);
	VOID RemixDebugLogDraw(D3D7::D3DPRIMITIVETYPE, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
		DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount);

	VOID ResetD3D9Device();
	void ReleaseResources(bool bResettingDevice);
	void LoadResources(bool bResettingDevice);
	VOID TryBeginScene();
	VOID TryEndScene();
	VOID Present();
	VOID DrawPrimitive(D3D7::D3DPRIMITIVETYPE, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
		DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags);
	VOID ScaleFRect(FRECT* pSrc, FRECT* pOut);
	VOID ScaleRect(RECT* pSrc, RECT* pOut);
	static LRESULT CALLBACK WndProc_Static(HWND, UINT, WPARAM, LPARAM);
	LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
	VOID AttachWndProc();
	VOID DetachWndProc();
	VOID ScaleD3D9Rect(const D3D7::D3DRECT* pSrc, D3D9::D3DRECT* pOut);
	VOID SetupWindow();
	void SynchronizeWithDeviceCaps();
	void BlitToBackbufferAndPresent(FRECT* backbuffer_rect, D3D9::IDirect3DTexture9* texture, FRECT* texture_rect);

	// Scoped Begin/EndScene for internal use (+ minimap)
	class SceneScope
	{
	public:
		SceneScope(D2GI* pD2GI)
			: m_pD2GI(pD2GI)
		{
			pD2GI->TryBeginScene();
		}

		~SceneScope()
		{
			m_pD2GI->TryEndScene();
		}

	private:
		D2GI* const m_pD2GI;
	};

public:
	D2GI();
	~D2GI();

	D2GIDirectDraw* GetDirectDrawProxy() const { return m_pDirectDrawProxy; }
	D3D9::IDirect3D9* GetD3D9() const { return m_pD3D9; }
	D3D9::IDirect3DDevice9* GetD3D9Device() const { return m_pDev; }
	DWORD GetOriginalWidth() const { return m_dwOriginalWidth; }
	DWORD GetOriginalHeight() const { return m_dwOriginalHeight; }
	DWORD GetOriginalBPP() const { return m_dwOriginalBPP; }
	DWORD GetForcedWidth() const { return m_dwForcedWidth; }
	DWORD GetForcedHeight() const { return m_dwForcedHeight; }

	SceneScope BeginSceneScope() { return SceneScope(this); }

	uint32_t GetMaxPrimitiveCount() const { return max_primitive_count_; }

	D3D9::IDirect3DSurface9* GetBackBufferCopySurface() const { return m_pBackBufferCopySurf; }
	Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> GetScreenshotSource() const;

	void EnableUVOverride(D3D9::D3DTEXTUREADDRESS AddressU, D3D9::D3DTEXTUREADDRESS AddressV) { m_UVOverride.emplace(AddressU, AddressV); }
	void DisableUVOverride() { m_UVOverride.reset(); }

	void OnMapDrawSetViewport(const RECT& viewport) { m_MinimapRenderer.SetViewport(viewport); }
	void SetInMapDraw(bool bInMapDraw) { m_bInMapDraw = bInMapDraw; }
	void RememberRainTexture(D2GITexture* pTex) { if (pTex != nullptr) m_pRainTexture = pTex; }
	void OnBeginMinimapDraw();
	void OnEndMinimapDraw();
	void OnAddMinimapLine(float x1, float y1, float x2, float y2, DWORD color) { m_MinimapRenderer.AddMinimapLine(x1, y1, x2, y2, color); }

	void OnDrawBridgeLightingPrimitive(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, LPVOID pVerts, DWORD dwVertCount, DWORD dwFlags);

	VOID OnDirectDrawReleased();
	VOID OnCooperativeLevelSet(HWND, DWORD);
	VOID OnDisplayModeSet(DWORD, DWORD, DWORD, DWORD dwFlags);
	VOID OnViewportSet(D3D7::LPD3DVIEWPORT7);
	void OnFlip();
	VOID OnBackBufferLock(BOOL bRead);
	void OnSystemMemorySurfaceBlitOnPrimarySingle(D2GISystemMemorySurface*, RECT*, D2GIPrimarySingleSurface*, RECT*);
	VOID OnClear(DWORD dwCount, D3D7::LPD3DRECT lpRects, DWORD dwFlags, D3D7::D3DCOLOR col, D3D7::D3DVALUE z, DWORD dwStencil);
	VOID OnLightEnable(DWORD, BOOL);
	VOID OnSysMemSurfaceBltOnBackBuffer(D2GISystemMemorySurface*, RECT*, D2GIBackBufferSurface*, RECT*);
	VOID OnSysMemSurfaceBltOnTexture(D2GISystemMemorySurface*, RECT*, D2GITexture*, RECT*);
	void OnSceneBegin();
	void OnSceneEnd();
	VOID OnRenderStateSet(D3D7::D3DRENDERSTATETYPE, DWORD);
	VOID OnTextureStageSet(DWORD, D3D7::D3DTEXTURESTAGESTATETYPE, DWORD);
	VOID OnTextureSet(DWORD, D2GITexture*);
	BOOL OnDeviceValidate(DWORD*);
	VOID OnTransformSet(D3D7::D3DTRANSFORMSTATETYPE, D3D7::LPD3DMATRIX);
	VOID OnLightSet(DWORD, D3D7::LPD3DLIGHT7);
	VOID OnMaterialSet(D3D7::LPD3DMATERIAL7);
	VOID OnClipStatusSet(D3D7::LPD3DCLIPSTATUS);
	VOID OnIndexedPrimitiveStridedDraw(D3D7::D3DPRIMITIVETYPE, DWORD, D3D7::LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD);
	VOID OnPrimitiveStridedDraw(D3D7::D3DPRIMITIVETYPE, DWORD, D3D7::LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD);
	VOID OnPrimitiveDraw(D3D7::D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD);
	VOID OnIndexedPrimitiveDraw(D3D7::D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD);
	BOOL OnRenderStateGet(D3D7::D3DRENDERSTATETYPE, DWORD*);
	VOID OnTransformsSetup(VOID* pThis, MAT3X4* pmView, MAT3X4* pmProj);
	std::vector<D3D9::D3DDISPLAYMODE> GetDisplayModes();
	VOID OnDisplayModeEnum(LPVOID pArg, D3D7::LPDDENUMMODESCALLBACK2 pCallback);

public:
	// These are public and static so the assembly hooks can write here easily.
	static float m_LastBltX, m_LastBltY;
};
