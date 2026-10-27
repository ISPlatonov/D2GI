
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
#include <iterator>
#include <cmath>
#include <string>

#define XXH_INLINE_ALL
#include "../thirdparty/xxhash/xxhash.h"

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
			m_pDev->SetTransform(D3D9::D3DTS_WORLD, (D3D9::D3DMATRIX*)pMatrix);
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
	m_pDev->SetLight(i, (D3D9::D3DLIGHT9*)pLight);
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


static bool IsWindshieldGlass(D2GITexture* pTex)
{
	if (pTex == nullptr)
		return false;
	const std::string& name = pTex->GetDebugName();
	static const char kName[] = "glass.txr";
	const size_t n = sizeof(kName) - 1;
	if (name.size() < n || _stricmp(name.c_str() + name.size() - n, kName) != 0)
		return false;
	return name.size() == n || name[name.size() - n - 1] == '\\' || name[name.size() - n - 1] == '/';
}


VOID D2GI::DrawPrimitive(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
	DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags)
{
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


BOOL D2GI::DrawUntexturedWithColor(D3D7::D3DPRIMITIVETYPE pt, DWORD dwFVF, BOOL bStrided, VOID* pVertexData,
	DWORD dwVertexCount, WORD* pIndexData, DWORD dwIndexCount, DWORD dwFlags)
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

	D3D9::IDirect3DTexture9* pSolid = GetSolidColorTexture(dwColor);
	if (pSolid == nullptr)
		return FALSE;

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
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_SELECTARG1);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_COLORARG1, D3DTA_TEXTURE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_SELECTARG1);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_TEXCOORDINDEX, 0);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_COLOROP, D3D9::D3DTOP_DISABLE);
	m_pDev->SetTextureStageState(1, D3D9::D3DTSS_ALPHAOP, D3D9::D3DTOP_DISABLE);

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
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAOP, s[0][D3D7::D3DTSS_ALPHAOP]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_ALPHAARG1, s[0][D3D7::D3DTSS_ALPHAARG1]);
	m_pDev->SetTextureStageState(0, D3D9::D3DTSS_TEXCOORDINDEX, s[0][D3D7::D3DTSS_TEXCOORDINDEX]);
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
