
#include "../common/common.h"
#include "../common/utils.h"
#include "../common/m3x4.h"
#include "../common/frect.h"
#include "../common/logger.h"
#include "../common/dir.h"

#include "d2gi.h"
#include "d2gi_ddraw.h"
#include "d2gi_prim_flip_surf.h"
#include "d2gi_backbuf_surf.h"
#include "d2gi_blitter.h"
#include "d2gi_prim_single_surf.h"
#include "d2gi_sysmem_surf.h"
#include "d2gi_texture.h"
#include "d2gi_strided_renderer.h"
#include "d2gi_config.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <cmath>
#include <string>

#define XXH_INLINE_ALL
#include "../thirdparty/xxhash/xxhash.h"

// Light slot reserved for the Remix sun (SunStrength); the game itself was seen using slots 0-1 and 9-10.
static const DWORD REMIX_SUN_LIGHT_INDEX = 7;
// Headlight spot lights use slots REMIX_HEADLIGHT_BASE.. (two per vehicle); the game only uses slots below 100.
static const DWORD REMIX_HEADLIGHT_BASE = 100;
static const int   REMIX_MAX_LIT_VEHICLES = 16;

#include <shlwapi.h>
#include <windowsx.h>

#pragma comment(lib, "d3d9.lib")

float D2GI::m_LastBltX, D2GI::m_LastBltY;
D3D9::IDirect3D9* D2GI::s_pKeptD3D9 = nullptr;
D3D9::IDirect3DDevice9* D2GI::s_pKeptDev = nullptr;

D2GI::D2GI()
	: m_pD3D9(nullptr), m_Blitter(this), m_StridedRenderer(this), m_MinimapRenderer(this)
{
	m_pDev = NULL;

	if (s_pKeptD3D9 != nullptr)
	{
		// Take over the interface and device kept by the previous D2GI instance
		m_pD3D9 = s_pKeptD3D9;
		m_pDev = s_pKeptDev;
		s_pKeptD3D9 = nullptr;
		s_pKeptDev = nullptr;
		Logger::Log(TEXT("Reusing kept D3D9 device"));
	}
	else
	{
		m_pD3D9 = D3D9::Direct3DCreate9(D3D_SDK_VERSION);
	}

	if (m_pD3D9 == nullptr)
		Logger::Error(TEXT("Failed to obtain IDirect3D9 interface"));

	m_pBackBufferCopy = NULL;
	m_pBackBufferCopySurf = NULL;
	m_pfnOriginalWndProc = NULL;

	m_eRenderState = RS_UNKNOWN;
	m_bColorKeyEnabled = FALSE;

	ZeroMemory(m_lpCurrentTextures, sizeof(m_lpCurrentTextures));
	m_pRainTexture = nullptr;
	m_bHaveWorldView = false;
	m_bSawDrop = m_bSawRain = m_bEmittedCabRain = false;

	// Direct3D default texture stage states (for draw logging)
	m_dbgStageState[0][D3D7::D3DTSS_COLOROP] = D3D7::D3DTOP_MODULATE;
	m_dbgStageState[1][D3D7::D3DTSS_COLOROP] = D3D7::D3DTOP_DISABLE;
	m_dbgStageState[0][D3D7::D3DTSS_ALPHAOP] = D3D7::D3DTOP_SELECTARG1;
	m_dbgStageState[1][D3D7::D3DTSS_ALPHAOP] = D3D7::D3DTOP_DISABLE;
	m_dbgStageState[0][D3D7::D3DTSS_TEXCOORDINDEX] = 0;
	m_dbgStageState[1][D3D7::D3DTSS_TEXCOORDINDEX] = 1;
	for (int i = 0; i < 2; i++)
	{
		m_dbgStageState[i][D3D7::D3DTSS_COLORARG1] = D3DTA_TEXTURE;
		m_dbgStageState[i][D3D7::D3DTSS_COLORARG2] = D3DTA_CURRENT;
		m_dbgStageState[i][D3D7::D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
		m_dbgStageState[i][D3D7::D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
	}

	m_pDirectDrawProxy = new D2GIDirectDraw(this);
}


D2GI::~D2GI()
{
	ReleaseResources(/*bResettingDevice=*/ false);

	DetachWndProc();

	if (D2GIConfig::RemixKeepDeviceEnabled() && m_pDev != nullptr && m_pD3D9 != nullptr)
	{
		// Leave the device clean for the next D2GI instance
		if (m_SceneBeginCount > 0)
		{
			m_pDev->EndScene();
			m_SceneBeginCount = 0;
		}
		for (DWORD i = 0; i < 8; i++)
			m_pDev->SetTexture(i, nullptr);
		m_pDev->SetStreamSource(0, nullptr, 0, 0);
		m_pDev->SetIndices(nullptr);
		m_pDev->SetVertexShader(nullptr);
		m_pDev->SetPixelShader(nullptr);
		{
			// Unbind our own render target / depth surfaces so the next Reset() succeeds
			D3D9::IDirect3DSurface9* pBackBuffer = nullptr;
			if (SUCCEEDED(m_pDev->GetBackBuffer(0, 0, D3D9::D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
			{
				m_pDev->SetRenderTarget(0, pBackBuffer);
				pBackBuffer->Release();
			}
			m_pDev->SetDepthStencilSurface(nullptr);
		}

		RELEASE(s_pKeptDev);
		RELEASE(s_pKeptD3D9);
		s_pKeptDev = m_pDev;
		s_pKeptD3D9 = m_pD3D9;
		m_pDev = nullptr;
		m_pD3D9 = nullptr;
		Logger::Log(TEXT("Keeping D3D9 device for the next DirectDraw instance"));
		return;
	}

	RELEASE(m_pDev);
	RELEASE(m_pD3D9);
}


Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> D2GI::GetScreenshotSource() const
{
	Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> result;
	if (m_eRenderState == RS_BACKBUFFER_STREAMING)
	{
		D2GIPrimaryFlippableSurface* pPrimSurf = m_pDirectDrawProxy->GetPrimaryFlippableSurface();
		result = pPrimSurf->GetBackBufferSurface()->GetD3D9WritingSurface();
	}
	else if (m_eRenderState == RS_BACKBUFFER_BLITTING || m_eRenderState == RS_3D_RENDERING)
	{
		m_pDev->GetRenderTarget(0, result.GetAddressOf());
	}

	return result;
}

void D2GI::OnBeginMinimapDraw()
{
	m_eRenderState = RS_3D_RENDERING;
	m_MinimapRenderer.BeginMinimapDraw();
}

void D2GI::OnEndMinimapDraw()
{
	m_eRenderState = RS_3D_RENDERING;
	m_MinimapRenderer.EndMinimapDraw();
}

void D2GI::OnDrawBridgeLightingPrimitive(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, LPVOID pVerts, DWORD dwVertCount, DWORD dwFlags)
{
	// No need to reset the texture and states, other blocks set-up their materials (and those states) again.
	// Not all those states were wrong previously, but let's not rely on the state of the previous effect that was drawn.
	OnTextureSet(0, nullptr);
	m_pDev->SetRenderState(D3D9::D3DRS_DIFFUSEMATERIALSOURCE, D3D9::D3DMCS_COLOR1);
	m_dwDiffuseMaterialSource = D3D9::D3DMCS_COLOR1;
	m_pDev->SetRenderState(D3D9::D3DRS_EMISSIVEMATERIALSOURCE, D3D9::D3DMCS_COLOR1);

	m_pDev->SetRenderState(D3D9::D3DRS_FOGENABLE, TRUE);
	m_pDev->SetRenderState(D3D9::D3DRS_ZENABLE, D3D9::D3DZB_TRUE);
	m_pDev->SetRenderState(D3D9::D3DRS_ZWRITEENABLE, TRUE);
	m_pDev->SetRenderState(D3D9::D3DRS_ZFUNC, D3D9::D3DCMP_LESSEQUAL);

	DrawPrimitive(pt, dwFVF, FALSE, pVerts, dwVertCount, nullptr, 0, dwFlags);
}

VOID D2GI::OnDirectDrawReleased()
{
	delete this;
}


VOID D2GI::OnCooperativeLevelSet(HWND hWnd, DWORD dwFlags)
{
	m_hWnd = hWnd;

	Logger::SetHWND(hWnd);
	AttachWndProc();
	SetupWindow();
}


VOID D2GI::OnDisplayModeSet(DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwFlags)
{
	m_dwOriginalWidth = dwWidth;
	m_dwOriginalHeight = dwHeight;
	m_dwOriginalBPP = dwBPP;

	ResetD3D9Device();

	m_fWidthScale = (FLOAT)m_dwForcedWidth / (FLOAT)m_dwOriginalWidth;
	m_fHeightScale = (FLOAT)m_dwForcedHeight / (FLOAT)m_dwOriginalHeight;
	m_fAspectRatioScale = ((FLOAT)m_dwForcedWidth / (FLOAT)m_dwForcedHeight);
	m_fAspectRatioScale /= ((FLOAT)m_dwOriginalWidth / (FLOAT)m_dwOriginalHeight);
}


VOID D2GI::ReleaseResources(bool bResettingDevice)
{
	m_MinimapRenderer.ReleaseResources(bResettingDevice);

	m_pDirectDrawProxy->ReleaseResources(bResettingDevice);
	m_Blitter.ReleaseResource(bResettingDevice);
	m_StridedRenderer.ReleaseResource(bResettingDevice);

	RELEASE(m_pDepthStencilSurf);
	RELEASE(m_pMSAASurf);

	RELEASE(m_pBackBufferCopySurf);
	RELEASE(m_pBackBufferCopy);

	m_PersistentBlits.clear();
	m_StaticBlitSources.clear();

	m_SolidColorTextures.clear();
}


VOID D2GI::LoadResources(bool bResettingDevice)
{
	m_pDirectDrawProxy->LoadResources(bResettingDevice);
	m_Blitter.LoadResource(bResettingDevice);
	m_StridedRenderer.LoadResource(bResettingDevice);

	m_MinimapRenderer.LoadResources(bResettingDevice);
}


VOID D2GI::ResetD3D9Device()
{
	D3D9::D3DPRESENT_PARAMETERS sParams;

	if (m_pDev != nullptr)
	{
		HRESULT hr;
		while ((hr = m_pDev->TestCooperativeLevel()) == D3DERR_DEVICELOST)
			Sleep(50);

		if (FAILED(hr) && hr != D3DERR_DEVICENOTRESET)
			Logger::Error(TEXT("Failed to ready the D3D9 device for resetting"));
	}

	ReleaseResources(/*bResettingDevice=*/ true);

	ZeroMemory(&sParams, sizeof(sParams));
	sParams.AutoDepthStencilFormat     = D3D9::D3DFMT_UNKNOWN;
	sParams.EnableAutoDepthStencil     = FALSE;
	sParams.BackBufferCount            = 1;
	sParams.BackBufferWidth            = m_dwForcedWidth;
	sParams.BackBufferHeight           = m_dwForcedHeight;
	sParams.BackBufferFormat           = D3D9::D3DFMT_X8R8G8B8;
	sParams.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
	sParams.SwapEffect                 = D3D9::D3DSWAPEFFECT_FLIP;
	sParams.Windowed                   = TRUE;
	sParams.hDeviceWindow              = m_hWnd;
	sParams.PresentationInterval       =
		D2GIConfig::VSyncEnabled() ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

	if (D2GIConfig::GetWindowMode() == WINDOWMODE::FULLSCREEN)
	{
		D3D9::D3DDISPLAYMODE sDisplayMode;
		UINT uModeID, uModeCount;

		uModeCount = m_pD3D9->GetAdapterModeCount(D3DADAPTER_DEFAULT, sParams.BackBufferFormat);
		for (uModeID = 0; uModeID < uModeCount; uModeID++)
		{
			if (SUCCEEDED(m_pD3D9->EnumAdapterModes(
				D3DADAPTER_DEFAULT, sParams.BackBufferFormat, uModeID, &sDisplayMode)))
			{
				if (sDisplayMode.Width == m_dwForcedWidth
					&& sDisplayMode.Height == m_dwForcedHeight)
				{
					sParams.Windowed = FALSE;
					sParams.FullScreen_RefreshRateInHz = sDisplayMode.RefreshRate;
					break;
				}
			}
		}

		if (sParams.Windowed)
		{
			Logger::Warning(
				TEXT("Can't set fullscreen mode %ux%u, display mode not found"),
				m_dwForcedWidth, m_dwForcedHeight);
		}
	}


	if (m_pDev == nullptr) {
		if (FAILED(m_pD3D9->CreateDevice(D3DADAPTER_DEFAULT, D3D9::D3DDEVTYPE_HAL, m_hWnd,
		                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
		                                 &sParams, &m_pDev))) {
			Logger::Error(T("Failed to create D3D9 device"));
		}
	} else {
		if (FAILED(m_pDev->Reset(&sParams)))
		{
			// A kept device may refuse to reset (e.g. new window); fall back to a fresh one
			Logger::Warning(T("Failed to reset D3D9 device, creating a new one"));
			RELEASE(m_pDev);
			if (FAILED(m_pD3D9->CreateDevice(D3DADAPTER_DEFAULT, D3D9::D3DDEVTYPE_HAL, m_hWnd,
			                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
			                                 &sParams, &m_pDev))) {
				Logger::Error(T("Failed to create D3D9 device"));
			}
		}
	}

	Logger::Log(T("Setting %ux%u mode (fullscreen: %s)"), sParams.BackBufferWidth, sParams.BackBufferHeight,
	            sParams.Windowed ? T("off") : T("on"));

	SynchronizeWithDeviceCaps();

	// If MSAA is requested, find the max supported value
	D3D9::D3DMULTISAMPLE_TYPE MSAAToUse = D3D9::D3DMULTISAMPLE_NONE;

	const DWORD MSAALevel = D2GIConfig::MSAALevel();
	if (MSAALevel > 1)
	{
		for (D3D9::D3DMULTISAMPLE_TYPE MSAA = static_cast<D3D9::D3DMULTISAMPLE_TYPE>(MSAALevel); MSAA >= D3D9::D3DMULTISAMPLE_2_SAMPLES;
				MSAA = static_cast<D3D9::D3DMULTISAMPLE_TYPE>(static_cast<int>(MSAA) - 1))
		{
			if (SUCCEEDED(m_pD3D9->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3D9::D3DDEVTYPE_HAL, sParams.BackBufferFormat, sParams.Windowed,
				MSAA, nullptr)))
			{
				MSAAToUse = MSAA;
				break;
			}
		}
	}

	if (FAILED(m_pDev->CreateTexture(sParams.BackBufferWidth, sParams.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET,
		sParams.BackBufferFormat, D3D9::D3DPOOL_DEFAULT, &m_pBackBufferCopy, NULL)))
		Logger::Error(TEXT("Failed to create backbuffer copy texture"));

	if (FAILED(m_pBackBufferCopy->GetSurfaceLevel(0, &m_pBackBufferCopySurf)))
		Logger::Error(TEXT("Failed to get backbuffer copy surface"));

	if (MSAAToUse != D3D9::D3DMULTISAMPLE_NONE)
	{
		if (SUCCEEDED(m_pDev->CreateRenderTarget(sParams.BackBufferWidth, sParams.BackBufferHeight, sParams.BackBufferFormat, MSAAToUse, 0, FALSE, &m_pMSAASurf, nullptr)))
		{
			m_pDev->SetRenderState(D3D9::D3DRS_MULTISAMPLEANTIALIAS, TRUE);
			m_pDev->SetRenderState(D3D9::D3DRS_ANTIALIASEDLINEENABLE, TRUE);
			m_pDev->SetRenderTarget(0, m_pMSAASurf);
		}
		else
		{
			Logger::Error(TEXT("Failed to create the MSAA surface"));
		}
	}

	if (SUCCEEDED(m_pDev->CreateDepthStencilSurface(sParams.BackBufferWidth, sParams.BackBufferHeight, D3D9::D3DFMT_D24X8, MSAAToUse, 0, TRUE, &m_pDepthStencilSurf, nullptr)))
	{
		m_pDev->SetDepthStencilSurface(m_pDepthStencilSurf);
	}
	else
	{
		Logger::Error(TEXT("Failed to create the depth/stencil surface"));
	}

	LoadResources(/*bResettingDevice=*/ true);
}


VOID D2GI::OnViewportSet(D3D7::LPD3DVIEWPORT7 pVP)
{
	D3D9::D3DVIEWPORT9 sD3D9Viewport;
	FRECT frtVP, frtScaledVP;

	frtVP = FRECT((FLOAT)pVP->dwX, (FLOAT)pVP->dwY,
		(FLOAT)(pVP->dwX + pVP->dwWidth), (FLOAT)(pVP->dwY + pVP->dwHeight));
	ScaleFRect(&frtVP, &frtScaledVP);

	// ugly piece of shit
	if (pVP->dwX != 0 && pVP->dwY != 0 
		&& pVP->dwWidth != m_dwOriginalWidth && pVP->dwHeight != m_dwOriginalHeight)
	{
		frtScaledVP.fLeft = std::max(0.0f, std::floor(frtScaledVP.fLeft - 0.5f));
		frtScaledVP.fTop = std::max(0.0f, std::floor(frtScaledVP.fTop - 0.5f));
		frtScaledVP.fRight = std::min((FLOAT)m_dwForcedWidth, std::ceil(frtScaledVP.fRight + 0.5f));
		frtScaledVP.fBottom = std::min((FLOAT)m_dwForcedWidth, std::ceil(frtScaledVP.fBottom + 0.5f));
	}

	sD3D9Viewport.X = (DWORD)frtScaledVP.fLeft;
	sD3D9Viewport.Y = (DWORD)frtScaledVP.fTop;
	sD3D9Viewport.Width = (DWORD)(frtScaledVP.GetWidth());
	sD3D9Viewport.Height = (DWORD)(frtScaledVP.GetHeight());
	sD3D9Viewport.MinZ = pVP->dvMinZ;
	sD3D9Viewport.MaxZ = pVP->dvMaxZ;
	m_pDev->SetViewport(&sD3D9Viewport);
}


VOID D2GI::OnBackBufferLock(BOOL bRead)
{
	if(bRead)
	{
		auto SceneScope = BeginSceneScope();

		D2GIBackBufferSurface* pBackBuf = m_pDirectDrawProxy->GetPrimaryFlippableSurface()->GetBackBufferSurface();

		Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> pRT;
		m_pDev->GetRenderTarget(0, pRT.GetAddressOf());
		m_pDev->StretchRect(pRT.Get(), NULL, m_pBackBufferCopySurf, NULL, D3D9::D3DTEXF_LINEAR);
		m_Blitter.Blit(pBackBuf->GetD3D9ReadingSurface(), NULL, m_pBackBufferCopy, NULL, FALSE);
	}else
		m_eRenderState = RS_BACKBUFFER_STREAMING;
}

void D2GI::OnFlip()
{
	if (m_eRenderState == RS_BACKBUFFER_STREAMING) {
		m_PersistentBlits.clear(); // menus and loading screens redraw everything themselves
		D3D9::IDirect3DTexture9* backbuffer_texture = m_pDirectDrawProxy->GetPrimaryFlippableSurface()
			->GetBackBufferSurface()
			->GetD3D9WritingTexture();

		BlitToBackbufferAndPresent(nullptr, backbuffer_texture, nullptr);
	} else if (m_eRenderState == RS_BACKBUFFER_BLITTING || m_eRenderState == RS_3D_RENDERING) {
		m_bSunSetThisFrame = false;
		if (D2GIConfig::RemixDebugDrawsEnabled())
		{
			m_dbg3DFrame++;
			m_dbgFrameDraw = 0;
		}
		if (m_pMSAASurf)
		{
			Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> pRT;
			m_pDev->GetBackBuffer(0, 0, D3D9::D3DBACKBUFFER_TYPE_MONO, pRT.GetAddressOf());
			m_pDev->StretchRect(m_pMSAASurf, NULL, pRT.Get(), NULL, D3D9::D3DTEXF_NONE);
		}
		if (D2GIConfig::RemixPersistentHUDEnabled())
			ReplayPersistentBlits();
		Present();
	}
}

void D2GI::OnSystemMemorySurfaceBlitOnPrimarySingle(D2GISystemMemorySurface* source, RECT* source_rect,
                                                    D2GIPrimarySingleSurface* destination, RECT* destination_rect)
{
	m_eRenderState = RS_PRIMARY_SURFACE_BLITTING;

	if (destination->GetBPP() != 8)
		return;

	source->UpdateWithPalette(destination->GetPalette());

	FRECT float_source_rect = source_rect == nullptr
		? FRECT(0, 0, (float)source->GetWidth(), (float)source->GetHeight())
		: FRECT(*source_rect);

	FRECT float_destination_rect;
	if (destination_rect == nullptr) {
		ScaleFRect(nullptr, &float_destination_rect);
	} else {
		float_destination_rect = FRECT(*destination_rect);
		ScaleFRect(&float_destination_rect, &float_destination_rect);
	}

	BlitToBackbufferAndPresent(&float_destination_rect, source->RequestGPUTexture(), &float_source_rect);
}

VOID D2GI::OnClear(DWORD dwCount, D3D7::LPD3DRECT pRects, DWORD dwFlags, D3D7::D3DCOLOR col, D3D7::D3DVALUE z, DWORD dwStencil)
{
	D3D9::D3DRECT* pScaledRects = static_cast<D3D9::D3DRECT*>(_malloca(dwCount * sizeof(*pScaledRects)));

	for (DWORD i = 0; i < dwCount; i++)
	{
		ScaleD3D9Rect(&pRects[i], &pScaledRects[i]);
	}

	m_pDev->Clear(dwCount, pScaledRects, dwFlags, col, z, dwStencil);

	_freea(pScaledRects);
}


VOID D2GI::OnLightEnable(DWORD i, BOOL bEnable)
{
	if (D2GIConfig::RemixHeadlights() > 0 && i >= REMIX_HEADLIGHT_BASE)
		return;
	if (D2GIConfig::RemixSunStrength() > 0 && i == REMIX_SUN_LIGHT_INDEX)
		return;
	if (i < 64)
	{
		m_bGameLightOn[i] = !!bEnable;
		if (m_bGameDirLightDropped[i])
			bEnable = FALSE;
	}
	if (D2GIConfig::RemixDebugDrawsEnabled())
	{
		static int s_nLogged = 0;
		if (s_nLogged++ < 100)
			Logger::Log(TEXT("Remix light %u enable %d"), (unsigned)i, (int)bEnable);
		if (IsDumpFrame())
			fprintf(m_pRemixLog, "E%u %u %d\n", m_dbg3DFrame, (unsigned)i, (int)bEnable);
	}
	m_pDev->LightEnable(i, bEnable);
}


VOID D2GI::OnSysMemSurfaceBltOnBackBuffer(D2GISystemMemorySurface* pSrc, RECT* pSrcRT, D2GIBackBufferSurface* pDst, RECT* pDstRT)
{
	D3D9::D3DSURFACE_DESC sSrcDesc, sDstDesc;
	FRECT frtSrc, frtDst;
	FRECT frtScaledDst;

	m_eRenderState = RS_BACKBUFFER_BLITTING;
	auto SceneScope = BeginSceneScope();

	Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> pRT;
	m_pDev->GetRenderTarget(0, pRT.GetAddressOf());
	D3D9::IDirect3DTexture9* pSrcTexture = pSrc->RequestGPUTexture();

	pSrcTexture->GetLevelDesc(0, &sSrcDesc);
	pRT->GetDesc(&sDstDesc);
	if (pSrcRT != nullptr)
		frtSrc = FRECT(*pSrcRT);
	else
		frtSrc = FRECT(0, 0, (FLOAT)sSrcDesc.Width, (FLOAT)sSrcDesc.Height);

	if (pDstRT != nullptr)
	{
		frtDst = FRECT(*pDstRT);

		// If we have any cached floating point X/Y values, add them before scaling.
		// This is useful for smooth scrolling minimap icons.
		float integralX, integralY;
		const float fracX = std::modf(m_LastBltX, &integralX);
		const float fracY = std::modf(m_LastBltY, &integralY);

		frtDst.fLeft += fracX;
		frtDst.fRight += fracX;
		frtDst.fTop += fracY;
		frtDst.fBottom += fracY;
	}
	else
		frtDst = FRECT(0, 0, (FLOAT)sDstDesc.Width, (FLOAT)sDstDesc.Height);

	ScaleFRect(&frtDst, &frtScaledDst);
	m_Blitter.Blit(pRT.Get(), &frtScaledDst,
		pSrcTexture, &frtSrc, pSrc->HasColorKeyConversion());

	if (D2GIConfig::RemixPersistentHUDEnabled())
	{
		// Minimap icons move every frame and would leave trails, so they are not remembered.
		// Everything else (panels, digits, text) is.
		if (!m_bInMapDraw)
		{
			// A fresh panel hides whatever older glyphs were drawn on top of it before:
			// forget them, otherwise e.g. a shorter number would keep a stale last digit.
			const float fArea = (frtDst.fRight - frtDst.fLeft) * (frtDst.fBottom - frtDst.fTop);
			if (fArea >= 2048.0f)
			{
				m_PersistentBlits.erase(std::remove_if(m_PersistentBlits.begin(), m_PersistentBlits.end(), [&](const PersistentBlit& b) {
					return b.dwLastFrame != m_dwHUDFrame &&
						b.frtDst.fLeft >= frtScaledDst.fLeft && b.frtDst.fRight <= frtScaledDst.fRight &&
						b.frtDst.fTop >= frtScaledDst.fTop && b.frtDst.fBottom <= frtScaledDst.fBottom &&
						!(b.frtDst.fLeft == frtScaledDst.fLeft && b.frtDst.fRight == frtScaledDst.fRight &&
						  b.frtDst.fTop == frtScaledDst.fTop && b.frtDst.fBottom == frtScaledDst.fBottom);
				}), m_PersistentBlits.end());
			}

			// Keep the list in drawing order: the latest blit goes last, so replaying the whole
			// list reproduces the layering (panel first, then the digits drawn over it)
			auto it = std::find_if(m_PersistentBlits.begin(), m_PersistentBlits.end(), [&](const PersistentBlit& b) {
				return b.frtDst.fLeft == frtScaledDst.fLeft && b.frtDst.fTop == frtScaledDst.fTop &&
					b.frtDst.fRight == frtScaledDst.fRight && b.frtDst.fBottom == frtScaledDst.fBottom;
			});
			if (it != m_PersistentBlits.end())
				m_PersistentBlits.erase(it);
			m_PersistentBlits.push_back({});
			it = std::prev(m_PersistentBlits.end());
			it->pTexture = pSrcTexture;
			it->frtSrc = frtSrc;
			it->frtDst = frtScaledDst;
			it->bColorKey = pSrc->HasColorKeyConversion();
			it->dwLastFrame = m_dwHUDFrame;
		}
	}
}


VOID D2GI::ReplayPersistentBlits()
{
	constexpr uint32_t MAX_AGE_FRAMES = 300;

	if (!m_PersistentBlits.empty())
	{
		Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> pRT;
		m_pDev->GetRenderTarget(0, pRT.GetAddressOf());

		auto SceneScope = BeginSceneScope();
		for (auto it = m_PersistentBlits.begin(); it != m_PersistentBlits.end();)
		{
			if (m_dwHUDFrame - it->dwLastFrame > MAX_AGE_FRAMES)
			{
				it = m_PersistentBlits.erase(it);
				continue;
			}
			// Redraw everything, including what the game drew this frame, so newer layers stay on top
			m_Blitter.Blit(pRT.Get(), &it->frtDst, it->pTexture.Get(), &it->frtSrc, it->bColorKey);
			++it;
		}
	}
	m_dwHUDFrame++;
}


VOID D2GI::OnSysMemSurfaceBltOnTexture(D2GISystemMemorySurface* pSrc, RECT* pSrcRT, D2GITexture* pDst, RECT* pDstRT)
{
	// MULTITHREADED_ACCESS
	if (D2GIConfig::RemixStaticBlitTexturesEnabled())
	{
		// [REMIX] StaticBlitTextures=1: the game regenerates the windshield rain texture every frame
		// from a back-buffer readback. Under Remix the readback is meaningless and every rewrite gets
		// a new texture hash, so neither a replacement material nor Ignore can target it.
		// Fill the texture once with fixed content and drop the per-frame updates: its hash is then
		// constant and the mod's material can replace it.
		FillStaticBlitTexture(pDst);
		return;
	}

	const POINT destPoint = pDstRT != nullptr ? POINT{ pDstRT->left, pDstRT->top } : POINT{ 0, 0 };
	if (FAILED(m_pDev->UpdateSurface(pSrc->GetSystemMemSurface(), pSrcRT, pDst->GetD3D9Surface(), &destPoint)))
		Logger::Error(TEXT("Failed to blit the surface"));
}


void D2GI::OnSceneBegin()
{
	// Some old game versions can call BeginScene/EndScene pairs with nothing between
	// or just with back buffer write inside. So we should track RS_3D_RENDERING state
	// on DrawPrimitive calls (instead of scene begin/end calls), otherwise we can
	// lose RS_BACKBUFFER_STREAMING state and get FMVs broken.
	TryBeginScene();
}


VOID D2GI::TryBeginScene()
{
	if (m_SceneBeginCount++ == 0)
	{
		m_pDev->BeginScene();
	}
}


void D2GI::OnSceneEnd()
{
	TryEndScene();
}


VOID D2GI::TryEndScene()
{
	if (m_SceneBeginCount > 0 && --m_SceneBeginCount == 0)
	{
		m_pDev->EndScene();
	}
}


VOID D2GI::OnRenderStateSet(D3D7::D3DRENDERSTATETYPE eState, DWORD dwValue)
{
	switch (eState)
	{
		case D3D7::D3DRENDERSTATE_CULLMODE:
			m_pDev->SetRenderState(D3D9::D3DRS_CULLMODE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_TEXTUREPERSPECTIVE:
			break;
		case D3D7::D3DRENDERSTATE_COLORKEYENABLE:
			m_bColorKeyEnabled = dwValue;
			break;
		case D3D7::D3DRENDERSTATE_ZENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_ZENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_SHADEMODE:
			m_pDev->SetRenderState(D3D9::D3DRS_SHADEMODE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_DITHERENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_DITHERENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_SRCBLEND:
			m_pDev->SetRenderState(D3D9::D3DRS_SRCBLEND, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_DESTBLEND:
			m_pDev->SetRenderState(D3D9::D3DRS_DESTBLEND, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_ALPHABLENDENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_SPECULARENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_SPECULARENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_COLORVERTEX:
			m_pDev->SetRenderState(D3D9::D3DRS_COLORVERTEX, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_AMBIENTMATERIALSOURCE:
			m_pDev->SetRenderState(D3D9::D3DRS_AMBIENTMATERIALSOURCE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_SPECULARMATERIALSOURCE:
			m_pDev->SetRenderState(D3D9::D3DRS_SPECULARMATERIALSOURCE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_EMISSIVEMATERIALSOURCE:
			m_pDev->SetRenderState(D3D9::D3DRS_EMISSIVEMATERIALSOURCE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_DIFFUSEMATERIALSOURCE:
			m_dwDiffuseMaterialSource = dwValue;
			m_pDev->SetRenderState(D3D9::D3DRS_DIFFUSEMATERIALSOURCE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_ALPHATESTENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_ALPHATESTENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_FOGENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_FOGENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_FOGCOLOR:
			m_dwFogColor = dwValue;
			m_pDev->SetRenderState(D3D9::D3DRS_FOGCOLOR, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_FOGVERTEXMODE:
			m_pDev->SetRenderState(D3D9::D3DRS_FOGVERTEXMODE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_FOGSTART:
			m_pDev->SetRenderState(D3D9::D3DRS_FOGSTART, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_FOGEND:
			m_pDev->SetRenderState(D3D9::D3DRS_FOGEND, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_AMBIENT:
			if (D2GIConfig::RemixDebugDrawsEnabled())
			{
				static DWORD s_dwLastAmbient = 0xFFFFFFFF;
				static int s_nLogged = 0;
				if (dwValue != s_dwLastAmbient && s_nLogged++ < 50)
					Logger::Log(TEXT("Remix ambient 0x%08X"), (unsigned)dwValue);
				s_dwLastAmbient = dwValue;
			}
			m_pDev->SetRenderState(D3D9::D3DRS_AMBIENT, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_ZWRITEENABLE:
			m_pDev->SetRenderState(D3D9::D3DRS_ZWRITEENABLE, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_ZFUNC:
			m_pDev->SetRenderState(D3D9::D3DRS_ZFUNC, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_CLIPPING:
			m_pDev->SetRenderState(D3D9::D3DRS_CLIPPING, dwValue);
			break;
		case D3D7::D3DRENDERSTATE_TEXTUREFACTOR:
			m_pDev->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, dwValue);
			break;
	}
}


VOID D2GI::OnTextureStageSet(DWORD i, D3D7::D3DTEXTURESTAGESTATETYPE eState, DWORD dwValue)
{
	if (i < 2 && (DWORD)eState < 32)
		m_dbgStageState[i][eState] = dwValue;

	switch (eState)
	{
		case D3D7::D3DTSS_COLOROP:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_COLOROP, dwValue);
			break;
		case D3D7::D3DTSS_COLORARG1:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_COLORARG1, dwValue);
			break;
		case D3D7::D3DTSS_COLORARG2:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_COLORARG2, dwValue);
			break;
		case D3D7::D3DTSS_ALPHAOP:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_ALPHAOP, dwValue);
			break;
		case D3D7::D3DTSS_ALPHAARG1:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_ALPHAARG1, dwValue);
			break;
		case D3D7::D3DTSS_ALPHAARG2:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_ALPHAARG2, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVMAT00:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVMAT00, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVMAT01:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVMAT01, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVMAT10:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVMAT10, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVMAT11:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVMAT11, dwValue);
			break;
		case D3D7::D3DTSS_TEXCOORDINDEX:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_TEXCOORDINDEX, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVLSCALE:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVLSCALE, dwValue);
			break;
		case D3D7::D3DTSS_BUMPENVLOFFSET:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_BUMPENVLOFFSET, dwValue);
			break;
		case D3D7::D3DTSS_TEXTURETRANSFORMFLAGS:
			m_pDev->SetTextureStageState(i, D3D9::D3DTSS_TEXTURETRANSFORMFLAGS, dwValue);
			break;

		case D3D7::D3DTSS_ADDRESS:
			if (m_UVOverride.has_value())
			{
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSU, m_UVOverride->first);
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSV, m_UVOverride->second);
				break;
			}

			// UV debug - press Shift+T to paint all out-of-bounds UV coordinates purple,
			// then look for 1px seams in the vegetation.
#ifdef _DEBUG
			if (dwValue == D3D9::D3DTADDRESS_WRAP)
			{
				using namespace D3D9;
				static bool bDebugEnabled = false;
				static bool bHotkeyPressed = false;
				if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 && (GetAsyncKeyState(0x54) & 0x8000) != 0)
				{
					if (!bHotkeyPressed)
					{
						bDebugEnabled = !bDebugEnabled;
						bHotkeyPressed = true;
					}
				}
				else
				{
					bHotkeyPressed = false;
				}

				if (bDebugEnabled)
				{
					using namespace D3D9;
					m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSU, D3D9::D3DTADDRESS_BORDER);
					m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSV, D3D9::D3DTADDRESS_BORDER);
					m_pDev->SetSamplerState(i, D3D9::D3DSAMP_BORDERCOLOR, D3DCOLOR_RGBA(255, 0, 255, 255));
					break;
				}
			}
#endif

			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSU, dwValue);
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSV, dwValue);
			break;
		case D3D7::D3DTSS_ADDRESSU:
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSU, dwValue);
			break;
		case D3D7::D3DTSS_ADDRESSV:
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_ADDRESSV, dwValue);
			break;
		case D3D7::D3DTSS_BORDERCOLOR:
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_BORDERCOLOR, dwValue);
			break;
		case D3D7::D3DTSS_MAGFILTER:
			{
				const D3D9::D3DTEXTUREFILTERTYPE aeMagTexFMap[] =
				{
					D3D9::D3DTEXF_NONE,
					D3D9::D3DTEXF_POINT,
					anisotropy_state_.is_enabled_for_magnifying ? D3D9::D3DTEXF_ANISOTROPIC : D3D9::D3DTEXF_LINEAR,
					D3D9::D3DTEXF_PYRAMIDALQUAD,
					D3D9::D3DTEXF_GAUSSIANQUAD,
					D3D9::D3DTEXF_ANISOTROPIC,
				};
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MAGFILTER, aeMagTexFMap[dwValue]);
			}
			break;
		case D3D7::D3DTSS_MINFILTER:
			{
				const D3D9::D3DTEXTUREFILTERTYPE aeMinTexFMap[] =
				{
					D3D9::D3DTEXF_NONE,
					D3D9::D3DTEXF_POINT,
					anisotropy_state_.is_enabled_for_minifying ? D3D9::D3DTEXF_ANISOTROPIC : D3D9::D3DTEXF_LINEAR,
					D3D9::D3DTEXF_ANISOTROPIC,
				};
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MINFILTER, aeMinTexFMap[dwValue]);
			}
			break;
		case D3D7::D3DTSS_MIPFILTER:
			{
				static const D3D9::D3DTEXTUREFILTERTYPE aeMipTexFMap[] =
				{
					D3D9::D3DTEXF_NONE,
					D3D9::D3DTEXF_NONE,
					D3D9::D3DTEXF_POINT,
					D3D9::D3DTEXF_LINEAR,
				};
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MIPFILTER, aeMipTexFMap[dwValue]);
			}
			break;
		case D3D7::D3DTSS_MIPMAPLODBIAS:
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MIPMAPLODBIAS, dwValue);
			break;
		case D3D7::D3DTSS_MAXMIPLEVEL:
			m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MAXMIPLEVEL, dwValue);
			break;
		case D3D7::D3DTSS_MAXANISOTROPY:
			// If we force anisotropy, don't let the game change it
			if (!anisotropy_state_.is_enabled)
				m_pDev->SetSamplerState(i, D3D9::D3DSAMP_MAXANISOTROPY, dwValue);
			break;
	}

}

#include <cassert>

VOID D2GI::OnTextureSet(DWORD i, D2GITexture* pTex)
{
	m_lpCurrentTextures[i] = pTex;

	if (pTex != nullptr)
	{
		assert(pTex->GetType() != ST_SYSMEM);
	}

	m_pDev->SetTexture(i, pTex == NULL ? NULL : pTex->GetD3D9Texture());
}


BOOL D2GI::OnDeviceValidate(DWORD* pdw)
{
	return SUCCEEDED(m_pDev->ValidateDevice(pdw));
}


VOID D2GI::OnTransformSet(D3D7::D3DTRANSFORMSTATETYPE eType, D3D7::D3DMATRIX* pMatrix)
{
	if (((DWORD)eType == 16 || (DWORD)eType == 17) && pMatrix != nullptr) // D3DTRANSFORMSTATE_TEXTURE0/1
	{
		const DWORD stage = (DWORD)eType - 16;
		m_dbgTexXformSet[stage] = true;
		CopyMemory(m_dbgTexXform[stage], pMatrix, sizeof(m_dbgTexXform[stage]));
	}

	switch (eType)
	{
		case D3D7::D3DTRANSFORMSTATE_WORLD:
			if (pMatrix != nullptr)
				CopyMemory(m_World, pMatrix, sizeof(m_World));
			m_pDev->SetTransform(D3D9::D3DTS_WORLD, (D3D9::D3DMATRIX*)pMatrix);
			break;
		case D3D7::D3DTRANSFORMSTATE_VIEW:
			// The cab camera sets a view matrix with _11 == 1 (view space). Keep the last
			// world-camera view so cab-only frames can still place rain in the world.
			if (pMatrix != nullptr && fabsf(pMatrix->_11 - 1.f) > 0.2f)
			{
				CopyMemory(m_WorldView, pMatrix, sizeof(m_WorldView));
				m_bHaveWorldView = true;
				if (D2GIConfig::RemixHeadlights() > 0 && !m_bHeadlightsSetThisFrame)
					SetRemixHeadlights(m_WorldView);
			}
			m_pDev->SetTransform(D3D9::D3DTS_VIEW, (D3D9::D3DMATRIX*)pMatrix);
			break;
		case D3D7::D3DTRANSFORMSTATE_WORLD1:
			m_pDev->SetTransform(D3D9::D3DTS_WORLD1, (D3D9::D3DMATRIX*)pMatrix);
			break;
		case D3D7::D3DTRANSFORMSTATE_WORLD2:
			m_pDev->SetTransform(D3D9::D3DTS_WORLD2, (D3D9::D3DMATRIX*)pMatrix);
			break;
		case D3D7::D3DTRANSFORMSTATE_WORLD3:
			m_pDev->SetTransform(D3D9::D3DTS_WORLD3, (D3D9::D3DMATRIX*)pMatrix);
			break;
		default:
			m_pDev->SetTransform((D3D9::D3DTRANSFORMSTATETYPE)eType, (D3D9::D3DMATRIX*)pMatrix);
			break;
	}
}


VOID D2GI::OnLightSet(DWORD i, D3D7::LPD3DLIGHT7 pLight)
{
	if (D2GIConfig::RemixHeadlights() > 0 && i >= REMIX_HEADLIGHT_BASE)
		return;
	if (D2GIConfig::RemixSunStrength() > 0 && i == REMIX_SUN_LIGHT_INDEX)
	{
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Logger::Log(TEXT("Remix: the game uses light %u, which SunStrength reserves for the sun; ignoring it"), (unsigned)i);
		}
		return;
	}
	// SingleSun: the game lights each object with its own directional lights (light 0 changes direction per
	// object, light 1 is a white horizontal fill). Remix turns every one of them into a distant light, so with
	// our sun the scene had several suns and shadows in several directions. Keep only our sun.
	const bool bDropDir = D2GIConfig::RemixSingleSunEnabled() && D2GIConfig::RemixSunStrength() > 0 &&
		pLight != nullptr && pLight->dltType == D3D7::D3DLIGHT_DIRECTIONAL;
	if (i < 64)
	{
		if (bDropDir && !m_bGameDirLightDropped[i])
		{
			static bool s_bLogged = false;
			if (!s_bLogged)
			{
				s_bLogged = true;
				Logger::Log(TEXT("Remix: SingleSun drops the game's directional lights (first: light %u)"), (unsigned)i);
			}
			m_pDev->LightEnable(i, FALSE);
		}
		if (m_bGameDirLightDropped[i] && !bDropDir && pLight != nullptr)
		{
			// The index holds a non-directional light again: set it and restore what the game enabled.
			m_bGameDirLightDropped[i] = false;
			m_pDev->SetLight(i, (D3D9::D3DLIGHT9*)pLight);
			m_pDev->LightEnable(i, m_bGameLightOn[i]);
			return;
		}
		m_bGameDirLightDropped[i] = bDropDir;
	}
	if (bDropDir)
		return;
	// DebugDraws: log the game's fixed-function lights (RTX Remix converts them into its own lights).
	// Log a light index again only when its type changes, at most 300 lines.
	if (D2GIConfig::RemixDebugDrawsEnabled() && pLight != nullptr)
	{
		static DWORD s_dwLastType[64] = {};
		static int s_nLogged = 0;
		const DWORD dwType = (DWORD)pLight->dltType;
		if (i < 64 && s_nLogged < 300 && (s_dwLastType[i] != dwType || s_nLogged < 40))
		{
			s_dwLastType[i] = dwType;
			s_nLogged++;
			Logger::Log(TEXT("Remix light %u: type %u diffuse(%.2f %.2f %.2f) pos(%.1f %.1f %.1f) dir(%.2f %.2f %.2f) range %.1f att(%.3f %.3f %.3f) cone(%.2f %.2f)"),
				(unsigned)i, (unsigned)dwType, pLight->dcvDiffuse.r, pLight->dcvDiffuse.g, pLight->dcvDiffuse.b,
				pLight->dvPosition.x, pLight->dvPosition.y, pLight->dvPosition.z,
				pLight->dvDirection.x, pLight->dvDirection.y, pLight->dvDirection.z, pLight->dvRange,
				pLight->dvAttenuation0, pLight->dvAttenuation1, pLight->dvAttenuation2, pLight->dvTheta, pLight->dvPhi);
		}
	}
	// DebugDraws dump frames: every light the game sets, in draw order (next to the F<frame> draw lines)
	if (D2GIConfig::RemixDebugDrawsEnabled() && pLight != nullptr && IsDumpFrame())
		fprintf(m_pRemixLog, "L%u %u type%u diff(%.2f %.2f %.2f) pos(%.1f %.1f %.1f) dir(%.2f %.2f %.2f) range %.1f att(%.3f %.3f %.4f) cone(%.2f %.2f)\n",
			m_dbg3DFrame, (unsigned)i, (unsigned)pLight->dltType, pLight->dcvDiffuse.r, pLight->dcvDiffuse.g, pLight->dcvDiffuse.b,
			pLight->dvPosition.x, pLight->dvPosition.y, pLight->dvPosition.z,
			pLight->dvDirection.x, pLight->dvDirection.y, pLight->dvDirection.z, pLight->dvRange,
			pLight->dvAttenuation0, pLight->dvAttenuation1, pLight->dvAttenuation2, pLight->dvTheta, pLight->dvPhi);
	m_pDev->SetLight(i, (D3D9::D3DLIGHT9*)pLight);
}


bool D2GI::IsDumpFrame() const
{
	return m_pRemixLog != nullptr && m_dbg3DFrame > 0 && m_dbg3DFrame % 600 == 0 && m_dbg3DFrame <= 600 * 40;
}


VOID D2GI::RestoreMaterial()
{
	m_pDev->SetMaterial((D3D9::D3DMATERIAL9*)&m_CurrentMaterial);
}


VOID D2GI::OnMaterialSet(D3D7::LPD3DMATERIAL7 pMaterial)
{
	if (pMaterial != nullptr)
		m_CurrentMaterial = *pMaterial;

	m_pDev->SetMaterial((D3D9::D3DMATERIAL9*)pMaterial);
}


VOID D2GI::OnClipStatusSet(D3D7::LPD3DCLIPSTATUS pStatus)
{
}


VOID D2GI::Present()
{
	m_bSawDrop = false;
	m_bSawRain = false;
	m_bEmittedCabRain = false;
	m_bLastFrameCab = m_bSawCabThisFrame;
	m_bSawCabThisFrame = false;
	m_bHeadlightsSetThisFrame = false;

	// In D3D7 it was allowed to call Present inside a BeginScene/EndScene pair, but in D3D9 it's not.
	bool bRestartScene = false;
	if (m_SceneBeginCount > 0)
	{
		m_pDev->EndScene();
		bRestartScene = true;
	}

	const HRESULT present_result = m_pDev->Present(NULL, NULL, NULL, NULL);
	if (present_result == D3DERR_DEVICELOST) {
		Logger::Log(T("D3D9 device is lost, resetting it..."));
		ResetD3D9Device();
	}

	if (bRestartScene)
	{
		m_pDev->BeginScene();
	}
}


VOID D2GI::OnIndexedPrimitiveStridedDraw(
	D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, D3D7::LPD3DDRAWPRIMITIVESTRIDEDDATA pData,
	DWORD dwCount, LPWORD pIdx, DWORD dwIdxCount, DWORD dwFlags)
{
	DrawPrimitive(pt, dwFVF, TRUE, pData, dwCount, pIdx, dwIdxCount, dwFlags);
}


VOID D2GI::OnPrimitiveStridedDraw(
	D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, D3D7::LPD3DDRAWPRIMITIVESTRIDEDDATA pData,
	DWORD dwCount, DWORD dwFlags)
{
	DrawPrimitive(pt, dwFVF, TRUE, pData, dwCount, NULL, 0, dwFlags);
}



VOID D2GI::OnPrimitiveDraw(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, LPVOID pVerts, DWORD dwVertCount, DWORD dwFlags)
{
	void* vertexMemory = nullptr;

	const bool bNeedsRHWFixup = (dwFVF & D3DFVF_XYZRHW) != 0;
	const bool bNeedsPointSizeFixup = pt == D3D7::D3DPT_POINTLIST && (dwFVF & D3DFVF_RESERVED1) != 0;
	if (bNeedsRHWFixup || bNeedsPointSizeFixup)
	{
		const size_t uStride = CalcFVFStride(dwFVF);
		const size_t uSize = uStride * dwVertCount;

		vertexMemory = _malloca(uSize);
		memcpy(vertexMemory, pVerts, uSize);

		if (bNeedsRHWFixup)
		{
			float* pV = static_cast<float*>(vertexMemory);
			for (size_t i = 0; i < dwVertCount; i++)
			{
				pV[0] *= m_fWidthScale;
				pV[1] *= m_fHeightScale;

				pV = reinterpret_cast<float*>(reinterpret_cast<char*>(pV) + uStride);
			}
		}
		if (bNeedsPointSizeFixup)
		{
			const float scaledPointSize = m_dwForcedHeight / 480.0f; // Keep constant size relative to the lowest base resolution

			float* pPointSize = reinterpret_cast<float*>(static_cast<char*>(vertexMemory) + CalcOffsetToPSize(dwFVF));
			for (size_t i = 0; i < dwVertCount; i++)
			{
				*pPointSize = scaledPointSize;

				pPointSize = reinterpret_cast<float*>(reinterpret_cast<char*>(pPointSize) + uStride);
			}
		}
		pVerts = vertexMemory;
	}

	DrawPrimitive(pt, dwFVF, FALSE, pVerts, dwVertCount, NULL, 0, dwFlags);
	_freea(vertexMemory);
}


VOID D2GI::OnIndexedPrimitiveDraw(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, LPVOID pVerts, DWORD dwVertCount, LPWORD pIdx, DWORD dwIdxCount, DWORD dwFlags)
{
	DrawPrimitive(pt, dwFVF, FALSE, pVerts, dwVertCount, pIdx, dwIdxCount, dwFlags);
}


BOOL D2GI::OnRenderStateGet(D3D7::D3DRENDERSTATETYPE eState, DWORD* pValue)
{
	switch (eState)
	{
		case D3D7::D3DRENDERSTATE_CULLMODE:
			m_pDev->GetRenderState(D3D9::D3DRS_CULLMODE, pValue);
			return TRUE;
	}

	return FALSE;
}


static bool TextureNameIs(D2GITexture* pTex, const char* file)
{
	if (pTex == nullptr || file == nullptr)
		return false;
	const std::string& name = pTex->GetDebugName();
	const size_t n = std::strlen(file);
	if (name.size() < n || _stricmp(name.c_str() + name.size() - n, file) != 0)
		return false;
	return name.size() == n || name[name.size() - n - 1] == '\\' || name[name.size() - n - 1] == '/';
}


static bool IsWindshieldGlass(D2GITexture* pTex)
{
	if (pTex == nullptr)
		return false;
	const std::string& name = pTex->GetDebugName();
	static const char kName[] = "glass.txr";
	const size_t n = sizeof(kName) - 1;
	if (name.size() < n || _stricmp(name.c_str() + name.size() - n, kName) != 0)
		return false;
	return TextureNameIs(pTex, "glass.txr");
}


VOID D2GI::MaybeEmitCabRain()
{
	if (m_bEmittedCabRain || !m_bSawDrop || m_bSawRain)
		return;
	m_bEmittedCabRain = true;

	if (m_pRainTexture == nullptr || !m_bHaveWorldView || m_pDev == nullptr)
	{
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Logger::Log(TEXT("Remix: cab rain not drawn (texture=%d view=%d)"),
				(int)(m_pRainTexture != nullptr), (int)m_bHaveWorldView);
		}
		return;
	}

	const FLOAT* v = m_WorldView;
	const FLOAT rx = v[0], ry = v[1], rz = v[2];
	const FLOAT ux = v[4], uy = v[5], uz = v[6];
	const FLOAT fx = v[8], fy = v[9], fz = v[10];
	const FLOAT tx = v[12], ty = v[13], tz = v[14];
	const FLOAT eye[3] = {
		-(tx * rx + ty * ux + tz * fx),
		-(tx * ry + ty * uy + tz * fy),
		-(tx * rz + ty * uz + tz * fz),
	};

	struct Vtx { FLOAT x, y, z; DWORD c; FLOAT u, vv; };
	Vtx verts[24];
	const FLOAT dist = 14.f, halfW = 7.f, halfH = 10.f;
	for (int i = 0; i < 6; i++)
	{
		const FLOAT ang = (FLOAT)i * (3.14159265f / 3.f);
		const FLOAT cs = cosf(ang), sn = sinf(ang);
		const FLOAT dx = rx * cs + fx * sn;
		const FLOAT dy = ry * cs + fy * sn;
		const FLOAT dz = rz * cs + fz * sn;
		const FLOAT cx = eye[0] + dx * dist;
		const FLOAT cy = eye[1] + dy * dist;
		const FLOAT cz = eye[2] + dz * dist;
		// Horizontal axis perpendicular to the sheet normal (dx,dy,dz) and to camera up.
		FLOAT wx = uy * dz - uz * dy;
		FLOAT wy = uz * dx - ux * dz;
		FLOAT wz = ux * dy - uy * dx;
		const FLOAT wlen = sqrtf(wx * wx + wy * wy + wz * wz);
		if (wlen < 1e-4f)
		{
			ZeroMemory(verts + i * 4, sizeof(Vtx) * 4);
			continue;
		}
		wx /= wlen; wy /= wlen; wz /= wlen;
		const FLOAT ox = wx * halfW, oy = wy * halfW, oz = wz * halfW;
		const FLOAT hx = ux * halfH, hy = uy * halfH, hz = uz * halfH;
		Vtx* q = verts + i * 4;
		q[0] = { cx - ox - hx, cy - oy - hy, cz - oz - hz, 0xFFFFFFFF, 0.f, 1.f };
		q[1] = { cx + ox - hx, cy + oy - hy, cz + oz - hz, 0xFFFFFFFF, 1.f, 1.f };
		q[2] = { cx - ox + hx, cy - oy + hy, cz - oz + hz, 0xFFFFFFFF, 0.f, 0.f };
		q[3] = { cx + ox + hx, cy + oy + hy, cz + oz + hz, 0xFFFFFFFF, 1.f, 0.f };
	}

	D3D9::D3DMATRIX world = {}, view = {};
	world._11 = world._22 = world._33 = world._44 = 1.f;
	CopyMemory(&view, m_WorldView, sizeof(view));

	D3D9::IDirect3DTexture9* pRain = m_pRainTexture->GetD3D9Texture();
	if (pRain == nullptr)
		return;

	D3D9::IDirect3DBaseTexture9* pPrevTex0 = nullptr;
	D3D9::IDirect3DBaseTexture9* pPrevTex1 = nullptr;
	if (FAILED(m_pDev->GetTexture(0, &pPrevTex0)))
		pPrevTex0 = nullptr;
	if (FAILED(m_pDev->GetTexture(1, &pPrevTex1)))
		pPrevTex1 = nullptr;
	DWORD blend = 0, src = 0, dst = 0, zwrite = 0, zenable = 0, lighting = 0, cull = 0;
	m_pDev->GetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, &blend);
	m_pDev->GetRenderState(D3D9::D3DRS_SRCBLEND, &src);
	m_pDev->GetRenderState(D3D9::D3DRS_DESTBLEND, &dst);
	m_pDev->GetRenderState(D3D9::D3DRS_ZWRITEENABLE, &zwrite);
	m_pDev->GetRenderState(D3D9::D3DRS_ZENABLE, &zenable);
	m_pDev->GetRenderState(D3D9::D3DRS_LIGHTING, &lighting);
	m_pDev->GetRenderState(D3D9::D3DRS_CULLMODE, &cull);
	D3D9::D3DMATRIX prevWorld, prevView;
	m_pDev->GetTransform(D3D9::D3DTS_WORLD, &prevWorld);
	m_pDev->GetTransform(D3D9::D3DTS_VIEW, &prevView);

	m_pDev->SetTransform(D3D9::D3DTS_WORLD, &world);
	m_pDev->SetTransform(D3D9::D3DTS_VIEW, &view);
	m_pDev->SetTexture(0, pRain);
	m_pDev->SetTexture(1, nullptr);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_MODULATE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_SELECTARG1);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_DISABLE);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_DISABLE);
	m_pDev->SetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, TRUE);
	m_pDev->SetRenderState(D3D9::D3DRS_SRCBLEND, D3D9::D3DBLEND_SRCALPHA);
	m_pDev->SetRenderState(D3D9::D3DRS_DESTBLEND, D3D9::D3DBLEND_INVSRCALPHA);
	m_pDev->SetRenderState(D3D9::D3DRS_ZWRITEENABLE, FALSE);
	m_pDev->SetRenderState(D3D9::D3DRS_ZENABLE, TRUE);
	m_pDev->SetRenderState(D3D9::D3DRS_LIGHTING, FALSE);
	m_pDev->SetRenderState(D3D9::D3DRS_CULLMODE, D3D9::D3DCULL_NONE);
	m_pDev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	{
		auto scene = BeginSceneScope();
		for (int i = 0; i < 6; i++)
		{
			const Vtx* q = verts + i * 4;
			if (q[0].x == 0.f && q[0].y == 0.f && q[0].z == 0.f && q[3].x == 0.f && q[3].y == 0.f && q[3].z == 0.f)
				continue;
			m_pDev->DrawPrimitiveUP(D3D9::D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
		}
	}

	m_pDev->SetTransform(D3D9::D3DTS_WORLD, &prevWorld);
	m_pDev->SetTransform(D3D9::D3DTS_VIEW, &prevView);
	m_pDev->SetTexture(0, pPrevTex0);
	m_pDev->SetTexture(1, pPrevTex1);
	if (pPrevTex0 != nullptr)
		pPrevTex0->Release();
	if (pPrevTex1 != nullptr)
		pPrevTex1->Release();
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, m_dbgStageState[0][D3D7::D3DTSS_COLOROP]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, m_dbgStageState[0][D3D7::D3DTSS_COLORARG1]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG2, m_dbgStageState[0][D3D7::D3DTSS_COLORARG2]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, m_dbgStageState[0][D3D7::D3DTSS_ALPHAOP]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, m_dbgStageState[0][D3D7::D3DTSS_ALPHAARG1]);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, m_dbgStageState[1][D3D7::D3DTSS_COLOROP]);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, m_dbgStageState[1][D3D7::D3DTSS_ALPHAOP]);
	m_pDev->SetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, blend);
	m_pDev->SetRenderState(D3D9::D3DRS_SRCBLEND, src);
	m_pDev->SetRenderState(D3D9::D3DRS_DESTBLEND, dst);
	m_pDev->SetRenderState(D3D9::D3DRS_ZWRITEENABLE, zwrite);
	m_pDev->SetRenderState(D3D9::D3DRS_ZENABLE, zenable);
	m_pDev->SetRenderState(D3D9::D3DRS_LIGHTING, lighting);
	m_pDev->SetRenderState(D3D9::D3DRS_CULLMODE, cull);

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Logger::Log(TEXT("Remix: drawing cab rain sheets at (%.0f %.0f %.0f)"), eye[0], eye[1], eye[2]);
	}
}


// Draw distance (king.exe 8.1). The view distance comes from WEATHER.INI, not from TRUCK.INI or the b3d LOD
// switches: every frame 0x5A9700.. interpolates the weather tables for the time of day with 0x5A9240
// (cdecl: float, float, const float* table -> st0) and stores fog far (fogf_*, table 0x6FC9B8) in
// settings+0x10, fog near (fogn_*, table 0x6FCA58) in settings+0x18 and the far clip plane
// frpl_* (table 0x6FD75C) x fog far in settings+0x20. Stock values: fog far 900 on a good day, 250 bad day,
// 100 bad night, far plane 0.6 x that. Redirect the fog far and fog near calls to a wrapper that multiplies
// the result by DrawDistance / 100: fog and the far plane (which follows fog far) move out together.
typedef double(__cdecl* WeatherLerpFn)(float, float, const void*);
static double s_dDrawDistanceScale = 1.0;

static double __cdecl ScaledWeatherDistance(float a, float b, const void* pTable)
{
	const WeatherLerpFn pfnOrig = reinterpret_cast<WeatherLerpFn>((size_t)0x5A9240);
	return pfnOrig(a, b, pTable) * s_dDrawDistanceScale;
}

static void ApplyDrawDistancePatch()
{
	static bool s_bDone = false;
	if (s_bDone)
		return;
	s_bDone = true;
	const int nPercent = D2GIConfig::RemixDrawDistance();
	if (nPercent <= 0 || nPercent == 100)
		return;
	const BYTE* pBase = reinterpret_cast<const BYTE*>(GetModuleHandle(nullptr));
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
	if (pBase != reinterpret_cast<const BYTE*>(0x400000) || pNt->FileHeader.TimeDateStamp != 0x400502EA)
	{
		Logger::Log(TEXT("Remix: DrawDistance needs king.exe 8.1; not applied"));
		return;
	}
	// call 0x5A9240 right after push 0x6FC9B8 (fog far) and push 0x6FCA58 (fog near).
	static const DWORD kCallSites[] = { 0x5A972F, 0x5A974D };
	for (DWORD site : kCallSites)
	{
		BYTE* p = reinterpret_cast<BYTE*>((size_t)site);
		const DWORD target = site + 5 + *reinterpret_cast<const DWORD*>(p + 1);
		if (p[0] != 0xE8 || target != 0x5A9240)
		{
			Logger::Log(TEXT("Remix: DrawDistance call site %08X does not match; not applied"), (unsigned)site);
			return;
		}
	}
	s_dDrawDistanceScale = nPercent / 100.0;
	for (DWORD site : kCallSites)
	{
		BYTE* p = reinterpret_cast<BYTE*>((size_t)site);
		DWORD dwOld = 0;
		if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &dwOld))
			return;
		const DWORD rel = (DWORD)(size_t)&ScaledWeatherDistance - (site + 5);
		CopyMemory(p + 1, &rel, 4);
		VirtualProtect(p, 5, dwOld, &dwOld);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
	}
	Logger::Log(TEXT("Remix: draw distance x%.2f (weather fog and far plane)"), s_dDrawDistanceScale);
}


VOID D2GI::DrawPrimitive(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
	DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags)
{
	ApplyDrawDistancePatch();
	if (D2GIConfig::RemixSunStrength() > 0 && !m_bSunSetThisFrame)
		SetRemixSun();

	m_eRenderState = RS_3D_RENDERING;

	if (D2GIConfig::RemixDebugDrawsEnabled() && !m_bInUntexturedFix)
		RemixDebugLogDraw(pt, dwFVF, bStrided, pVertexData, dwVertexCount, pIndexData, dwIndexCount);

	// Windshield: the cab camera draws it as one 4-vert fan, stage 0 txr\glass.txr, stage 1 the 128x128
	// glass_env blit texture with projective UVs. Under Remix that card renders black over the whole
	// windshield. Match it by the glass.txr name only (no other draw uses that texture) and skip it.
	if (D2GIConfig::RemixClearWindshieldEnabled() && !m_bInUntexturedFix && IsWindshieldGlass(m_lpCurrentTextures[0]))
	{
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Logger::Log(TEXT("Remix: skipping windshield glass quad (%u verts)"), (unsigned)dwVertexCount);
		}
		return;
	}

	// Cab frames draw drop.txr in world space, then switch the camera to view space and
	// never submit rain.txr. Emit the rain sheets before the gauges, in that world camera.
	if (D2GIConfig::RemixEmitCabRainEnabled() && !m_bInUntexturedFix)
	{
		if (TextureNameIs(m_lpCurrentTextures[0], "rain.txr"))
		{
			m_pRainTexture = m_lpCurrentTextures[0];
			m_bSawRain = true;
		}
		else if (TextureNameIs(m_lpCurrentTextures[0], "drop.txr"))
			m_bSawDrop = true;
		else if (TextureNameIs(m_lpCurrentTextures[0], "pribor.txr"))
			MaybeEmitCabRain();
	}
	// Headlight glows (lg_head.txr quads in front of a vehicle): learn where that vehicle's lamps are and draw the
	// glow with a texture Remix does not turn into effect lights (the Headlights spots light the road instead,
	// whichever way the vehicle faces the camera).
	D2GITexture* pGlowSaved = nullptr;
	if (D2GIConfig::RemixHeadlights() > 0 && !m_bInUntexturedFix && TextureNameIs(m_lpCurrentTextures[0], "lg_head.txr") &&
		LearnVehicleLamp(dwFVF, bStrided, pVertexData, dwVertexCount))
	{
		D3D9::IDirect3DTexture9* pGlow = GetPlayerGlowTexture();
		if (pGlow != nullptr)
		{
			pGlowSaved = m_lpCurrentTextures[0];
			m_pDev->SetTexture(0, pGlow);
		}
	}
	struct GlowRestore {
		D2GI* p; D2GITexture* t;
		~GlowRestore() { if (t != nullptr) p->RestoreTexture0(t); }
	} glowRestore = { this, pGlowSaved };

	// GlowLightColor: Remix colors the effect light of an lg_head glow (rtx.lightConverter) by the draw's material
	// diffuse, which the game leaves from whatever it drew before (a purple car lit the road purple). Draw the
	// glows with a fixed warm white material, then put the game's material back.
	bool bGlowMaterial = false;
	if (D2GIConfig::RemixGlowLightColorEnabled() && TextureNameIs(pGlowSaved != nullptr ? pGlowSaved : m_lpCurrentTextures[0], "lg_head.txr"))
	{
		D3D9::D3DMATERIAL9 mat = {};
		mat.Diffuse = { 1.0f, 0.93f, 0.8f, 1.0f };
		mat.Ambient = mat.Diffuse;
		m_pDev->SetMaterial(&mat);
		bGlowMaterial = true;
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Logger::Log(TEXT("Remix: GlowLightColor draws lg_head glows with a fixed warm white material"));
		}
	}
	struct GlowMaterialRestore {
		D2GI* p; bool b;
		~GlowMaterialRestore() { if (b) p->RestoreMaterial(); }
	} glowMaterialRestore = { this, bGlowMaterial };

	// NightBackdrops: the mountain skybox images (rtx.skyBoxTextures) are lit by nothing and the game darkens them
	// only with fog, which Remix does not use, so at night they glowed as by day. Modulate them in stage 1 by the
	// same night factor as the sky dome. Matched by Remix hash of the 256x256 mountain textures.
	struct StageRestore {
		D3D9::IDirect3DDevice9* d; bool b; DWORD tf, op, a1, a2, aop, aa1;
		~StageRestore()
		{
			if (!b)
				return;
			d->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, tf);
			d->SetTextureStageState(1, D3D9::D3DTSS_COLORARG1, a1);
			d->SetTextureStageState(1, D3D9::D3DTSS_COLORARG2, a2);
			d->SetTextureStageState(1, D3D9::D3DTSS_ALPHAARG1, aa1);
			d->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, op);
			d->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, aop);
		}
	} backdropRestore = { m_pDev, false };
	if (D2GIConfig::RemixNightBackdropsEnabled() && !m_bInUntexturedFix && m_lpCurrentTextures[0] != nullptr &&
		m_lpCurrentTextures[0]->GetWidth() == 256 && m_lpCurrentTextures[0]->GetHeight() == 256 &&
		m_dbgStageState[1][D3D7::D3DTSS_COLOROP] == D3D7::D3DTOP_DISABLE)
	{
		static const unsigned long long kMountains[] = { 0x172E71FD33DEDF0Eull, 0x409C83E1829A1815ull, 0xFEBCB5E0E999B867ull };
		const unsigned long long h = CachedTextureHash(m_lpCurrentTextures[0]);
		if (std::find(std::begin(kMountains), std::end(kMountains), h) != std::end(kMountains))
		{
			StageRestore& r = backdropRestore;
			r.b = true;
			m_pDev->GetRenderState(D3D9::D3DRS_TEXTUREFACTOR, &r.tf);
			m_pDev->GetTextureStageState(1, D3D9::D3DTSS_COLOROP, &r.op);
			m_pDev->GetTextureStageState(1, D3D9::D3DTSS_COLORARG1, &r.a1);
			m_pDev->GetTextureStageState(1, D3D9::D3DTSS_COLORARG2, &r.a2);
			m_pDev->GetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, &r.aop);
			m_pDev->GetTextureStageState(1, D3D9::D3DTSS_ALPHAARG1, &r.aa1);
			const DWORD k = SkyDomeScale();
			m_pDev->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, 0xFF000000 | (k << 16) | (k << 8) | k);
			m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_MODULATE);
			m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG1, D3DTA_CURRENT);
			m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG2, D3DTA_TFACTOR);
			m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_SELECTARG1);
			m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAARG1, D3DTA_CURRENT);
			static bool s_bLogged = false;
			if (!s_bLogged)
			{
				s_bLogged = true;
				Logger::Log(TEXT("Remix: NightBackdrops dims mountain backdrop %016llX by %u/255"), h, (unsigned)k);
			}
		}
	}

	// WetRoads: while it rains (the game draws its rain streaks or road splashes), draw the known road textures
	// with a darkened copy. The copy has its own Remix content hash, which mod.usda gives a wet material (glossy,
	// puddles, raindrop ripples). When the rain stops the roads stay wet for a while.
	if (D2GIConfig::RemixWetRoadsEnabled() && !m_bInUntexturedFix && m_lpCurrentTextures[0] != nullptr)
	{
		D2GITexture* pTex = m_lpCurrentTextures[0];
		if (TextureNameIs(pTex, "drop.txr") || TextureNameIs(pTex, "rain.txr"))
			m_qwLastRainTick = GetTickCount64();
		else if (m_qwLastRainTick != 0 && GetTickCount64() - m_qwLastRainTick < 60000 && pGlowSaved == nullptr &&
			pTex->GetWidth() <= 256 && pTex->GetHeight() <= 256)
		{
			static const unsigned long long kRoads[] = {
				0x9A2C159304E5A4D4ull, // road2.txr: main road with markings
				0x2D89EABEB66BE74Aull, // road.txr: plain asphalt
				0x5FBCFC958E1DDF00ull, // store_b0.txr: cobblestones
			};
			const unsigned long long h = CachedTextureHash(pTex);
			if (std::find(std::begin(kRoads), std::end(kRoads), h) != std::end(kRoads))
			{
				D3D9::IDirect3DTexture9* pWet = GetWetTexture(pTex, h);
				if (pWet != nullptr)
				{
					m_pDev->SetTexture(0, pWet);
					glowRestore.t = pTex;
				}
			}
		}
	}

	// Backdrop: the game first draws an untextured sky dome and a flat ground plane around the camera with
	// z-write off. Under Remix they cover the world (sky) or lie over roads (khaki plane), and their
	// UntexturedColors hash changes with weather and time of day. No other untextured 3D draw has z-write off.
	if (D2GIConfig::RemixSkipBackdropEnabled() && !m_bInUntexturedFix && !m_bInMapDraw && IsBackdropDraw(dwFVF))
	{
		// SkyDome: keep the dome (not the flat ground plane) as Remix sky with a constant texture, so clear
		// weather does not show a black sky above the skybox images.
		if (D2GIConfig::RemixSkyDomeEnabled() && IsDomeShaped(dwFVF, bStrided, pVertexData, dwVertexCount) &&
			DrawUntexturedWithColor(pt, dwFVF, bStrided, pVertexData, dwVertexCount, pIndexData, dwIndexCount, dwFlags, TRUE))
		{
			static bool s_bDomeLogged = false;
			if (!s_bDomeLogged)
			{
				s_bDomeLogged = true;
				Logger::Log(TEXT("Remix: drawing the sky dome with the constant sky texture (%u verts)"), (unsigned)dwVertexCount);
			}
			return;
		}
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Logger::Log(TEXT("Remix: skipping untextured backdrop draw (%u verts)"), (unsigned)dwVertexCount);
		}
		return;
	}

	if (D2GIConfig::RemixUntexturedColorsEnabled() && !m_bInUntexturedFix &&
		DrawUntexturedWithColor(pt, dwFVF, bStrided, pVertexData, dwVertexCount, pIndexData, dwIndexCount, dwFlags))
		return;

	DWORD        dwAlphaTestEnable, dwAlphaTestFunc, dwAlphaTestRef;
	D2GITexture* pTexture = m_lpCurrentTextures[0];

	BOOL bEmulateColorKey = (m_bColorKeyEnabled && pTexture != NULL && pTexture->HasColorKeyConversion());
	BOOL fixAlpha = D2GIConfig::FixAlphaEnabled() && (pTexture != NULL && pTexture->GetD2GIPixelFormat() == D2GIPF_16_4444);


	if (bEmulateColorKey || fixAlpha)
	{
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHATESTENABLE, &dwAlphaTestEnable);
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHAFUNC, &dwAlphaTestFunc);
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHAREF, &dwAlphaTestRef);
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHATESTENABLE, TRUE);
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHAFUNC, D3D9::D3DCMP_GREATEREQUAL);
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHAREF, fixAlpha ? 0x1 : 0x80);
	}

	if (bStrided)
	{
		if (pIndexData != NULL)
		{
			m_StridedRenderer.DrawIndexedPrimitiveStrided(pt, dwFVF,
				(D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*)pVertexData, dwVertexCount,
				pIndexData, dwIndexCount, dwFlags);
		}
		else
		{
			m_StridedRenderer.DrawPrimitiveStrided(pt, dwFVF,
				(D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*)pVertexData, dwVertexCount, dwFlags);
		}
	}
	else
	{
		UINT uVertexStride, uPrimCount;

		uVertexStride = CalcFVFStride(dwFVF);
		uPrimCount = CalcPrimitiveCount(pt, (pIndexData != NULL) ? dwIndexCount : dwVertexCount);

		m_pDev->SetFVF(dwFVF);
		if (pIndexData != NULL)
		{
			m_pDev->DrawIndexedPrimitiveUP((D3D9::D3DPRIMITIVETYPE)pt, 0, dwVertexCount,
				uPrimCount, pIndexData, D3D9::D3DFMT_INDEX16, pVertexData, uVertexStride);
		}
		else
		{
			auto SceneScope = BeginSceneScope();
			m_pDev->DrawPrimitiveUP((D3D9::D3DPRIMITIVETYPE)pt, uPrimCount, pVertexData, uVertexStride);
		}
	}

	if (bEmulateColorKey || fixAlpha)
	{
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHATESTENABLE, dwAlphaTestEnable);
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHAFUNC, dwAlphaTestFunc);
		m_pDev->SetRenderState(D3D9::D3DRS_ALPHAREF, dwAlphaTestRef);
	}
}


VOID D2GI::ScaleFRect(FRECT* pSrc, FRECT* pOut)
{
	if (pSrc == NULL)
	{
		pOut->fLeft = pOut->fTop = 0;
		pOut->fRight = (FLOAT)m_dwForcedWidth;
		pOut->fBottom = (FLOAT)m_dwForcedHeight;
		return;
	}

	pOut->fLeft = (pSrc->fLeft * m_fWidthScale);
	pOut->fTop = (pSrc->fTop * m_fHeightScale);
	pOut->fRight = (pSrc->fRight * m_fWidthScale);
	pOut->fBottom = (pSrc->fBottom * m_fHeightScale);
}


VOID D2GI::ScaleRect(RECT* pSrc, RECT* pOut)
{
	if (pSrc == NULL)
	{
		pOut->left = pOut->top = 0;
		pOut->right = m_dwForcedWidth;
		pOut->bottom = m_dwForcedHeight;
		return;
	}

	pOut->left = pSrc->left * m_dwForcedWidth / m_dwOriginalWidth;
	pOut->top = pSrc->top * m_dwForcedHeight / m_dwOriginalHeight;
	pOut->right = pSrc->right * m_dwForcedWidth / m_dwOriginalWidth;
	pOut->bottom = pSrc->bottom * m_dwForcedHeight / m_dwOriginalHeight;
}


VOID D2GI::AttachWndProc()
{
	if (m_pfnOriginalWndProc != NULL)
		return;

	m_pfnOriginalWndProc = (WNDPROC)GetWindowLong(m_hWnd, GWL_WNDPROC);
	SetWindowLong(m_hWnd, GWL_WNDPROC, (LONG)WndProc_Static);
	SetWindowLong(m_hWnd, GWL_USERDATA, (LONG)this);
}


VOID D2GI::DetachWndProc()
{
	if (m_pfnOriginalWndProc == NULL)
		return;

	SetWindowLong(m_hWnd, GWL_WNDPROC, (LONG)m_pfnOriginalWndProc);
	SetWindowLong(m_hWnd, GWL_USERDATA, 0);
	m_pfnOriginalWndProc = NULL;
}


LRESULT D2GI::WndProc_Static(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return ((D2GI*)GetWindowLong(hWnd, GWL_USERDATA))->WndProc(hWnd, uMsg, wParam, lParam);
}


LRESULT D2GI::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MBUTTONDBLCLK:
		{
			INT x = GET_X_LPARAM(lParam);
			INT y = GET_Y_LPARAM(lParam);

			x = x * m_dwOriginalWidth / m_dwForcedWidth;
			y = y * m_dwOriginalHeight / m_dwForcedHeight;

			lParam = MAKELPARAM(x, y);
			break;
		}
		case WM_GETMINMAXINFO:
		case WM_NCACTIVATE:
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
			return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

	return CallWindowProc(m_pfnOriginalWndProc, hWnd, uMsg, wParam, lParam);
}


VOID D2GI::ScaleD3D9Rect(const D3D7::D3DRECT* pSrc, D3D9::D3DRECT* pOut)
{
	if (pSrc == nullptr)
	{
		pOut->x1 = pOut->y1 = 0;
		pOut->x2 = m_dwForcedWidth;
		pOut->y2 = m_dwForcedHeight;
		return;
	}

	pOut->x1 = pSrc->x1 * m_dwForcedWidth / m_dwOriginalWidth;
	pOut->y1 = pSrc->y1 * m_dwForcedHeight / m_dwOriginalHeight;
	pOut->x2 = pSrc->x2 * m_dwForcedWidth / m_dwOriginalWidth;
	pOut->y2 = pSrc->y2 * m_dwForcedHeight / m_dwOriginalHeight;
}


VOID D2GI::OnTransformsSetup(VOID* pThis, MAT3X4* pmView, MAT3X4* pmProj)
{
	pmProj->_11 /= m_fAspectRatioScale;
}


VOID D2GI::SetupWindow()
{
	DWORD dwWinStyle;
	DWORD dwWinExStyle;
	if (D2GIConfig::GetWindowMode() == WINDOWMODE::WINDOWED)
	{
		dwWinStyle = WS_VISIBLE | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
		dwWinExStyle = WS_EX_OVERLAPPEDWINDOW;
	}
	else
	{
		dwWinStyle = WS_VISIBLE | WS_POPUP;
		dwWinExStyle = 0;
	}

	const INT nDisplayWidth = GetSystemMetrics(SM_CXSCREEN);
	const INT nDisplayHeight = GetSystemMetrics(SM_CYSCREEN);
	const INT nWinWidth = std::min(nDisplayWidth, (INT)D2GIConfig::GetVideoWidth());
	const INT nWinHeight = std::min(nDisplayHeight, (INT)D2GIConfig::GetVideoHeight());
	const INT nWinX = (nDisplayWidth - nWinWidth) / 2;
	const INT nWinY = (nDisplayHeight - nWinHeight) / 2;

	RECT rtWindowRect = { nWinX, nWinY, nWinX+nWinWidth, nWinY+nWinHeight };
	AdjustWindowRectEx(&rtWindowRect, dwWinStyle, FALSE, dwWinExStyle);

	SetWindowLong(m_hWnd, GWL_STYLE, dwWinStyle);
	SetWindowLong(m_hWnd, GWL_EXSTYLE, dwWinExStyle);
	SetWindowPos(m_hWnd, HWND_TOP, rtWindowRect.left, rtWindowRect.top, rtWindowRect.right - rtWindowRect.left, rtWindowRect.bottom - rtWindowRect.top, SWP_DRAWFRAME);
	
	// The window is already active, but the original WndProc didn't let the window style to change the first time - invalidate the nonclient area to force it to repaint.
	SendMessage(m_hWnd, WM_NCACTIVATE, TRUE, NULL);

	m_dwForcedWidth = nWinWidth;
	m_dwForcedHeight = nWinHeight;
}

std::vector<D3D9::D3DDISPLAYMODE> D2GI::GetDisplayModes() {
	std::vector<D3D9::D3DDISPLAYMODE> displayModes;
	INT								  i, nNum;
	D3D9::IDirect3D9*				  pD3D9 = GetD3D9();

	nNum = pD3D9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3D9::D3DFMT_X8R8G8B8);

	for (i = 0; i < nNum; i++)
	{
		D3D9::D3DDISPLAYMODE sDMode;

		if (SUCCEEDED(pD3D9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3D9::D3DFMT_X8R8G8B8, i, &sDMode)))
		{
			displayModes.push_back(sDMode);
		}
	}

	return displayModes;
}

VOID D2GI::OnDisplayModeEnum(LPVOID pArg, D3D7::LPDDENUMMODESCALLBACK2 pCallback)
{
	typedef std::vector<D3D7::DDSURFACEDESC2> DModeContainer;

	INT                  i, nNum;
	DModeContainer       descs(g_asStdDisplayModes, g_asStdDisplayModes + g_uStdDisplayModesCount);
	D3D9::IDirect3D9*    pD3D9 = GetD3D9();

	nNum = pD3D9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3D9::D3DFMT_X8R8G8B8);
	
	for (i = 0; i < nNum; i++)
	{
		D3D9::D3DDISPLAYMODE sDMode;
		D3D7::DDSURFACEDESC2 sDMDesc;

		if (SUCCEEDED(pD3D9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3D9::D3DFMT_X8R8G8B8, i, &sDMode)))
		{
			if (IsStdDisplayMode(sDMode.Width, sDMode.Height))
				continue;

			ZeroMemory(&sDMDesc, sizeof(sDMDesc));
			sDMDesc.dwSize = sizeof(sDMDesc);
			sDMDesc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_REFRESHRATE | DDSD_PITCH | DDSD_PIXELFORMAT;
			sDMDesc.dwWidth = sDMode.Width;
			sDMDesc.dwHeight = sDMode.Height;
			sDMDesc.lPitch = sDMode.Width;
			sDMDesc.ddpfPixelFormat = g_pf8_Pal;

			descs.push_back(sDMDesc);

			sDMDesc.lPitch = sDMode.Width * sizeof(UINT16);
			sDMDesc.ddpfPixelFormat = g_pf16_565;

			descs.push_back(sDMDesc);
		}
	}

	for (D3D7::DDSURFACEDESC2& desc : descs)
	{
		if (pCallback(&desc, pArg) == DDENUMRET_CANCEL)
			break;
	}
}

void D2GI::SynchronizeWithDeviceCaps()
{
	D3D9::D3DCAPS9 caps;

#ifdef DEVICE_CAPS_DUMP
	TCHAR caps_hex_dump[sizeof(caps) * 2 + 1] = T(""); // place dump from remote GPU here
	TCHAR hex_byte[3] = T("\0\0");

	for (size_t i = 0; i < sizeof(caps); i++) {
		_tcsncpy(hex_byte, caps_hex_dump + i * 2, 2);
		*((uint8_t*)&caps + i) = (uint8_t)_tcstol(hex_byte, nullptr, 16);
	}
	// set breakpoint on the next line to see remote GPU caps
#endif

	if (SUCCEEDED(m_pDev->GetDeviceCaps(&caps))) {
#ifdef DEVICE_CAPS_DUMP
		static bool are_caps_dumped = false;
		if (!are_caps_dumped) {
			for (size_t i = 0; i < sizeof(caps); i++)
				_stprintf(caps_hex_dump + i * 2, T("%02X"), (int)*((uint8_t*)&caps + i));

			Logger::Log(T("Device caps: %s"), caps_hex_dump);
			are_caps_dumped = true;
		}
#endif
	} else {
		Logger::Warning(T("Failed to get device caps, some features won't work."));
		ZeroMemory(&caps, sizeof(caps));
	}

	max_primitive_count_ = caps.MaxPrimitiveCount;

	uint32_t anisotropy_level = std::min(std::max(D2GIConfig::AnisotropyLevel(), 1u), static_cast<uint32_t>(caps.MaxAnisotropy));
	bool is_minifying_anisotropy_supported = !!(caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC);
	bool is_magnifying_anisotropy_supported = !!(caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC);

	anisotropy_state_.is_enabled = (anisotropy_level > 1
	                                && !!(caps.RasterCaps & D3DPRASTERCAPS_ANISOTROPY)
	                                && (is_minifying_anisotropy_supported || is_magnifying_anisotropy_supported));

	anisotropy_state_.is_enabled_for_minifying = anisotropy_state_.is_enabled && is_minifying_anisotropy_supported;
	anisotropy_state_.is_enabled_for_magnifying = anisotropy_state_.is_enabled && is_magnifying_anisotropy_supported;

	if (anisotropy_state_.is_enabled) {
		Logger::Log(T("Anisotropy: using %ux"), anisotropy_level);
		for (DWORD stage = 0; stage < 8; stage++)
			m_pDev->SetSamplerState(stage, D3D9::D3DSAMP_MAXANISOTROPY, anisotropy_level);
	} else {
		Logger::Log(T("Anisotropy: disabled"));
	}
}

void D2GI::BlitToBackbufferAndPresent(FRECT* backbuffer_rect, D3D9::IDirect3DTexture9* texture, FRECT* texture_rect)
{
	// It's better not to use ComPtr here: we must release backbuffer before Present()
	// call as Reset() call may happen inside.
	D3D9::IDirect3DSurface9* backbuffer;
	m_pDev->GetBackBuffer(0, 0, D3D9::D3DBACKBUFFER_TYPE_MONO, &backbuffer);

	// Instead of using StretchRect() we should implement it via blitter.
	// StretchRect() may not support magnifying operation on some GPUs,
	// and it seems that it can be bugged in some device drivers.
	// Shader-based blitter is more stable solution.
	TryBeginScene();
	m_Blitter.Blit(backbuffer, backbuffer_rect, texture, texture_rect, false);
	TryEndScene();

	backbuffer->Release();

	Present();
}


// ======= Remix diagnostics: log each unique draw signature once =======

namespace
{
	struct UVRange
	{
		bool  valid = false;
		float minU = 0, maxU = 0, minV = 0, maxV = 0;
		float firstU = 0, firstV = 0;
	};

	UINT FVFPositionSize(DWORD dwFVF)
	{
		switch (dwFVF & 0x00E)
		{
			case 0x002: return 12; // XYZ
			case 0x004: return 16; // XYZRHW
			case 0x006: return 16; // XYZB1
			case 0x008: return 20;
			case 0x00A: return 24;
			case 0x00C: return 28;
			case 0x00E: return 32;
		}
		return 0;
	}

	UINT FVFTexCoordOffset(DWORD dwFVF, UINT uSet)
	{
		UINT uOffset = FVFPositionSize(dwFVF);
		if (dwFVF & 0x010) uOffset += 12; // NORMAL
		if (dwFVF & 0x020) uOffset += 4;  // RESERVED1 / PSIZE
		if (dwFVF & 0x040) uOffset += 4;  // DIFFUSE
		if (dwFVF & 0x080) uOffset += 4;  // SPECULAR
		return uOffset + uSet * 8;        // assume 2D texcoords
	}

	UVRange ComputeUVRange(DWORD dwFVF, BOOL bStrided, VOID* pVertexData, DWORD dwVertexCount, UINT uSet)
	{
		UVRange r;
		const UINT uSetCount = (dwFVF & 0xF00) >> 8;
		if (uSet >= uSetCount || pVertexData == nullptr || dwVertexCount == 0)
			return r;

		const BYTE* pBase;
		UINT uStride;
		if (bStrided)
		{
			auto* pData = static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
			pBase = static_cast<const BYTE*>(pData->textureCoords[uSet].lpvData);
			uStride = pData->textureCoords[uSet].dwStride;
		}
		else
		{
			pBase = static_cast<const BYTE*>(pVertexData) + FVFTexCoordOffset(dwFVF, uSet);
			uStride = (UINT)CalcFVFStride(dwFVF);
		}
		if (pBase == nullptr)
			return r;

		const DWORD dwCount = std::min<DWORD>(dwVertexCount, 4096);
		for (DWORD i = 0; i < dwCount; i++)
		{
			const float* uv = reinterpret_cast<const float*>(pBase + i * uStride);
			if (!r.valid)
			{
				r.valid = true;
				r.minU = r.maxU = r.firstU = uv[0];
				r.minV = r.maxV = r.firstV = uv[1];
			}
			r.minU = std::min(r.minU, uv[0]); r.maxU = std::max(r.maxU, uv[0]);
			r.minV = std::min(r.minV, uv[1]); r.maxV = std::max(r.maxV, uv[1]);
		}
		return r;
	}

	std::string DescribeTexture(D2GITexture* pTex)
	{
		if (pTex == nullptr)
			return "none";
		char buf[200];
		const std::string& name = pTex->GetDebugName();
		sprintf_s(buf, "'%s' %ux%u pf%d", name.empty() ? "?" : name.c_str(),
			(unsigned)pTex->GetWidth(), (unsigned)pTex->GetHeight(), (int)pTex->GetD2GIPixelFormat());
		return buf;
	}

	UINT BytesPerPixel(D3D9::D3DFORMAT f)
	{
		switch (f)
		{
		case D3D9::D3DFMT_A8R8G8B8: case D3D9::D3DFMT_X8R8G8B8: return 4;
		case D3D9::D3DFMT_R8G8B8: return 3;
		case D3D9::D3DFMT_R5G6B5: case D3D9::D3DFMT_X1R5G5B5: case D3D9::D3DFMT_A1R5G5B5:
		case D3D9::D3DFMT_A4R4G4B4: case D3D9::D3DFMT_X4R4G4B4: return 2;
		case D3D9::D3DFMT_P8: case D3D9::D3DFMT_L8: case D3D9::D3DFMT_A8: return 1;
		default: return 0;
		}
	}

	// The hash RTX Remix shows for this texture: XXH3_64 of mip level 0, rows packed tightly
	// (dxvk-remix D3D9CommonTexture::SetupForRtxFrom). Remix hashes on first upload, so this
	// matches as long as the game has not rewritten the texture since.
	std::string RemixTextureHash(D2GITexture* pTex)
	{
		if (pTex == nullptr)
			return "-";
		D3D9::IDirect3DTexture9* pD3DTex = pTex->GetD3D9Texture();
		if (pD3DTex == nullptr)
			return "?";
		D3D9::D3DSURFACE_DESC desc;
		if (FAILED(pD3DTex->GetLevelDesc(0, &desc)))
			return "?";
		const UINT bpp = BytesPerPixel(desc.Format);
		if (bpp == 0)
			return "fmt?";
		D3D9::D3DLOCKED_RECT lr;
		if (FAILED(pD3DTex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
			return "locked?";
		const size_t row = (size_t)desc.Width * bpp;
		XXH3_state_t* st = XXH3_createState();
		XXH3_64bits_reset(st);
		for (UINT y = 0; y < desc.Height; y++)
			XXH3_64bits_update(st, static_cast<const BYTE*>(lr.pBits) + (size_t)y * lr.Pitch, row);
		const XXH64_hash_t h = XXH3_64bits_digest(st);
		XXH3_freeState(st);
		pD3DTex->UnlockRect(0);
		char buf[24];
		sprintf_s(buf, "%016llX", (unsigned long long)h);
		return buf;
	}

	std::string DescribeUV(const UVRange& r)
	{
		if (!r.valid)
			return "-";
		char buf[160];
		sprintf_s(buf, "u[%.3f..%.3f] v[%.3f..%.3f] first(%.3f,%.3f)", r.minU, r.maxU, r.minV, r.maxV, r.firstU, r.firstV);
		return buf;
	}
}


VOID D2GI::RemixDebugLogDraw(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
	DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount)
{
	constexpr size_t MAX_UNIQUE_DRAWS = 4000;

	m_dbgDrawCounter++;
	m_dbgFrameDraw++;

	// Frame-order dump: every 600th 3D frame (up to 8 times), one line per draw in drawing order.
	// Remix ray-traces draws only until the first UI-tagged texture, so the order matters.
	if (IsDumpFrame())
	{
		DWORD rsZW = 0, rsBlend = 0;
		m_pDev->GetRenderState(D3D9::D3DRS_ZWRITEENABLE, &rsZW);
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, &rsBlend);
		const std::string& n0 = m_lpCurrentTextures[0] != nullptr ? m_lpCurrentTextures[0]->GetDebugName() : std::string("none");
		// Depth of the draw in the world camera: mean of up to 64 vertices, through the world and world-view matrices
		float depth = -1.0f;
		if ((dwFVF & 0x00E) == 0x002 && pVertexData != nullptr && dwVertexCount > 0)
		{
			const BYTE* pPos = static_cast<const BYTE*>(pVertexData);
			UINT uStride = (UINT)CalcFVFStride(dwFVF);
			if (bStrided)
			{
				const D3D7::D3DDRAWPRIMITIVESTRIDEDDATA* sd = static_cast<const D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
				pPos = static_cast<const BYTE*>(sd->position.lpvData);
				uStride = sd->position.dwStride;
			}
			if (pPos != nullptr)
			{
				const DWORD n = std::min<DWORD>(dwVertexCount, 64);
				float c[3] = { 0, 0, 0 };
				for (DWORD i = 0; i < n; i++)
				{
					const FLOAT* v = reinterpret_cast<const FLOAT*>(pPos + i * uStride);
					c[0] += v[0]; c[1] += v[1]; c[2] += v[2];
				}
				c[0] /= n; c[1] /= n; c[2] /= n;
				const FLOAT* W = m_World;
				const float w[3] = {
					c[0] * W[0] + c[1] * W[4] + c[2] * W[8] + W[12],
					c[0] * W[1] + c[1] * W[5] + c[2] * W[9] + W[13],
					c[0] * W[2] + c[1] * W[6] + c[2] * W[10] + W[14],
				};
				const FLOAT* V = m_WorldView;
				depth = w[0] * V[2] + w[1] * V[6] + w[2] * V[10] + V[14];
			}
		}
		fprintf(m_pRemixLog, "F%u %4u pt%d v%u zw%u bl%u d%.0f %s\n", m_dbg3DFrame, m_dbgFrameDraw, (int)pt, (unsigned)dwVertexCount,
			(unsigned)rsZW, (unsigned)rsBlend, depth, n0.empty() ? "?" : n0.c_str());
	}
	if (m_dbgSeenDraws.size() >= MAX_UNIQUE_DRAWS)
		return;

	if (m_pRemixLog == nullptr)
	{
		TCHAR szPath[MAX_PATH];
		PathCombine(szPath, Directory::GetEXEDirectory(), TEXT("d2gi_remix.log"));
		m_pRemixLog = _tfopen(szPath, TEXT("w"));
		if (m_pRemixLog == nullptr)
			return;
		fprintf(m_pRemixLog, "# D2GI Remix draw log. One line per unique draw signature.\n"
			"# st=stage state: op=COLOROP a1/a2=COLORARG1/2 tci=TEXCOORDINDEX ttf=TEXTURETRANSFORMFLAGS (hex)\n");
		fflush(m_pRemixLog);
	}

	const DWORD (&s)[2][32] = m_dbgStageState;
	D2GITexture* pTex0 = m_lpCurrentTextures[0];
	D2GITexture* pTex1 = m_lpCurrentTextures[1];
	const std::string tex0 = DescribeTexture(pTex0);
	const std::string tex1 = DescribeTexture(pTex1);

	char szKey[1024];
	sprintf_s(szKey, "pt%d fvf%03X %s%s | t0 %s op%X a%X/%X tci%X ttf%X | t1 %s op%X a%X/%X tci%X ttf%X",
		(int)pt, (unsigned)dwFVF, bStrided ? "strided" : "up", pIndexData != nullptr ? "+idx" : "",
		tex0.c_str(), s[0][D3D7::D3DTSS_COLOROP], s[0][D3D7::D3DTSS_COLORARG1], s[0][D3D7::D3DTSS_COLORARG2],
		s[0][D3D7::D3DTSS_TEXCOORDINDEX], s[0][D3D7::D3DTSS_TEXTURETRANSFORMFLAGS],
		tex1.c_str(), s[1][D3D7::D3DTSS_COLOROP], s[1][D3D7::D3DTSS_COLORARG1], s[1][D3D7::D3DTSS_COLORARG2],
		s[1][D3D7::D3DTSS_TEXCOORDINDEX], s[1][D3D7::D3DTSS_TEXTURETRANSFORMFLAGS]);

	if (!m_dbgSeenDraws.insert(szKey).second)
		return;

	const UVRange uv0 = ComputeUVRange(dwFVF, bStrided, pVertexData, dwVertexCount, 0);
	const UVRange uv1 = ComputeUVRange(dwFVF, bStrided, pVertexData, dwVertexCount, 1);

	fprintf(m_pRemixLog, "#%u draw%u verts%u idx%u %s\n    uv0 %s\n    uv1 %s\n",
		(unsigned)m_dbgSeenDraws.size(), m_dbgDrawCounter, (unsigned)dwVertexCount, (unsigned)dwIndexCount, szKey,
		DescribeUV(uv0).c_str(), DescribeUV(uv1).c_str());
	fprintf(m_pRemixLog, "    remix hash t0 %s t1 %s\n", RemixTextureHash(pTex0).c_str(), RemixTextureHash(pTex1).c_str());

	{
		// Position bounding box and material color, to find big flat surfaces like ground
		const BYTE* pPos = nullptr;
		UINT uPosStride = 0;
		if (bStrided)
		{
			auto* pData = static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
			pPos = static_cast<const BYTE*>(pData->position.lpvData);
			uPosStride = pData->position.dwStride;
		}
		else
		{
			pPos = static_cast<const BYTE*>(pVertexData);
			uPosStride = (UINT)CalcFVFStride(dwFVF);
		}
		float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
		if (pPos != nullptr)
		{
			const DWORD dwCount = std::min<DWORD>(dwVertexCount, 4096);
			for (DWORD v = 0; v < dwCount; v++)
			{
				const float* p = reinterpret_cast<const float*>(pPos + v * uPosStride);
				for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
			}
		}
		// Vertex diffuse color range (ARGB), to see what alpha the game gives effects like rain
		const BYTE* pCol = nullptr;
		UINT uColStride = 0;
		if (dwFVF & D3DFVF_DIFFUSE)
		{
			if (bStrided)
			{
				auto* pData = static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
				pCol = static_cast<const BYTE*>(pData->diffuse.lpvData);
				uColStride = pData->diffuse.dwStride;
			}
			else
			{
				UINT off = 0;
				switch (dwFVF & D3D7::D3DFVF_POSITION_MASK)
				{
				case D3DFVF_XYZ: off = 12; break;
				case D3DFVF_XYZRHW: off = 16; break;
				default: off = 12 + 4 * (((dwFVF & D3D7::D3DFVF_POSITION_MASK) - D3DFVF_XYZ) / 2); break;
				}
				if (dwFVF & D3DFVF_NORMAL) off += 12;
				if (dwFVF & D3DFVF_RESERVED1) off += 4;
				pCol = static_cast<const BYTE*>(pVertexData) + off;
				uColStride = (UINT)CalcFVFStride(dwFVF);
			}
		}
		BYTE cmn[4] = { 255, 255, 255, 255 }, cmx[4] = { 0, 0, 0, 0 };
		if (pCol != nullptr)
		{
			const DWORD dwCount = std::min<DWORD>(dwVertexCount, 4096);
			for (DWORD v = 0; v < dwCount; v++)
			{
				const BYTE* c = pCol + v * uColStride; // B G R A
				for (int k = 0; k < 4; k++) { cmn[k] = std::min(cmn[k], c[k]); cmx[k] = std::max(cmx[k], c[k]); }
			}
			fprintf(m_pRemixLog, "    vcolor A[%u..%u] R[%u..%u] G[%u..%u] B[%u..%u]\n",
				cmn[3], cmx[3], cmn[2], cmx[2], cmn[1], cmx[1], cmn[0], cmx[0]);
		}
		DWORD rsBlend = 0, rsSrc = 0, rsDst = 0, rsAT = 0, rsZW = 0, rsLight = 0;
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHABLENDENABLE, &rsBlend);
		m_pDev->GetRenderState(D3D9::D3DRS_SRCBLEND, &rsSrc);
		m_pDev->GetRenderState(D3D9::D3DRS_DESTBLEND, &rsDst);
		m_pDev->GetRenderState(D3D9::D3DRS_ALPHATESTENABLE, &rsAT);
		m_pDev->GetRenderState(D3D9::D3DRS_ZWRITEENABLE, &rsZW);
		m_pDev->GetRenderState(D3D9::D3DRS_LIGHTING, &rsLight);
		{
			D3D9::D3DMATRIX w, v;
			m_pDev->GetTransform(D3D9::D3DTS_WORLD, &w);
			m_pDev->GetTransform(D3D9::D3DTS_VIEW, &v);
			fprintf(m_pRemixLog, "    pos min(%.1f %.1f %.1f) max(%.1f %.1f %.1f) worldT(%.1f %.1f %.1f) viewT(%.1f %.1f %.1f) view00 %.2f\n",
				mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], w._41, w._42, w._43, v._41, v._42, v._43, v._11);
		}
		fprintf(m_pRemixLog, "    blend %u src%u dst%u alphatest %u zwrite %u lighting %u\n",
			(unsigned)rsBlend, (unsigned)rsSrc, (unsigned)rsDst, (unsigned)rsAT, (unsigned)rsZW, (unsigned)rsLight);

		const D3D7::D3DCOLORVALUE& d = m_CurrentMaterial.diffuse;
		fprintf(m_pRemixLog, "    size %.1f x %.1f x %.1f  matDiffuse(%.2f %.2f %.2f %.2f) diffSrc%u\n",
			mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], d.r, d.g, d.b, d.a, (unsigned)m_dwDiffuseMaterialSource);
	}

	for (int i = 0; i < 2; i++)
	{
		if (s[i][D3D7::D3DTSS_TEXTURETRANSFORMFLAGS] != 0 && m_dbgTexXformSet[i])
		{
			const FLOAT* m = m_dbgTexXform[i];
			fprintf(m_pRemixLog, "    texmat%d [%.3f %.3f | %.3f %.3f | %.3f %.3f]\n", i, m[0], m[1], m[4], m[5], m[8], m[9]);
		}
	}
	fflush(m_pRemixLog);
}


// ======= Remix: give untextured geometry a solid color texture =======

D3D9::IDirect3DTexture9* D2GI::GetSolidColorTexture(DWORD dwColor)
{
	auto it = m_SolidColorTextures.find(dwColor);
	if (it != m_SolidColorTextures.end())
		return it->second.Get();

	Microsoft::WRL::ComPtr<D3D9::IDirect3DTexture9> pTex;
	if (FAILED(m_pDev->CreateTexture(1, 1, 1, 0, D3D9::D3DFMT_A8R8G8B8, D3D9::D3DPOOL_MANAGED, pTex.GetAddressOf(), nullptr)))
		return nullptr;

	D3D9::D3DLOCKED_RECT lr;
	if (FAILED(pTex->LockRect(0, &lr, nullptr, 0)))
		return nullptr;
	*static_cast<DWORD*>(lr.pBits) = dwColor;
	pTex->UnlockRect(0);

	if (D2GIConfig::RemixDebugDrawsEnabled())
		Logger::Log(TEXT("Remix: created solid color texture 0x%08X"), dwColor);

	m_SolidColorTextures.emplace(dwColor, pTex);
	return pTex.Get();
}


namespace
{
	DWORD ColorValueToARGB(const D3D7::D3DCOLORVALUE& c)
	{
		auto ToByte = [](float f) -> DWORD { return (DWORD)(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
		return (ToByte(c.a) << 24) | (ToByte(c.r) << 16) | (ToByte(c.g) << 8) | ToByte(c.b);
	}

	// Offset of the DIFFUSE color inside a non-strided FVF vertex
	UINT FVFDiffuseOffset(DWORD dwFVF)
	{
		UINT uOffset;
		switch (dwFVF & 0x00E)
		{
			case 0x004: uOffset = 16; break;
			case 0x006: uOffset = 16; break;
			case 0x008: uOffset = 20; break;
			case 0x00A: uOffset = 24; break;
			case 0x00C: uOffset = 28; break;
			case 0x00E: uOffset = 32; break;
			default:    uOffset = 12; break;
		}
		if (dwFVF & 0x010) uOffset += 12; // NORMAL
		if (dwFVF & 0x020) uOffset += 4;  // PSIZE
		return uOffset;
	}

	// Average vertex color, quantized a little so near-identical colors share one texture
	DWORD AverageVertexColor(DWORD dwFVF, BOOL bStrided, VOID* pVertexData, DWORD dwVertexCount)
	{
		const BYTE* pBase;
		UINT uStride;
		if (bStrided)
		{
			auto* pData = static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
			pBase = static_cast<const BYTE*>(pData->diffuse.lpvData);
			uStride = pData->diffuse.dwStride;
		}
		else
		{
			pBase = static_cast<const BYTE*>(pVertexData) + FVFDiffuseOffset(dwFVF);
			uStride = (UINT)CalcFVFStride(dwFVF);
		}
		if (pBase == nullptr || dwVertexCount == 0)
			return 0xFFFFFFFF;

		uint64_t sum[4] = {};
		for (DWORD i = 0; i < dwVertexCount; i++)
		{
			const DWORD c = *reinterpret_cast<const DWORD*>(pBase + i * uStride);
			for (int k = 0; k < 4; k++)
				sum[k] += (c >> (k * 8)) & 0xFF;
		}
		DWORD dwResult = 0;
		for (int k = 0; k < 4; k++)
		{
			DWORD v = (DWORD)(sum[k] / dwVertexCount) & 0xF8; // 5 bits per channel
			dwResult |= v << (k * 8);
		}
		return dwResult;
	}
}


BOOL D2GI::IsDomeShaped(DWORD dwFVF, BOOL bStrided, VOID* pVertexData, DWORD dwVertexCount)
{
	// The ground plane is flat (one Z, Z is up); the dome spans height.
	const BYTE* pPos = nullptr;
	UINT uStride = 0;
	if (pVertexData == nullptr || dwVertexCount == 0)
		return FALSE;
	if (bStrided)
	{
		auto* pData = static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
		pPos = static_cast<const BYTE*>(pData->position.lpvData);
		uStride = pData->position.dwStride;
	}
	else
	{
		pPos = static_cast<const BYTE*>(pVertexData);
		uStride = (UINT)CalcFVFStride(dwFVF);
	}
	if (pPos == nullptr)
		return FALSE;
	float zMin = 1e30f, zMax = -1e30f;
	for (DWORD v = 0; v < dwVertexCount; v++)
	{
		const float z = reinterpret_cast<const float*>(pPos + v * uStride)[2];
		zMin = std::min(zMin, z);
		zMax = std::max(zMax, z);
	}
	return (zMax - zMin) > 1.0f;
}


BOOL D2GI::IsBackdropDraw(DWORD dwFVF)
{
	const bool bScreenSpace = (dwFVF & 0x00E) == 0x004;
	const bool bUntextured = m_lpCurrentTextures[0] == nullptr ||
		m_dbgStageState[0][D3D7::D3DTSS_COLOROP] == D3D7::D3DTOP_DISABLE;
	if (bScreenSpace || !bUntextured)
		return FALSE;
	DWORD dwZWrite = TRUE;
	m_pDev->GetRenderState(D3D9::D3DRS_ZWRITEENABLE, &dwZWrite);
	return dwZWrite == FALSE;
}


BOOL D2GI::DrawUntexturedWithColor(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
	DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags, BOOL bSkyDome)
{
	const bool bScreenSpace = (dwFVF & 0x00E) == 0x004; // XYZRHW: 2D/UI, leave alone
	const bool bUntextured = m_lpCurrentTextures[0] == nullptr ||
		m_dbgStageState[0][D3D7::D3DTSS_COLOROP] == D3D7::D3DTOP_DISABLE;
	if (bScreenSpace || !bUntextured || pVertexData == nullptr || dwVertexCount == 0)
		return FALSE;

	const UINT uTexCount = (dwFVF & 0xF00) >> 8;
	// Pick the color the fixed-function pipeline would have used as diffuse
	DWORD dwColor;
	if ((dwFVF & 0x040) && m_dwDiffuseMaterialSource == D3D9::D3DMCS_COLOR1)
		dwColor = AverageVertexColor(dwFVF, bStrided, pVertexData, dwVertexCount);
	else
		dwColor = ColorValueToARGB(m_CurrentMaterial.diffuse) & 0xF8F8F8F8;
	dwColor |= 0xFF000000; // opaque; transparency still comes from blend states

	// The sky dome gets one constant white texture (Remix hash CD6B1C920D3F662C, listed in rtx.skyBoxTextures)
	// and keeps its color through the texture stage, so its hash does not change with time of day and weather.
	D3D9::IDirect3DTexture9* pSolid = GetSolidColorTexture(bSkyDome ? 0xFFFFFFFF : dwColor);
	if (pSolid == nullptr)
		return FALSE;
	const bool bVertexColor = (dwFVF & 0x040) && m_dwDiffuseMaterialSource == D3D9::D3DMCS_COLOR1;

	// Geometry without texture coordinates gets (0,0) for every vertex
	static const FLOAT s_ZeroUV[2] = { 0.0f, 0.0f };
	D3D7::D3DDRAWPRIMITIVESTRIDEDDATA stridedCopy;
	VOID* pDrawData = pVertexData;
	DWORD dwDrawFVF = dwFVF;

	if (uTexCount == 0)
	{
		dwDrawFVF = (dwFVF & ~0xF00) | 0x100; // TEX1
		if (bStrided)
		{
			stridedCopy = *static_cast<D3D7::D3DDRAWPRIMITIVESTRIDEDDATA*>(pVertexData);
			stridedCopy.textureCoords[0].lpvData = const_cast<FLOAT*>(s_ZeroUV);
			stridedCopy.textureCoords[0].dwStride = 0;
			pDrawData = &stridedCopy;
		}
		else
		{
			const UINT uOldStride = (UINT)CalcFVFStride(dwFVF);
			const UINT uNewStride = uOldStride + 8;
			m_UntexturedVertexScratch.resize((size_t)uNewStride * dwVertexCount);
			BYTE* pDst = m_UntexturedVertexScratch.data();
			const BYTE* pSrc = static_cast<const BYTE*>(pVertexData);
			for (DWORD i = 0; i < dwVertexCount; i++)
			{
				CopyMemory(pDst + i * uNewStride, pSrc + i * uOldStride, uOldStride);
				CopyMemory(pDst + i * uNewStride + uOldStride, s_ZeroUV, 8);
			}
			pDrawData = pDst;
		}
	}

	const DWORD (&s)[2][32] = m_dbgStageState;
	m_pDev->SetTexture(0, pSolid);
	DWORD dwSavedTFactor = 0;
	if (bSkyDome)
	{
		// Remix lights the scene with the sky, so the game's dusk/night dome colors light it far too much.
		// Dim the dome by the same fog-luminance curve the sun uses (full at day, ~0.2 at 23:00, ~0.02 at 2:00).
		const DWORD dwScale = SkyDomeScale();
		const DWORD dwScaleColor = 0xFF000000 | (dwScale << 16) | (dwScale << 8) | dwScale;
		m_pDev->GetRenderState(D3D9::D3DRS_TEXTUREFACTOR, &dwSavedTFactor);
		m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_MODULATE);
		m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
		if (bVertexColor)
		{
			// texture * vertex color, then * the scale in stage 1
			m_pDev->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, dwScaleColor);
			m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		}
		else
		{
			m_pDev->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, ScaleColor(dwColor, dwScale));
			m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG2, D3DTA_TFACTOR);
		}
	}
	else
	{
		m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_SELECTARG1);
		m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
	}
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_SELECTARG1);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_TEXCOORDINDEX, 0);
	if (bSkyDome && bVertexColor)
	{
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_MODULATE);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG1, D3DTA_CURRENT);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG2, D3DTA_TFACTOR);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_SELECTARG1);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAARG1, D3DTA_CURRENT);
	}
	else
	{
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_DISABLE);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_DISABLE);
	}

	m_bInUntexturedFix = true;
	D2GITexture* pSavedTex0 = m_lpCurrentTextures[0];
	m_lpCurrentTextures[0] = nullptr; // no color key emulation for the solid texture
	DrawPrimitive(pt, dwDrawFVF, bStrided, pDrawData, dwVertexCount, pIndexData, dwIndexCount, dwFlags);
	m_lpCurrentTextures[0] = pSavedTex0;
	m_bInUntexturedFix = false;

	// Restore what the game had set
	m_pDev->SetTexture(0, pSavedTex0 != nullptr ? pSavedTex0->GetD3D9Texture() : nullptr);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, s[0][D3D7::D3DTSS_COLOROP]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, s[0][D3D7::D3DTSS_COLORARG1]);
	if (bSkyDome)
	{
		m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG2, s[0][D3D7::D3DTSS_COLORARG2]);
		m_pDev->SetRenderState(D3D9::D3DRS_TEXTUREFACTOR, dwSavedTFactor);
	}
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, s[0][D3D7::D3DTSS_ALPHAOP]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, s[0][D3D7::D3DTSS_ALPHAARG1]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_TEXCOORDINDEX, s[0][D3D7::D3DTSS_TEXCOORDINDEX]);
	if (bSkyDome && bVertexColor)
	{
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG1, s[1][D3D7::D3DTSS_COLORARG1]);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLORARG2, s[1][D3D7::D3DTSS_COLORARG2]);
		m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAARG1, s[1][D3D7::D3DTSS_ALPHAARG1]);
	}
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, s[1][D3D7::D3DTSS_COLOROP]);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, s[1][D3D7::D3DTSS_ALPHAOP]);

	return TRUE;
}


// ======= Remix: constant content for per-frame blit textures =======

VOID D2GI::FillStaticBlitTexture(D2GITexture* pDst)
{
	D3D9::IDirect3DSurface9* pDstSurf = pDst->GetD3D9Surface();
	if (pDstSurf == nullptr)
		return;

	D3D9::D3DSURFACE_DESC desc;
	if (FAILED(pDstSurf->GetDesc(&desc)))
		return;

	// One constant source surface per size/format, reused for every update
	const uint64_t key = (uint64_t(desc.Width) << 40) | (uint64_t(desc.Height) << 20) | uint64_t(desc.Format);
	auto it = m_StaticBlitSources.find(key);
	if (it == m_StaticBlitSources.end())
	{
		Microsoft::WRL::ComPtr<D3D9::IDirect3DSurface9> pSrc;
		if (FAILED(m_pDev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3D9::D3DPOOL_SYSTEMMEM, pSrc.GetAddressOf(), nullptr)))
			return;

		D3D9::D3DLOCKED_RECT lr;
		if (FAILED(pSrc->LockRect(&lr, nullptr, 0)))
			return;
		// Transparent black with a small signature in the first pixel, so the hash is unique and stable
		for (UINT y = 0; y < desc.Height; y++)
			ZeroMemory(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, lr.Pitch);
		if (desc.Format == D3D9::D3DFMT_A8R8G8B8 || desc.Format == D3D9::D3DFMT_X8R8G8B8)
			*static_cast<DWORD*>(lr.pBits) = 0x01524149; // "RAI\x01"
		else
			*static_cast<WORD*>(lr.pBits) = 0x4149;
		// Windshield size, observed in Remix: hash 1AB38C53610F8621 (mat_1AB38C53610F8621).
		// Other sizes hash differently because the hash covers the whole image.
		pSrc->UnlockRect();

		Logger::Log(TEXT("Remix: static content for blit texture %ux%u fmt %u"), desc.Width, desc.Height, (unsigned)desc.Format);
		it = m_StaticBlitSources.emplace(key, pSrc).first;
	}

	m_pDev->UpdateSurface(it->second.Get(), nullptr, pDstSurf, nullptr);
}


// ======= Remix: a sun that follows the game's time of day and weather =======
// The game has no sun light: daylight is ambient and vertex color, so under Remix the world is lit by the sky
// only. The fog color follows time of day and weather (WEATHER.INI fog_* tables: ~255 255 230 on a good day,
// ~84 80 76 on a bad day, ~40 81 102 / 17 22 25 at night), so it drives a directional light that RTX Remix turns
// into a distant light: color = fog color normalized, brightness = SunStrength% * luminance^2.
// Fog-luminance curve shared by the sun and the sky dome: (lum / 0.6)^2, clamped to [0.02, 1].
FLOAT D2GI::NightFactor() const
{
	const float r = ((m_dwFogColor >> 16) & 0xFF) / 255.0f;
	const float g = ((m_dwFogColor >> 8) & 0xFF) / 255.0f;
	const float b = (m_dwFogColor & 0xFF) / 255.0f;
	const float lum = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 0.6f;
	return std::min(1.0f, std::max(0.02f, lum * lum));
}


DWORD D2GI::SkyDomeScale() const
{
	const DWORD dwScale = (DWORD)(NightFactor() * 255.0f + 0.5f);
	static DWORD s_dwLogged = 0xFFFFFFFF;
	static int s_nLogged = 0;
	if (dwScale != s_dwLogged && s_nLogged < 30)
	{
		s_dwLogged = dwScale;
		s_nLogged++;
		Logger::Log(TEXT("Remix: sky dome brightness %u/255 for fog 0x%06X"), (unsigned)dwScale,
			(unsigned)(m_dwFogColor & 0xFFFFFF));
	}
	return dwScale;
}


DWORD D2GI::ScaleColor(DWORD dwColor, DWORD dwScale)
{
	DWORD dwOut = dwColor & 0xFF000000;
	for (int shift = 0; shift < 24; shift += 8)
		dwOut |= ((((dwColor >> shift) & 0xFF) * dwScale / 255) & 0xFF) << shift;
	return dwOut;
}


// Game clock (king.exe 8.1): hour of day as a double at [*0x6F33AC + 0x58] (0..24, fractional). The game reads it
// at 0x550562 to switch the automatic night lights (on before 5:00 and after 22:00).
static bool ReadGameHour(double* pHour)
{
	static int s_iKnownExe = -1;
	if (s_iKnownExe < 0)
	{
		const BYTE* pBase = reinterpret_cast<const BYTE*>(GetModuleHandle(nullptr));
		const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
		s_iKnownExe = (pBase == reinterpret_cast<const BYTE*>(0x400000) &&
			pNt->FileHeader.TimeDateStamp == 0x400502EA) ? 1 : 0;
	}
	if (!s_iKnownExe)
		return false;
	__try
	{
		const DWORD dwClock = *reinterpret_cast<volatile DWORD*>(0x6F33AC);
		if (dwClock < 0x10000)
			return false;
		const double h = *reinterpret_cast<volatile double*>(dwClock + 0x58);
		if (!(h >= 0.0 && h <= 48.0))
			return false;
		*pHour = std::fmod(h, 24.0);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}


VOID D2GI::SetRemixSun()
{
	m_bSunSetThisFrame = true;

	const float r = ((m_dwFogColor >> 16) & 0xFF) / 255.0f;
	const float g = ((m_dwFogColor >> 8) & 0xFF) / 255.0f;
	const float b = (m_dwFogColor & 0xFF) / 255.0f;
	const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	const float maxc = std::max(r, std::max(g, b));
	const float k = (maxc > 0.0f ? 1.0f / maxc : 0.0f) * lum * lum * (D2GIConfig::RemixSunStrength() / 100.0f);

	D3D9::D3DLIGHT9 sun = {};
	sun.Type = D3D9::D3DLIGHT_DIRECTIONAL;
	sun.Diffuse = { r * k, g * k, b * k, 1.0f };
	sun.Specular = sun.Diffuse;
	// Z is up in this game (road ~20, lamps ~50). Default: sun from the side, ~55 degrees above the horizon.
	float dx = 0.45f, dy = 0.35f, dz = -0.82f;
	double hour = 0.0;
	if (D2GIConfig::RemixSunFollowsTimeEnabled() && ReadGameHour(&hour))
	{
		// Day from 5:00 to 22:00 (the game's own night-lights hours), noon at 13:30. The sun rises in +X, goes over
		// -Y and sets in -X; its height follows a sine with 60 degrees at noon. At night it stays low (8 degrees)
		// on the side where it set or will rise: its color from the fog is dark then anyway.
		const double kPi = 3.14159265358979;
		// Step the sun every 0.05 h (3 game minutes), so Remix does not see a new distant light every frame
		const double hq = std::floor(hour * 20.0) / 20.0;
		const double t = (hq - 5.0) / 17.0;                         // 0 sunrise .. 1 sunset
		const double tc = std::min(std::max(t, 0.0), 1.0);
		const double az = kPi * tc;                                  // 0 east .. pi west
		const double el = std::max(8.0, 60.0 * std::sin(kPi * tc)) * kPi / 180.0;
		// Direction the light travels: from the sun towards the ground
		dx = (float)(-std::cos(az) * std::cos(el));
		dy = (float)(std::sin(az) * std::cos(el));
		dz = (float)(-std::sin(el));
		// A low sun shines through more air: dimmer and warmer. Without this, slopes facing a sun near the horizon
		// got full noon light and looked white (owner, 31.10). f = 1 at 60 degrees, ~0.16 at 8 degrees.
		const float f = (float)(std::sin(el) / std::sin(60.0 * kPi / 180.0));
		const float att = 0.3f + 0.7f * f;
		sun.Diffuse.r *= att;
		sun.Diffuse.g *= att * (0.8f + 0.2f * f);
		sun.Diffuse.b *= att * (0.55f + 0.45f * f);
		sun.Specular = sun.Diffuse;
		static int s_nLastHour = -1;
		if ((int)hour != s_nLastHour)
		{
			s_nLastHour = (int)hour;
			Logger::Log(TEXT("Remix: sun for %.2f h: dir (%.2f %.2f %.2f)"), hour, dx, dy, dz);
		}
	}
	const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
	sun.Direction = { dx / len, dy / len, dz / len };
	sun.Range = 10000.0f;
	m_pDev->SetLight(REMIX_SUN_LIGHT_INDEX, &sun);
	m_pDev->LightEnable(REMIX_SUN_LIGHT_INDEX, TRUE);

	static DWORD s_dwLoggedFog = 0xFFFFFFFF;
	static int s_nLogged = 0;
	if (m_dwFogColor != s_dwLoggedFog && s_nLogged < 30)
	{
		s_dwLoggedFog = m_dwFogColor;
		s_nLogged++;
		Logger::Log(TEXT("Remix: sun from fog 0x%06X -> (%.2f %.2f %.2f)"), (unsigned)(m_dwFogColor & 0xFFFFFF),
			sun.Diffuse.r, sun.Diffuse.g, sun.Diffuse.b);
	}
}


// ======= Remix: the player's headlights =======
// The game lights the road in front of the player's truck only with ambient and vertex color, so under Remix the
// road ahead is dark at night. From the driver's view (pribor.txr drawn in the previous frame) and when the fog
// says it is dark, put two spot lights in front of the camera. The world camera is the view matrix the game sets
// for the world (row vectors: p_view = p_world * R + t), so the camera axes in world space are the rows of R^-1.
// The player's headlight switch, read from king.exe 8.1 (Steam, PE timestamp 0x400502EA):
// player vehicle = *(*(0x6D2078) + 0x268); vehicle+0x51B8 = 0 off, 1 low beam, 2 high beam (L toggles, Ctrl+L low/high,
// see the key handler at 0x597D36); vehicle+0x51B4 = automatic lights the game sets by the hour (0x55057A).
// Returns -1 when the game version is unknown or the vehicle is not there (menus, loading).
static int ReadGameDword(DWORD dwAddr, DWORD* pOut)
{
	__try
	{
		*pOut = *reinterpret_cast<volatile DWORD*>(dwAddr);
		return 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

static int ReadPlayerHeadlightSwitch(DWORD* pAuto)
{
	static int s_iKnownExe = -1;
	if (s_iKnownExe < 0)
	{
		const BYTE* pBase = reinterpret_cast<const BYTE*>(GetModuleHandle(nullptr));
		const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
		s_iKnownExe = (pBase == reinterpret_cast<const BYTE*>(0x400000) &&
			pNt->FileHeader.TimeDateStamp == 0x400502EA) ? 1 : 0;
		Logger::Log(TEXT("Remix: headlight switch %s"), s_iKnownExe ? TEXT("read from king.exe 8.1")
			: TEXT("unknown for this king.exe; Headlights follow the time of day"));
	}
	if (!s_iKnownExe)
		return -1;

	DWORD dwGame = 0, dwVeh = 0, dwSwitch = 0;
	if (!ReadGameDword(0x6D2078, &dwGame) || dwGame < 0x10000)
		return -1;
	if (!ReadGameDword(dwGame + 0x268, &dwVeh) || dwVeh < 0x10000)
		return -1;
	if (!ReadGameDword(dwVeh + 0x51B8, &dwSwitch) || dwSwitch > 2)
		return -1;
	if (pAuto != nullptr && !ReadGameDword(dwVeh + 0x51B4, pAuto))
		*pAuto = 0;
	return (int)dwSwitch;
}


static int CopyGameMemory(void* pDst, DWORD dwSrc, DWORD dwSize)
{
	__try
	{
		memcpy(pDst, reinterpret_cast<const void*>(dwSrc), dwSize);
		return 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}


// DebugDraws: on dump frames, write the world view matrix and the player vehicle object to
// d2gi_vehicle_<frame>.bin (64 bytes view, then 0x6000 bytes of the vehicle) to find its transform offline.
VOID D2GI::DumpPlayerVehicle(const FLOAT* v)
{
	if (!D2GIConfig::RemixDebugDrawsEnabled() || !IsDumpFrame() || m_dbgVehicleDumpFrame == m_dbg3DFrame)
		return;
	m_dbgVehicleDumpFrame = m_dbg3DFrame;
	DWORD dwGame = 0, dwVeh = 0;
	if (!ReadGameDword(0x6D2078, &dwGame) || dwGame < 0x10000 || !ReadGameDword(dwGame + 0x268, &dwVeh) || dwVeh < 0x10000)
		return;
	static BYTE s_buf[0x6000];
	if (!CopyGameMemory(s_buf, dwVeh, sizeof(s_buf)))
		return;
	if (m_dbg3DFrame == 1800 || m_dbg3DFrame == 4200)
		ScanForVehicles(dwVeh);
	char name[64];
	sprintf_s(name, "d2gi_vehicle_%u.bin", m_dbg3DFrame);
	FILE* f = nullptr;
	if (fopen_s(&f, name, "wb") == 0 && f != nullptr)
	{
		fwrite(v, sizeof(FLOAT), 16, f);
		fwrite(&dwVeh, 4, 1, f);
		fwrite(s_buf, 1, sizeof(s_buf), f);
		fclose(f);
	}
}


VOID D2GI::SetRemixHeadlights(const FLOAT* v)
{
	m_bHeadlightsSetThisFrame = true;
	DumpPlayerVehicle(v);

	DWORD dwAuto = 0;
	const int iSwitch = ReadPlayerHeadlightSwitch(&dwAuto);
	static int s_iLoggedSwitch = -2;
	static DWORD s_dwLoggedAuto = 0xFFFFFFFF;
	static int s_nSwitchLogs = 0;
	if ((iSwitch != s_iLoggedSwitch || dwAuto != s_dwLoggedAuto) && s_nSwitchLogs < 50)
	{
		s_iLoggedSwitch = iSwitch;
		s_dwLoggedAuto = dwAuto;
		s_nSwitchLogs++;
		Logger::Log(TEXT("Remix: player headlight switch %d (auto %u)"), iSwitch, (unsigned)dwAuto);
	}
	// Unknown exe: fall back to "on when dark"
	const bool bSwitchOn = iSwitch < 0 ? NightFactor() < 0.35f : iSwitch > 0;
	(void)bSwitchOn;
	UpdateVehicleList();

	float P[12];
	const bool bHavePlayer = ReadPlayerMatrix(P);
	const DWORD dwPlayer = PlayerVehiclePtr();

	// Vehicles with their lights on, nearest to the player first
	struct Lit { size_t idx; float d2; int beam; };
	static std::vector<Lit> s_lit;
	s_lit.clear();
	for (size_t k = 0; k < m_Vehicles.size(); k++)
	{
		const TrackedVehicle& tv = m_Vehicles[k];
		if (tv.misses > 10)
			continue;
		int beam = 0;
		if (tv.ptr == dwPlayer)
			beam = iSwitch < 0 ? (NightFactor() < 0.35f ? 1 : 0) : iSwitch;
		else
		{
			DWORD sw = 0, au = 0;
			ReadGameDword(tv.ptr + 0x51B8, &sw);
			ReadGameDword(tv.ptr + 0x51B4, &au);
			beam = sw == 2 ? 2 : ((sw != 0 || au != 0) ? 1 : 0);
		}
		if (beam == 0)
			continue;
		float d2 = 0.0f;
		if (bHavePlayer)
		{
			const float dx = tv.M[9] - P[9], dy = tv.M[10] - P[10], dz = tv.M[11] - P[11];
			d2 = dx * dx + dy * dy + dz * dz;
			// 1000: with DrawDistance x3, cars farther than the old 600 are visible and lost their light
			if (d2 > 1000.0f * 1000.0f)
				continue;
		}
		s_lit.push_back({ k, d2, beam });
	}
	std::sort(s_lit.begin(), s_lit.end(), [](const Lit& a, const Lit& b) { return a.d2 < b.d2; });
	if (s_lit.size() > (size_t)REMIX_MAX_LIT_VEHICLES)
		s_lit.resize(REMIX_MAX_LIT_VEHICLES);

	// Remix ignores the diffuse level here: with constant attenuation it converts a D3D light into a light whose
	// intensity grows with Range^2 (times rtx.lightConversionIntensityFactor). So the strength drives the range:
	// 100 = range 300 for the low beam, 450 for the high beam.
	const float s = D2GIConfig::RemixHeadlights() / 100.0f;
	int nSlots = 0;
	for (const Lit& lit : s_lit)
	{
		const TrackedVehicle& tv = m_Vehicles[lit.idx];
		const bool bHigh = lit.beam == 2;
		const float* right = tv.M;
		const float* fwd = tv.M + 3;
		const float* up = tv.M + 6;
		const float* pos = tv.M + 9;
		const float tilt = bHigh ? 0.03f : 0.12f;
		float dir[3] = { fwd[0] - up[0] * tilt, fwd[1] - up[1] * tilt, fwd[2] - up[2] * tilt };
		const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (dl > 0.0f) { dir[0] /= dl; dir[1] /= dl; dir[2] /= dl; }

		D3D9::D3DLIGHT9 L = {};
		L.Type = D3D9::D3DLIGHT_SPOT;
		L.Diffuse = { 1.0f, 0.95f, 0.85f, 1.0f };
		L.Specular = L.Diffuse;
		L.Direction = { dir[0], dir[1], dir[2] };
		L.Range = (bHigh ? 450.0f : 300.0f) * std::sqrt(s);
		L.Falloff = 1.0f;
		L.Attenuation0 = 1.0f;
		L.Theta = bHigh ? 0.25f : 0.4f;  // full-brightness cone
		L.Phi = bHigh ? 0.5f : 0.8f;     // outer cone

		// Lamps at the vehicle's learned lamp positions, a little in front of the bumper
		for (int side = 0; side < 2; side++)
		{
			const float* l = tv.lamp[side];
			const float ly = l[1] + 0.6f;
			L.Position = {
				pos[0] + right[0] * l[0] + fwd[0] * ly + up[0] * l[2],
				pos[1] + right[1] * l[0] + fwd[1] * ly + up[1] * l[2],
				pos[2] + right[2] * l[0] + fwd[2] * ly + up[2] * l[2],
			};
			const DWORD slot = REMIX_HEADLIGHT_BASE + nSlots++;
			m_pDev->SetLight(slot, &L);
			m_pDev->LightEnable(slot, TRUE);
		}
	}
	for (int k = nSlots; k < m_nHeadlightSlots; k++)
		m_pDev->LightEnable(REMIX_HEADLIGHT_BASE + k, FALSE);
	m_nHeadlightSlots = nSlots;

	static int s_nMaxLogged = -1;
	if ((int)s_lit.size() > s_nMaxLogged)
	{
		s_nMaxLogged = (int)s_lit.size();
		Logger::Log(TEXT("Remix: headlights on %d vehicle(s) (%u tracked)"), (int)s_lit.size(), (unsigned)m_Vehicles.size());
	}
}


// Player vehicle transform (king.exe 8.1): vehicle+0x204 = 3 rows of 3 floats (local X right, Y forward, Z up in
// world space), vehicle+0x228 = position. Found by dumping the vehicle while driving (d2gi_vehicle_*.bin).
// Every vehicle class in king.exe shares this layout and the light flags at +0x51B4 / +0x51B8.
static bool ReadVehicleMatrix(DWORD dwVeh, FLOAT* M)
{
	if (!CopyGameMemory(M, dwVeh + 0x204, 12 * sizeof(FLOAT)))
		return false;
	for (int i = 0; i < 12; i++)
		if (!std::isfinite(M[i]))
			return false;
	for (int r = 0; r < 3; r++)
	{
		const float l = M[r * 3] * M[r * 3] + M[r * 3 + 1] * M[r * 3 + 1] + M[r * 3 + 2] * M[r * 3 + 2];
		if (l < 0.95f || l > 1.05f)
			return false;
	}
	return fabsf(M[9]) < 1.0e6f && fabsf(M[10]) < 1.0e6f && fabsf(M[11]) < 1.0e5f;
}


DWORD D2GI::PlayerVehiclePtr()
{
	if (ReadPlayerHeadlightSwitch(nullptr) < 0)
		return 0;
	DWORD dwGame = 0, dwVeh = 0;
	if (!ReadGameDword(0x6D2078, &dwGame) || dwGame < 0x10000 || !ReadGameDword(dwGame + 0x268, &dwVeh) || dwVeh < 0x10000)
		return 0;
	return dwVeh;
}


bool D2GI::ReadPlayerMatrix(FLOAT* M)
{
	const DWORD dwVeh = PlayerVehiclePtr();
	return dwVeh != 0 && ReadVehicleMatrix(dwVeh, M);
}


// Vehicle classes by vtable (king.exe 8.1): tractor trucks (the player's too), cars, buses/vans. Trailers
// (0x64CBFC) have no headlights. Default lamp positions per class until a vehicle's own glows are seen.
static const DWORD kVehicleVtables[3] = { 0x64CDF8, 0x64CFF4, 0x65089C };
static const float kDefaultLamps[3][2][3] = {
	{ { -0.9f, 6.1f, 0.05f }, { 0.9f, 6.1f, 0.05f } },
	{ { -0.65f, 2.2f, 0.0f }, { 0.65f, 2.2f, 0.0f } },
	{ { -1.0f, 5.0f, 0.1f }, { 1.0f, 5.0f, 0.1f } },
};

static int VehicleClass(DWORD vt)
{
	for (int i = 0; i < 3; i++)
		if (kVehicleVtables[i] == vt)
			return i;
	return -1;
}


// Keeps m_Vehicles up to date: drops entries whose object no longer looks like a vehicle, and walks a few MB of
// the game's heap per frame looking for new ones (a full pass takes a few seconds).
VOID D2GI::UpdateVehicleList()
{
	if (ReadPlayerHeadlightSwitch(nullptr) < 0)
		return;

	for (size_t k = 0; k < m_Vehicles.size();)
	{
		TrackedVehicle& tv = m_Vehicles[k];
		DWORD vt = 0;
		if (!ReadGameDword(tv.ptr, &vt) || vt != tv.vt)
		{
			m_Vehicles[k] = m_Vehicles.back();
			m_Vehicles.pop_back();
			continue;
		}
		// A transform that fails the sanity check for a frame or two (e.g. mid-update) used to drop the vehicle,
		// and it came back only when the memory scan reached it again, seconds later: its lights went out for that
		// time (owner, 29.10). Keep it with its last good transform; give up after ~2 s of bad reads.
		float M[12];
		if (ReadVehicleMatrix(tv.ptr, M))
		{
			CopyMemory(tv.M, M, sizeof(M));
			tv.misses = 0;
		}
		else if (++tv.misses > 120)
		{
			m_Vehicles[k] = m_Vehicles.back();
			m_Vehicles.pop_back();
			continue;
		}
		k++;
	}

	const DWORD dwPlayer = PlayerVehiclePtr();
	if (dwPlayer != 0)
		AddVehicle(dwPlayer);

	static DWORD s_dwSelfBase = 0, s_dwSelfEnd = 0;
	if (s_dwSelfBase == 0)
	{
		MEMORY_BASIC_INFORMATION mbi;
		HMODULE hSelf = nullptr;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCTSTR>(&kVehicleVtables), &hSelf);
		s_dwSelfBase = (DWORD)(size_t)hSelf;
		const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const BYTE*>(hSelf) + reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf)->e_lfanew);
		s_dwSelfEnd = s_dwSelfBase + pNt->OptionalHeader.SizeOfImage;
		(void)mbi;
	}

	size_t budget = 4 * 1024 * 1024;
	MEMORY_BASIC_INFORMATION mbi;
	while (budget > 0)
	{
		if (m_dwVehicleScanAddr < 0x10000 || m_dwVehicleScanAddr >= 0x7FFE0000)
			m_dwVehicleScanAddr = 0x10000;
		if (VirtualQuery(reinterpret_cast<LPCVOID>((size_t)m_dwVehicleScanAddr), &mbi, sizeof(mbi)) != sizeof(mbi))
		{
			m_dwVehicleScanAddr = 0x10000;
			break;
		}
		const DWORD dwRegionBase = (DWORD)(size_t)mbi.BaseAddress;
		const DWORD dwRegionEnd = dwRegionBase + (DWORD)mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xFF;
		const bool bScan = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
			(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE) && !(mbi.Protect & PAGE_GUARD) &&
			!(dwRegionBase >= s_dwSelfBase && dwRegionBase < s_dwSelfEnd);
		if (!bScan)
		{
			m_dwVehicleScanAddr = dwRegionEnd;
			continue;
		}
		const DWORD dwFrom = std::max(m_dwVehicleScanAddr, dwRegionBase) & ~3u;
		const DWORD dwTo = (DWORD)std::min<size_t>((size_t)dwRegionEnd, (size_t)dwFrom + budget);
		ScanRangeForVehicles(dwFrom, dwTo);
		budget -= std::min<size_t>(budget, (size_t)(dwTo - dwFrom) + 4096);
		m_dwVehicleScanAddr = dwTo;
	}
}


// Collects addresses in [dwFrom, dwTo) whose dword is a vehicle vtable. Plain C so it can use SEH.
static int FindVehicleVtables(DWORD dwFrom, DWORD dwTo, DWORD* pOut, int nMax)
{
	int n = 0;
	__try
	{
		const DWORD* d = reinterpret_cast<const DWORD*>((size_t)dwFrom);
		const DWORD* e = reinterpret_cast<const DWORD*>((size_t)dwTo);
		for (; d + (0x234 / 4) < e && n < nMax; d++)
		{
			const DWORD v = *d;
			if (v == kVehicleVtables[0] || v == kVehicleVtables[1] || v == kVehicleVtables[2])
				pOut[n++] = (DWORD)(size_t)d;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return n;
}


VOID D2GI::ScanRangeForVehicles(DWORD dwFrom, DWORD dwTo)
{
	DWORD found[256];
	const int n = FindVehicleVtables(dwFrom, dwTo, found, 256);
	for (int k = 0; k < n; k++)
		AddVehicle(found[k]);
}


VOID D2GI::AddVehicle(DWORD ptr)
{
	for (const TrackedVehicle& tv : m_Vehicles)
		if (tv.ptr == ptr)
			return;
	if (m_Vehicles.size() >= 256)
		return;
	TrackedVehicle tv = {};
	tv.ptr = ptr;
	if (!ReadGameDword(ptr, &tv.vt))
		return;
	const int cls = VehicleClass(tv.vt);
	if (cls < 0 || !ReadVehicleMatrix(ptr, tv.M))
		return;
	CopyMemory(tv.lamp, kDefaultLamps[cls], sizeof(tv.lamp));
	if (m_ClassLampSamples[cls] > 0)
		CopyMemory(tv.lamp, m_ClassLamp[cls], sizeof(tv.lamp));
	m_Vehicles.push_back(tv);
}


// An lg_head.txr quad: if it sits at the front of a tracked vehicle, average its center (in that vehicle's
// coordinates) into the vehicle's left or right lamp and return true.
bool D2GI::LearnVehicleLamp(DWORD dwFVF, BOOL bStrided, VOID* pVertexData, DWORD dwVertexCount)
{
	if (bStrided || pVertexData == nullptr || dwVertexCount == 0 || (dwFVF & 0x00E) != 0x002 || m_Vehicles.empty())
		return false;

	const UINT uStride = (UINT)CalcFVFStride(dwFVF);
	float c[3] = { 0, 0, 0 };
	for (DWORD i = 0; i < dwVertexCount; i++)
	{
		const FLOAT* p = reinterpret_cast<const FLOAT*>(static_cast<const BYTE*>(pVertexData) + i * uStride);
		c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
	}
	c[0] /= dwVertexCount; c[1] /= dwVertexCount; c[2] /= dwVertexCount;
	const FLOAT* W = m_World;
	const float w[3] = {
		c[0] * W[0] + c[1] * W[4] + c[2] * W[8] + W[12],
		c[0] * W[1] + c[1] * W[5] + c[2] * W[9] + W[13],
		c[0] * W[2] + c[1] * W[6] + c[2] * W[10] + W[14],
	};

	TrackedVehicle* best = nullptr;
	float bestL[3] = { 0, 0, 0 }, bestD = 1e30f;
	for (TrackedVehicle& tv : m_Vehicles)
	{
		const float d[3] = { w[0] - tv.M[9], w[1] - tv.M[10], w[2] - tv.M[11] };
		float l[3];
		for (int r = 0; r < 3; r++)
			l[r] = d[0] * tv.M[r * 3] + d[1] * tv.M[r * 3 + 1] + d[2] * tv.M[r * 3 + 2];
		if (fabsf(l[0]) > 2.0f || l[1] < 0.5f || l[1] > 10.0f || fabsf(l[2]) > 3.0f)
			continue;
		const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (dd < bestD) { bestD = dd; best = &tv; CopyMemory(bestL, l, sizeof(l)); }
	}
	if (best == nullptr)
		return false;

	const int side = bestL[0] < 0.0f ? 0 : 1;
	int& n = best->samples[side];
	float* lamp = best->lamp[side];
	// Once a lamp is known, ignore glows far from it: a glow of an untracked car just ahead also lands in the box
	// and used to drag our lamp (and our headlight) forward, away from the truck (owner, 29.10).
	if (n >= 20)
	{
		const float e[3] = { bestL[0] - lamp[0], bestL[1] - lamp[1], bestL[2] - lamp[2] };
		if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] > 0.5f * 0.5f)
			return true;
	}
	if (n == 0)
		CopyMemory(lamp, bestL, sizeof(bestL));
	else if (n < 1000)
	{
		const float k = 1.0f / (float)(n + 1);
		for (int i = 0; i < 3; i++)
			lamp[i] += (bestL[i] - lamp[i]) * k;
	}
	if (n < 1000)
		n++;
	const int cls = VehicleClass(best->vt);
	if (cls >= 0 && n == 20)
	{
		CopyMemory(m_ClassLamp[cls][side], lamp, sizeof(float) * 3);
		m_ClassLampSamples[cls]++;
		static int s_nLogged = 0;
		if (s_nLogged++ < 20)
			Logger::Log(TEXT("Remix: vehicle %08X (class %d) %s lamp at (%.2f %.2f %.2f)"), (unsigned)best->ptr, cls,
				side == 0 ? TEXT("left") : TEXT("right"), lamp[0], lamp[1], lamp[2]);
	}
	return true;
}


// A glow texture of our own for the player's headlight quads: the quads sample a narrow vertical strip
// (u 0.45..0.55), so the content is a soft warm-white glow fading towards the ends of the strip.
// Its Remix hash is logged; it is listed in rtx.particleTextures but not in rtx.lightConverter.
D3D9::IDirect3DTexture9* D2GI::GetPlayerGlowTexture()
{
	if (m_pPlayerGlowTex)
		return m_pPlayerGlowTex.Get();
	if (m_bPlayerGlowFailed)
		return nullptr;
	m_bPlayerGlowFailed = true;

	const UINT N = 64;
	Microsoft::WRL::ComPtr<D3D9::IDirect3DTexture9> pTex;
	if (FAILED(m_pDev->CreateTexture(N, N, 1, 0, D3D9::D3DFMT_A8R8G8B8, D3D9::D3DPOOL_MANAGED, pTex.GetAddressOf(), nullptr)))
		return nullptr;
	D3D9::D3DLOCKED_RECT lr;
	if (FAILED(pTex->LockRect(0, &lr, nullptr, 0)))
		return nullptr;
	static DWORD s_pixels[64 * 64];
	for (UINT y = 0; y < N; y++)
	{
		DWORD* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch);
		for (UINT x = 0; x < N; x++)
		{
			const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
			const float du = fabsf(u - 0.5f) * 2.0f, dv = fabsf(v - 0.5f) * 2.0f;
			float a = (1.0f - du) * (1.0f - dv * dv);
			a = std::max(0.0f, std::min(1.0f, a));
			const DWORD A = (DWORD)(a * 255.0f + 0.5f);
			const DWORD px = (A << 24) | (0xFF << 16) | (0xF6 << 8) | 0xE6;
			row[x] = px;
			s_pixels[y * N + x] = px;
		}
	}
	pTex->UnlockRect(0);
	const unsigned long long h = XXH3_64bits(s_pixels, sizeof(s_pixels));
	Logger::Log(TEXT("Remix: player headlight glow texture created, Remix hash 0x%08X%08X"),
		(unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFF));
	m_pPlayerGlowTex = pTex;
	m_bPlayerGlowFailed = false;
	return m_pPlayerGlowTex.Get();
}


VOID D2GI::RestoreTexture0(D2GITexture* pTex)
{
	m_pDev->SetTexture(0, pTex != nullptr ? pTex->GetD3D9Texture() : nullptr);
}


// DebugDraws: find vehicle-like objects. Walks committed read-write private memory and lists every address
// whose dword is a vtable in king.exe (.rdata) and whose +0x204 holds three unit rows and a position near the
// player (within 3000 units). Also lists dwords that point at the player vehicle (containers).
VOID D2GI::ScanForVehicles(DWORD dwPlayer)
{
	DWORD dwPlayerVt = 0;
	ReadGameDword(dwPlayer, &dwPlayerVt);
	float P[12];
	if (!CopyGameMemory(P, dwPlayer + 0x204, sizeof(P)))
		return;
	char name[64];
	sprintf_s(name, "d2gi_vehscan_%u.txt", m_dbg3DFrame);
	FILE* f = nullptr;
	if (fopen_s(&f, name, "w") != 0 || f == nullptr)
		return;
	fprintf(f, "player %08X vtable %08X pos %.1f %.1f %.1f\n", (unsigned)dwPlayer, (unsigned)dwPlayerVt, P[9], P[10], P[11]);

	int nObj = 0, nRef = 0;
	static std::vector<DWORD> s_objs;
	static std::vector<DWORD> s_arrays;
	s_objs.clear();
	s_arrays.clear();
	MEMORY_BASIC_INFORMATION mbi;
	BYTE* p = reinterpret_cast<BYTE*>(0x10000);
	while (p < reinterpret_cast<BYTE*>(0x7FFE0000) && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		BYTE* next = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xFF;
		if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
			(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE) && !(mbi.Protect & PAGE_GUARD))
		{
			const DWORD* d = static_cast<const DWORD*>(mbi.BaseAddress);
			const size_t n = mbi.RegionSize / 4;
			for (size_t i = 0; i < n; i++)
			{
				const DWORD v = d[i];
				const DWORD addr = (DWORD)(size_t)(d + i);
				if (v == dwPlayer && nRef < 400)
				{
					fprintf(f, "ref %08X\n", (unsigned)addr);
					nRef++;
				}
				if (v < 0x64B000 || v >= 0x66C000 || i + (0x234 / 4) >= n)
					continue;
				const float* m = reinterpret_cast<const float*>(d + i + 0x204 / 4);
				bool ok = true;
				for (int r = 0; r < 3 && ok; r++)
				{
					const float l = m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2];
					ok = std::isfinite(l) && l > 0.98f && l < 1.02f;
				}
				if (!ok || !std::isfinite(m[9]) || !std::isfinite(m[10]) || !std::isfinite(m[11]))
					continue;
				const float dx = m[9] - P[9], dy = m[10] - P[10], dz = m[11] - P[11];
				if (dx * dx + dy * dy + dz * dz > 3000.0f * 3000.0f)
					continue;
				if (nObj < 400)
				{
					fprintf(f, "obj %08X vt %08X pos %.1f %.1f %.1f fwd %.2f %.2f %.2f\n", (unsigned)addr, (unsigned)v,
						m[9], m[10], m[11], m[3], m[4], m[5]);
					nObj++;
					s_objs.push_back(addr);
				}
			}
		}
		p = next;
	}
	fprintf(f, "end objs %d refs %d\n", nObj, nRef);

	// Second pass: every dword that points at a found object (containers of vehicles)
	int nRefs2 = 0;
	p = reinterpret_cast<BYTE*>(0x10000);
	while (!s_objs.empty() && p < reinterpret_cast<BYTE*>(0x7FFE0000) && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		BYTE* next = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xFF;
		if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
			(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE) && !(mbi.Protect & PAGE_GUARD))
		{
			const DWORD* d = static_cast<const DWORD*>(mbi.BaseAddress);
			const size_t n = mbi.RegionSize / 4;
			for (size_t i = 0; i < n && nRefs2 < 3000; i++)
			{
				const DWORD v = d[i];
				if (v < 0x10000000 && v > 0x00800000)
					; // objects are on the heap; keep the check cheap
				for (DWORD o : s_objs)
				{
					if (v == o)
					{
						const DWORD at = (DWORD)(size_t)(d + i);
						fprintf(f, "ref2 %08X -> %08X\n", (unsigned)at, (unsigned)o);
						nRefs2++;
						// start of a run of >= 3 object pointers: remember it as an array
						if (i >= 1 && i + 2 < n && std::find(s_objs.begin(), s_objs.end(), d[i - 1]) == s_objs.end() &&
							std::find(s_objs.begin(), s_objs.end(), d[i + 1]) != s_objs.end() &&
							std::find(s_objs.begin(), s_objs.end(), d[i + 2]) != s_objs.end())
							s_arrays.push_back(at);
						break;
					}
				}
			}
		}
		p = next;
	}
	fprintf(f, "end refs2 %d\n", nRefs2);

	// Third pass: who points at (or just before) each array start
	int nRefs3 = 0;
	p = reinterpret_cast<BYTE*>(0x10000);
	while (!s_arrays.empty() && p < reinterpret_cast<BYTE*>(0x7FFE0000) && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		BYTE* next = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
		const DWORD prot = mbi.Protect & 0xFF;
		if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
			(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE) && !(mbi.Protect & PAGE_GUARD))
		{
			const DWORD* d = static_cast<const DWORD*>(mbi.BaseAddress);
			const size_t n = mbi.RegionSize / 4;
			for (size_t i = 0; i < n && nRefs3 < 500; i++)
			{
				const DWORD v = d[i];
				for (DWORD a : s_arrays)
				{
					if (v <= a && v + 0x10 >= a)
					{
						const DWORD at = (DWORD)(size_t)(d + i);
						fprintf(f, "ref3 %08X -> %08X (array %08X)  next dwords %08X %08X %08X\n", (unsigned)at, (unsigned)v, (unsigned)a,
							i + 1 < n ? (unsigned)d[i + 1] : 0u, i + 2 < n ? (unsigned)d[i + 2] : 0u, i + 3 < n ? (unsigned)d[i + 3] : 0u);
						nRefs3++;
						break;
					}
				}
			}
		}
		p = next;
	}
	fprintf(f, "end refs3 %d\n", nRefs3);
	fclose(f);
}


unsigned long long D2GI::CachedTextureHash(D2GITexture* pTex)
{
	auto it = m_TexHashCache.find(pTex);
	if (it != m_TexHashCache.end())
		return it->second;
	unsigned long long h = 0;
	D3D9::IDirect3DTexture9* pD3DTex = pTex->GetD3D9Texture();
	D3D9::D3DSURFACE_DESC desc;
	if (pD3DTex != nullptr && SUCCEEDED(pD3DTex->GetLevelDesc(0, &desc)))
	{
		const UINT bpp = BytesPerPixel(desc.Format);
		D3D9::D3DLOCKED_RECT lr;
		if (bpp != 0 && SUCCEEDED(pD3DTex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
		{
			XXH3_state_t* st = XXH3_createState();
			XXH3_64bits_reset(st);
			for (UINT y = 0; y < desc.Height; y++)
				XXH3_64bits_update(st, static_cast<const BYTE*>(lr.pBits) + (size_t)y * lr.Pitch, (size_t)desc.Width * bpp);
			h = XXH3_64bits_digest(st);
			XXH3_freeState(st);
			pD3DTex->UnlockRect(0);
		}
	}
	m_TexHashCache[pTex] = h;
	return h;
}


// A copy of a road texture with every channel at 3/4 (wet surfaces are darker). Only 16-bit 565 and 32-bit
// formats; all mip levels are copied. Its top level hashes differently from the original, and mod.usda has a wet
// material for that hash (computed offline the same way: see remix-mod/README.md).
D3D9::IDirect3DTexture9* D2GI::GetWetTexture(D2GITexture* pSrc, unsigned long long qwHash)
{
	auto it = m_WetTextures.find(qwHash);
	if (it != m_WetTextures.end())
		return it->second.Get();
	m_WetTextures[qwHash] = nullptr;
	D3D9::IDirect3DTexture9* pSrcTex = pSrc->GetD3D9Texture();
	if (pSrcTex == nullptr)
		return nullptr;
	D3D9::D3DSURFACE_DESC desc;
	if (FAILED(pSrcTex->GetLevelDesc(0, &desc)))
		return nullptr;
	const bool b565 = desc.Format == D3D9::D3DFMT_R5G6B5;
	const bool b8888 = desc.Format == D3D9::D3DFMT_X8R8G8B8 || desc.Format == D3D9::D3DFMT_A8R8G8B8;
	if (!b565 && !b8888)
	{
		Logger::Log(TEXT("Remix: WetRoads: road texture %016llX has format %u, not handled"), qwHash, (unsigned)desc.Format);
		return nullptr;
	}
	const DWORD dwLevels = pSrcTex->GetLevelCount();
	Microsoft::WRL::ComPtr<D3D9::IDirect3DTexture9> pWet;
	if (FAILED(m_pDev->CreateTexture(desc.Width, desc.Height, dwLevels, 0, desc.Format, D3D9::D3DPOOL_MANAGED, &pWet, nullptr)))
		return nullptr;
	for (DWORD lv = 0; lv < dwLevels; lv++)
	{
		D3D9::D3DSURFACE_DESC ld;
		D3D9::D3DLOCKED_RECT s, d;
		if (FAILED(pSrcTex->GetLevelDesc(lv, &ld)) || FAILED(pSrcTex->LockRect(lv, &s, nullptr, D3DLOCK_READONLY)))
			return nullptr;
		if (FAILED(pWet->LockRect(lv, &d, nullptr, 0)))
		{
			pSrcTex->UnlockRect(lv);
			return nullptr;
		}
		for (UINT y = 0; y < ld.Height; y++)
		{
			const BYTE* sp = static_cast<const BYTE*>(s.pBits) + (size_t)y * s.Pitch;
			BYTE* dp = static_cast<BYTE*>(d.pBits) + (size_t)y * d.Pitch;
			for (UINT x = 0; x < ld.Width; x++)
			{
				if (b565)
				{
					const WORD v = reinterpret_cast<const WORD*>(sp)[x];
					const WORD r = ((v >> 11) & 31) * 3 / 4, g = ((v >> 5) & 63) * 3 / 4, b = (v & 31) * 3 / 4;
					reinterpret_cast<WORD*>(dp)[x] = (WORD)((r << 11) | (g << 5) | b);
				}
				else
				{
					const DWORD v = reinterpret_cast<const DWORD*>(sp)[x];
					const DWORD r = ((v >> 16) & 255) * 3 / 4, g = ((v >> 8) & 255) * 3 / 4, b = (v & 255) * 3 / 4;
					reinterpret_cast<DWORD*>(dp)[x] = (v & 0xFF000000) | (r << 16) | (g << 8) | b;
				}
			}
		}
		pWet->UnlockRect(lv);
		pSrcTex->UnlockRect(lv);
	}
	m_WetTextures[qwHash] = pWet;
	// Log the copy's content hash so mod.usda can be checked against it
	D3D9::D3DLOCKED_RECT r;
	unsigned long long wetHash = 0;
	if (SUCCEEDED(pWet->LockRect(0, &r, nullptr, D3DLOCK_READONLY)))
	{
		const UINT bpp = b565 ? 2 : 4;
		XXH3_state_t* st = XXH3_createState();
		XXH3_64bits_reset(st);
		for (UINT y = 0; y < desc.Height; y++)
			XXH3_64bits_update(st, static_cast<const BYTE*>(r.pBits) + (size_t)y * r.Pitch, (size_t)desc.Width * bpp);
		wetHash = XXH3_64bits_digest(st);
		XXH3_freeState(st);
		pWet->UnlockRect(0);
	}
	Logger::Log(TEXT("Remix: WetRoads: wet copy of %016llX is %016llX (%ux%u, %u levels)"), qwHash, wetHash,
		(unsigned)desc.Width, (unsigned)desc.Height, (unsigned)dwLevels);
	return pWet.Get();
}
