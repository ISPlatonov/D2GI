#pragma once

#include <mutex>

// The game creates textures and system-memory surfaces from a loader thread while the main thread renders
// and creates its own resources. Under the RTX Remix bridge, two creation calls in flight at the same time
// can both fail with "no response from server" (D3DERR_INVALIDCALL), which D2GI treated as fatal: the game
// crashed when a new region loaded. Every D3D9 resource creation takes this lock, so they run one at a time.
inline std::recursive_mutex& D2GICreateLock()
{
	static std::recursive_mutex s_mutex;
	return s_mutex;
}
