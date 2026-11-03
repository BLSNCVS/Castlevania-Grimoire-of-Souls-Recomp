// SymbolImageBundleWrapper: renders an SF Symbol (the controller button names
// from GCControllerBundleWrapper) to PNG bytes that the game loads into a
// Texture2D. Drawn here with GDI as white glyphs on transparency, 4x
// supersampled; the game does not free the buffer, so images are cached.
#include "common.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace
{
    // --- PNG (RGBA8, stored deflate blocks) ---
    uint32_t Crc(const uint8_t* p, size_t n)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t v = i;
                for (int k = 0; k < 8; k++)
                    v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
                table[i] = v;
            }
            init = true;
        }
        uint32_t c = 0xffffffffu;
        for (size_t i = 0; i < n; i++)
            c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
        return c ^ 0xffffffffu;
    }

    void Be32(std::vector<uint8_t>& o, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8)
            o.push_back(uint8_t(v >> s));
    }

    void Chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data)
    {
        Be32(o, uint32_t(data.size()));
        size_t start = o.size();
        o.insert(o.end(), type, type + 4);
        o.insert(o.end(), data.begin(), data.end());
        Be32(o, Crc(o.data() + start, o.size() - start));
    }

    std::vector<uint8_t> EncodePng(int w, int h, const std::vector<uint8_t>& rgba)
    {
        std::vector<uint8_t> raw;
        for (int y = 0; y < h; y++)
        {
            raw.push_back(0);  // filter: none
            raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
        }
        std::vector<uint8_t> z = { 0x78, 0x01 };
        size_t pos = 0;
        do
        {
            size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
            z.push_back(pos + n >= raw.size() ? 1 : 0);
            z.push_back(uint8_t(n));
            z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n));
            z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            pos += n;
        } while (pos < raw.size());
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        Be32(z, (b << 16) | a);

        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        std::vector<uint8_t> ihdr;
        Be32(ihdr, uint32_t(w));
        Be32(ihdr, uint32_t(h));
        ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });  // 8-bit RGBA
        Chunk(png, "IHDR", ihdr);
        Chunk(png, "IDAT", z);
        Chunk(png, "IEND", {});
        return png;
    }

    // --- glyphs ---
    enum class Shape { Circle, RoundedBottom, RoundedTop, DPad, Stick, Plain };

    // PlayStation face symbols and the menu icon are drawn as vectors so they
    // do not depend on a symbol font being installed
    enum class Mark { None, Cross, Ring, Square, Triangle, Menu, View, Home };

    struct Glyph
    {
        Shape shape = Shape::Plain;
        Mark mark = Mark::None;
        std::wstring label;
        int dpadHighlight = -1;  // 0 up, 1 down, 2 left, 3 right
        bool pressed = false;
    };

    bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
    bool Contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

    std::wstring Upper(const std::string& s)
    {
        std::wstring out;
        for (char c : s)
            out += wchar_t(c >= 'a' && c <= 'z' ? c - 32 : c);
        return out;
    }

    Glyph Describe(const std::string& name)
    {
        Glyph g;
        std::string head = name.substr(0, name.find('.'));
        if (StartsWith(name, "dpad"))
        {
            g.shape = Shape::DPad;
            g.dpadHighlight = Contains(name, ".up") ? 0 : Contains(name, ".down") ? 1 : Contains(name, ".left") ? 2 : Contains(name, ".right") ? 3 : -1;
        }
        else if (Contains(name, "joystick"))
        {
            g.shape = Shape::Stick;
            g.label = head == "r" ? L"R" : L"L";
            g.pressed = Contains(name, "press");
        }
        else if (Contains(name, "rectangle.roundedbottom") || Contains(name, "rectangle.roundedtop"))
        {
            g.shape = Contains(name, "roundedbottom") ? Shape::RoundedBottom : Shape::RoundedTop;
            g.label = Upper(head);
        }
        else
        {
            g.shape = Contains(name, "circle") ? Shape::Circle : Shape::Plain;
            if (head == "xmark") g.mark = Mark::Cross;
            else if (head == "circle") g.mark = Mark::Ring;
            else if (head == "square") g.mark = Mark::Square;
            else if (head == "triangle") g.mark = Mark::Triangle;
            else if (StartsWith(name, "line.3.horizontal") || StartsWith(name, "line.horizontal.3")) g.mark = Mark::Menu;
            else if (StartsWith(name, "rectangle.on.rectangle")) g.mark = Mark::View;
            else if (head == "plus") g.label = L"+";
            else if (head == "minus") g.label = L"-";
            else if (head == "house") g.mark = Mark::Home;
            else if (name == "xbox.logo") { g.shape = Shape::Circle; g.label = L"X"; }
            else if (name == "logo.playstation") { g.shape = Shape::Circle; g.label = L"PS"; }
            else g.label = Upper(head);
        }
        return g;
    }

    std::vector<uint8_t> Render(const Glyph& g, int w, int h)
    {
        const int s = 4, W = w * s, H = h * s;
        std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bmp || !bits)
        {
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
            return rgba;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        ZeroMemory(bits, size_t(W) * H * 4);

        int m = W / 16, stroke = W / 14, half = stroke / 2;
        HPEN pen = CreatePen(PS_SOLID, stroke, RGB(255, 255, 255));
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RECT text = { m, m, W - m, H - m };

        switch (g.shape)
        {
        case Shape::Circle:
        case Shape::Stick:
            Ellipse(dc, m + half, m + half, W - m - half, H - m - half);
            if (g.shape == Shape::Stick && g.pressed)
            {
                SelectObject(dc, white);
                Ellipse(dc, W / 2 - W / 10, H - m - H / 5, W / 2 + W / 10, H - m - H / 10);
            }
            break;
        case Shape::RoundedBottom:
        case Shape::RoundedTop:
        {
            // outline = filled outer shape minus filled inner shape
            int top = H / 5, bottom = H - H / 5;
            auto fill = [&](int inset, HBRUSH brush) {
                RECT r = { m + inset, top + inset, W - m - inset, bottom - inset };
                HRGN round = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, W / 2 - inset, H / 2 - inset);
                int mid = (r.top + r.bottom) / 2;
                HRGN square = g.shape == Shape::RoundedBottom ? CreateRectRgn(r.left, r.top, r.right, mid) : CreateRectRgn(r.left, mid, r.right, r.bottom);
                CombineRgn(round, round, square, RGN_OR);
                FillRgn(dc, round, brush);
                DeleteObject(square);
                DeleteObject(round);
            };
            fill(0, white);
            fill(stroke, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            text = { m, top, W - m, bottom };
            break;
        }
        case Shape::DPad:
        {
            int a = W / 3, b = 2 * W / 3, t = m, e = W - m;
            RECT arms[4] = { { a, t, b, a }, { a, b, b, e }, { t, a, a, b }, { b, a, e, b } };
            for (int i = 0; i < 4; i++)
            {
                SelectObject(dc, i == g.dpadHighlight ? static_cast<HGDIOBJ>(white) : GetStockObject(NULL_BRUSH));
                Rectangle(dc, arms[i].left, arms[i].top, arms[i].right, arms[i].bottom);
            }
            break;
        }
        case Shape::Plain:
            break;
        }

        if (g.mark != Mark::None)
        {
            HPEN thick = CreatePen(PS_SOLID, stroke, RGB(255, 255, 255));
            SelectObject(dc, thick);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            int cx = W / 2, cy = H / 2, r = W / 5;
            switch (g.mark)
            {
            case Mark::Cross:
                MoveToEx(dc, cx - r, cy - r, nullptr); LineTo(dc, cx + r, cy + r);
                MoveToEx(dc, cx + r, cy - r, nullptr); LineTo(dc, cx - r, cy + r);
                break;
            case Mark::Ring:
                Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
                break;
            case Mark::Square:
                Rectangle(dc, cx - r, cy - r, cx + r, cy + r);
                break;
            case Mark::Triangle:
            {
                POINT pts[3] = { { cx, cy - r - r / 5 }, { cx + r + r / 6, cy + r - r / 6 }, { cx - r - r / 6, cy + r - r / 6 } };
                Polygon(dc, pts, 3);
                break;
            }
            case Mark::Menu:
                for (int i = -1; i <= 1; i++)
                {
                    MoveToEx(dc, cx - r, cy + i * r * 2 / 3, nullptr);
                    LineTo(dc, cx + r, cy + i * r * 2 / 3);
                }
                break;
            case Mark::View:
                Rectangle(dc, cx - r, cy - r * 2 / 3, cx + r / 3, cy + r / 3);
                Rectangle(dc, cx - r / 3, cy - r / 3, cx + r, cy + r * 2 / 3);
                break;
            case Mark::Home:
            {
                POINT pts[5] = { { cx, cy - r }, { cx + r, cy }, { cx + r * 2 / 3, cy + r }, { cx - r * 2 / 3, cy + r }, { cx - r, cy } };
                Polygon(dc, pts, 5);
                break;
            }
            case Mark::None:
                break;
            }
            SelectObject(dc, pen);
            DeleteObject(thick);
        }

        if (!g.label.empty())
        {
            int len = int(g.label.size());
            int height = (text.bottom - text.top) * (len > 1 ? 5 : 6) / 10;
            if (len > 2)
                height = height * 2 / len + height / 4;
            HFONT font = CreateFontW(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, g.label.c_str(), len, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }
        GdiFlush();

        // box-filter down; brightness becomes the white glyph's alpha
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                unsigned sum = 0;
                for (int dy = 0; dy < s; dy++)
                {
                    for (int dx = 0; dx < s; dx++)
                    {
                        const uint8_t* p = src + (size_t(y * s + dy) * W + (x * s + dx)) * 4;
                        sum += (p[0] + p[1] + p[2]) / 3;
                    }
                }
                uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
                d[0] = d[1] = d[2] = 255;
                d[3] = uint8_t(sum / (s * s));
            }
        }

        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldBmp);
        DeleteObject(pen);
        DeleteObject(white);
        DeleteObject(bmp);
        DeleteDC(dc);
        return rgba;
    }

    SRWLOCK g_lock = SRWLOCK_INIT;
}

// Returns the PNG's size in bytes, or 0 when nothing can be drawn.
GOSR_EXPORT int GetSymbolImage(void** data, int width, int height, const char* name)
{
    if (data)
        *data = nullptr;
    if (!data || !name || width <= 0 || height <= 0 || width > 1024 || height > 1024)
        return 0;
    static std::map<std::string, std::vector<uint8_t>> cache;
    std::string key = std::string(name) + "@" + std::to_string(width) + "x" + std::to_string(height);
    AcquireSRWLockExclusive(&g_lock);
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.emplace(key, EncodePng(width, height, Render(Describe(name), width, height))).first;
    *data = it->second.data();
    int size = int(it->second.size());
    ReleaseSRWLockExclusive(&g_lock);
    return size;
}
