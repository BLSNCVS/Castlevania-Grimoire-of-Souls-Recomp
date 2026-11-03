// GameCenterAccessPointBundleWrapper: the Game Center access point overlay.
// There is no Game Center on Windows; the access point is never shown.
#include "common.h"

namespace
{
    void WriteRect(float* x, float* y, float* w, float* h, float rx, float ry, float rw, float rh)
    {
        if (x) *x = rx;
        if (y) *y = ry;
        if (w) *w = rw;
        if (h) *h = rh;
    }
}

GOSR_EXPORT void _showAccessPoint(int) {}
GOSR_EXPORT void _hideAccessPoint() {}
GOSR_EXPORT void _showHighlights(int) {}
GOSR_EXPORT void _getAccessPointFrameInScreenCoord(float* x, float* y, float* w, float* h) { WriteRect(x, y, w, h, 0, 0, 0, 0); }
GOSR_EXPORT void _getMainScreenBounds(float* x, float* y, float* w, float* h)
{
    WriteRect(x, y, w, h, 0, 0, float(GetSystemMetrics(SM_CXSCREEN)), float(GetSystemMetrics(SM_CYSCREEN)));
}
GOSR_EXPORT int _isPresentingGameCenter() { return 0; }
GOSR_EXPORT int _isVisible() { return 0; }
GOSR_EXPORT void _trigger() {}
