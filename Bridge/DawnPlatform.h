#pragma once
// Dawn's platform for the app (DawnPlatform.cpp): worker threads and the shader/pipeline cache on disk.
#include <string>

namespace dawn::platform {
	class Platform;
}

namespace moonlight_xbox_dx {
	// Lives as long as the app (Dawn's instance keeps a pointer to it).
	dawn::platform::Platform* MakeDawnPlatform(const std::wstring& cacheFolder);
	std::string DawnCacheStats(dawn::platform::Platform* platform);
}
