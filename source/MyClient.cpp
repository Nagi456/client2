#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include "launcher/PidOption.h"
#include "MinecraftDataProvider.h"
#include "AutoRq.h"
#include "PingMonitor.h"
#include "TexturePacks.h"
#include "PackInstall.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ============================================================
// MyClient - current build
// External Windows overlay prototype for Minecraft Bedrock.
//
// Current features:
//   - Keystrokes HUD
//   - CPS Counter (LMB/RMB)
//   - Live armor durability HUD
//   - Arrow Counter (requires a game data adapter)
//   - Minecraft render FPS
//   - Block Outline foundation
//   - HUD editor (drag / wheel resize)
//   - Feature shortcuts
//   - Settings
//   - Hive party invitation detection/whitelist acceptance (live test pending)
//
// Minecraft data/event connection for Armor, Arrows, FPS and Zoom.
// 3D Block Outline projection
// is intentionally left as a connection layer.
//
// Block Outline currently has a data reception hook:
//     UpdateBlockOutlineData(...)
//     ClearBlockOutlineData()
//
// The actual Minecraft-side target block data will be connected
// later without changing the rest of the client structure.
// ============================================================


// ------------------------- Features --------------------------

// Everything starts OFF on a fresh install. Toggles are saved to
// myclient_config.txt (next to MyClient.exe) and restored on every start.
bool keystrokesEnabled = false;
bool cpsEnabled = false;
bool armorHudEnabled = false;
bool arrowCounterEnabled = false;
bool resourceCounterEnabled = false;
bool pingEnabled = false;
// Feature List: shows the names of the features that are switched on
bool featureListEnabled = false;

// Texture pack switching (Textures page)
bool texturesEnabled = false;
// Auto RQ: queue the next Hive game when you die. mode = index into MyClientAutoRq::kModes
bool autoRqEnabled = false;
int autoRqMode = 0;
uint32_t texturesSeq = 0;            // bumped after every list rewrite; the DLL reloads when it changes
TexturePacks::State texState;
bool texScanned = false;
int texScroll = 0;
std::wstring texStatus;
bool fpsEnabled = false;
bool partyAutoAcceptEnabled = false;
bool blockOutlineEnabled = false;
bool zoomEnabled = false;

// Full Bright: keeps the game's night-vision brightness on while enabled.
bool fullbrightEnabled = false;
bool nametagEnabled = false;

// True while the configured Zoom key is physically held.
// The DLL validates this heartbeat before applying a temporary FOV change.
bool zoomActive = false;

enum class MenuCategory
{
    Feature,
    HUD,
    Settings
};

MenuCategory selectedCategory =
    MenuCategory::Feature;

bool menuOpen = false;
bool hudEditMode = false;
ULONGLONG editorNoticeUntil = 0;   // shows "positions reset" in the HUD editor toolbar for a moment
bool gameActive = true; // Minecraft (or this overlay) is the foreground window


// ------------------------- Detail Feature -------------------

enum class DetailFeature
{
    None,
    Keystrokes,
    CPS,
    Armor,
    Arrows,
    FPS,
    PartyAutoAccept,
    BlockOutline,
    Zoom,
    Resources,
    Fullbright,
    Nametag,
    Ping,
    Textures,
    AutoRq,
    FeatureList
};

DetailFeature detailFeature =
    DetailFeature::None;


HWND minecraftWindow = nullptr;
HWND overlayWindow = nullptr;

// Almost-invisible window under the G menu. A colour-keyed layered window lets the
// mouse pass through its transparent pixels, so without this the game kept the
// pointer (hidden cursor, camera still turning) everywhere outside the menu.
HWND catcherWindow = nullptr;

bool previousGState = false;


// ------------------------- Shortcuts -------------------------

struct ShortcutSettings
{
    int keystrokes = 0;
    int cps = 0;
    int armor = 0;
    int arrows = 0;
    int fps = 0;
    int party = 0;
    int blockOutline = 0;
    int zoom = 'C';
};

ShortcutSettings shortcuts;

bool waitingForShortcut = false;

struct BlockOutlineSettings
{
    COLORREF color = RGB(80, 180, 255);
    int lineWidth = 2;
    bool showOnlyWhenTargeted = true;
};

BlockOutlineSettings blockOutlineSettings;

// Colours offered in the Block Outline details screen (6 per row).
const COLORREF kOutlinePalette[12] =
{
    RGB(255,255,255), RGB(255, 60, 60), RGB(255,150, 40), RGB(255,220, 50),
    RGB(140,230, 60), RGB( 50,200,100), RGB( 60,220,230), RGB( 80,180,255),
    RGB( 60,100,255), RGB(150, 90,255), RGB(255,100,190), RGB(  0,  0,  0)
};

// The settings (details) page is laid out for x = 535. It is drawn shifted left by this much so it
// fills the page by itself (no feature list beside it); mouse coordinates are shifted back.
constexpr int kDetailShift = 345;

// Swatch rectangle for palette entry i, relative to the details origin (535, 70).
RECT OutlineSwatchRect(int i)
{
    const int col = i % 6;
    const int row = i / 6;
    const int x = 535 + col * 54;
    const int y = 70 + 138 + row * 42;
    return RECT{x, y, x + 46, y + 34};
}


// ------------------------- Minecraft Data -------------------

// Minecraft-specific state lives behind MinecraftDataProvider.
// The UI/HUD only consumes provider data, so provider updates do
// not require rewriting the rest of MyClient.

void UpdateBlockOutlineData(
    const MinecraftDataProvider::BlockOutlineData& data)
{
    MinecraftDataProvider::UpdateBlockOutlineData(data);
}

void UpdateBlockOutlineScreenData(
    const MinecraftDataProvider::BlockOutlineScreenData& data)
{
    MinecraftDataProvider::UpdateBlockOutlineScreenData(data);
}

void ClearBlockOutlineData()
{
    MinecraftDataProvider::ClearBlockOutlineData();
}

const MinecraftDataProvider::BlockOutlineData& GetCurrentBlockOutline()
{
    return MinecraftDataProvider::GetBlockOutlineData();
}

const MinecraftDataProvider::BlockOutlineScreenData& GetCurrentBlockOutlineScreen()
{
    return MinecraftDataProvider::GetBlockOutlineScreenData();
}


// ------------------------- HUD -------------------------------

struct HudElement
{
    int x;
    int y;
    float scale;
};


HudElement keystrokesHud{
    25,
    130,
    1.0f
};

HudElement cpsHud{
    25,
    235,
    1.0f
};

HudElement armorHud{
    25,
    315,
    1.0f
};

HudElement arrowHud{
    25,
    455,
    1.0f
};

HudElement fpsHud{
    25,
    505,
    1.0f
};

HudElement resourceHud{
    25,
    555,
    1.0f
};

HudElement pingHud{
    190,
    505,
    1.0f
};

HudElement featureListHud{
    450,
    25,
    1.0f
};


enum class HudElementId
{
    None,
    Keystrokes,
    CPS,
    Armor,
    Arrows,
    FPS,
    Resources,
    Ping,
    FeatureList
};


HudElementId selectedHud =
    HudElementId::None;

HudElementId draggingHud =
    HudElementId::None;

POINT dragOffset{};


// ------------------------- Party settings -------------------

std::vector<std::wstring> partyPlayers;

std::wstring partyInput;

// Feature search box (top of the Feature page).
std::wstring searchQuery;
bool searchActive = false;

// Favourite features (DetailFeature values), saved in the config file.
std::vector<int> favoriteFeatures;

bool partyAddingPlayer = false;


// ------------------------- Settings --------------------------

std::wstring clientName =
    L"MyClient";

float guiScale = 1.0f;

bool menuAnimation = true;

// Menu colour theme (index into kThemes), saved in the config file.
int themeIndex = 0;

// True only while the G menu itself is being drawn (not the in-game HUD).
bool menuDrawing = false;

bool MenuLightMode();
COLORREF MenuTextColor(COLORREF c);
COLORREF MenuBoxColor(COLORREF c);


// ------------------------- CPS -------------------------------

std::vector<
    std::chrono::steady_clock::time_point
> leftClicks;

std::vector<
    std::chrono::steady_clock::time_point
> rightClicks;


// ------------------------- Paths -----------------------------

std::wstring GetExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH
    );

    return std::filesystem::path(path)
        .parent_path()
        .wstring();
}


std::wstring ConfigPath()
{
    return GetExecutableDirectory()
        + L"\\myclient_config.txt";
}


// ------------------------- Helpers ---------------------------

bool IsInside(
    int mx,
    int my,
    int x,
    int y,
    int w,
    int h)
{
    return mx >= x &&
           mx <= x + w &&
           my >= y &&
           my <= y + h;
}


std::wstring KeyName(int vk)
{
    if (vk == 0)
        return L"Not Set";

    if (vk == VK_SPACE)
        return L"SPACE";

    if (vk == VK_RETURN)
        return L"ENTER";

    if (vk == VK_TAB)
        return L"TAB";

    if (vk == VK_SHIFT)
        return L"SHIFT";

    if (vk == VK_CONTROL)
        return L"CTRL";

    if (vk == VK_MENU)
        return L"ALT";

    if (vk == VK_ESCAPE)
        return L"ESC";

    if (vk == VK_BACK)
        return L"BACKSPACE";

    if (vk == VK_LEFT)
        return L"LEFT";

    if (vk == VK_RIGHT)
        return L"RIGHT";

    if (vk == VK_UP)
        return L"UP";

    if (vk == VK_DOWN)
        return L"DOWN";

    if (vk == VK_DELETE)
        return L"DELETE";

    if (vk == VK_INSERT)
        return L"INSERT";

    if (vk == VK_HOME)
        return L"HOME";

    if (vk == VK_END)
        return L"END";

    if (vk == VK_PRIOR)
        return L"PAGEUP";

    if (vk == VK_NEXT)
        return L"PAGEDOWN";

    if (vk >= VK_F1 &&
        vk <= VK_F24)
    {
        return L"F" +
            std::to_wstring(
                vk - VK_F1 + 1
            );
    }

    UINT scan =
        MapVirtualKeyW(
            static_cast<UINT>(vk),
            MAPVK_VK_TO_VSC
        );

    if (scan)
    {
        LONG lp =
            static_cast<LONG>(
                scan << 16
            );

        wchar_t name[128]{};

        if (GetKeyNameTextW(
                lp,
                name,
                128) > 0)
        {
            return name;
        }
    }

    if (vk >= 32 &&
        vk <= 126)
    {
        return std::wstring(
            1,
            static_cast<wchar_t>(vk)
        );
    }

    return L"VK " +
        std::to_wstring(vk);
}


bool IsReservedShortcut(int vk)
{
    return vk == 'G' ||
           vk == VK_ESCAPE;
}


int* GetShortcutPtr(
    DetailFeature f)
{
    switch (f)
    {
    case DetailFeature::Keystrokes:
        return &shortcuts.keystrokes;

    case DetailFeature::CPS:
        return &shortcuts.cps;

    case DetailFeature::Armor:
        return &shortcuts.armor;

    case DetailFeature::Arrows:
        return &shortcuts.arrows;

    case DetailFeature::FPS:
        return &shortcuts.fps;

    case DetailFeature::PartyAutoAccept:
        return &shortcuts.party;

    case DetailFeature::BlockOutline:
        return &shortcuts.blockOutline;

    case DetailFeature::Zoom:
        return &shortcuts.zoom;

    default:
        return nullptr;
    }
}


bool IsValidPlayerTag(const std::wstring& value)
{
    std::string tag;
    for (wchar_t c : value) {
        if (static_cast<uint32_t>(c) > 127) return false;
        tag += static_cast<char>(c);
    }
    return MyClientParty::ValidTag(tag);
}


// ------------------------- Persistence ----------------------
// Simple wide text format.
// Party names are stored after PARTY_BEGIN until PARTY_END.

void SaveConfig()
{
    std::wofstream file(
        std::filesystem::path(ConfigPath()),
        std::ios::trunc
    );

    if (!file.is_open())
        return;

    file << L"VERSION 4\n";

    file << L"FEATURES "
         << keystrokesEnabled << L' '
         << cpsEnabled << L' '
         << armorHudEnabled << L' '
         << arrowCounterEnabled << L' '
         << fpsEnabled << L' '
         << partyAutoAcceptEnabled << L' '
         << blockOutlineEnabled << L' '
         << zoomEnabled
         << L"\n";

    file << L"SHORTCUTS "
         << shortcuts.keystrokes << L' '
         << shortcuts.cps << L' '
         << shortcuts.armor << L' '
         << shortcuts.arrows << L' '
         << shortcuts.fps << L' '
         << shortcuts.party << L' '
         << shortcuts.blockOutline << L' '
         << shortcuts.zoom
         << L"\n";

    file << L"BLOCK_OUTLINE "
         << blockOutlineSettings.lineWidth << L' '
         << GetRValue(
                blockOutlineSettings.color
            ) << L' '
         << GetGValue(
                blockOutlineSettings.color
            ) << L' '
         << GetBValue(
                blockOutlineSettings.color
            ) << L' '
         << blockOutlineSettings.showOnlyWhenTargeted
         << L"\n";

    file << L"FULLBRIGHT "
         << fullbrightEnabled
         << L"\n";

    file << L"NAMETAG "
         << nametagEnabled
         << L"\n";

    file << L"TEXTURES "
         << texturesEnabled
         << L"\n";

    file << L"AUTORQ "
         << autoRqEnabled << L' '
         << autoRqMode
         << L"\n";

    file << L"ZOOM "
          << shortcuts.zoom << L' '
          << zoomEnabled
          << L"\n";

    file << L"HUD "
         << keystrokesHud.x << L' '
         << keystrokesHud.y << L' '
         << keystrokesHud.scale << L' '

         << cpsHud.x << L' '
         << cpsHud.y << L' '
         << cpsHud.scale << L' '

         << armorHud.x << L' '
         << armorHud.y << L' '
         << armorHud.scale << L' '

         << arrowHud.x << L' '
         << arrowHud.y << L' '
         << arrowHud.scale << L' '

         << fpsHud.x << L' '
         << fpsHud.y << L' '
         << fpsHud.scale

         << L"\n";

    file << L"RESOURCES "
         << resourceCounterEnabled << L' '
         << resourceHud.x << L' '
         << resourceHud.y << L' '
         << resourceHud.scale
         << L"\n";

    file << L"PING "
         << pingEnabled << L' '
         << pingHud.x << L' '
         << pingHud.y << L' '
         << pingHud.scale
         << L"\n";

    file << L"FEATURELIST "
         << featureListEnabled << L' '
         << featureListHud.x << L' '
         << featureListHud.y << L' '
         << featureListHud.scale
         << L"\n";

    file << L"CLIENT "
         << clientName
         << L"\n";

    file << L"GUI_SCALE "
         << guiScale
         << L"\n";

    file << L"MENU_ANIMATION "
         << menuAnimation
         << L"\n";

    file << L"THEME "
         << themeIndex
         << L"\n";

    for (int v : favoriteFeatures)
    {
        file << L"FAVORITE "
             << v
             << L"\n";
    }

    file << L"PARTY_BEGIN\n";

    for (const auto& p :
         partyPlayers)
    {
        file << p << L"\n";
    }

    file << L"PARTY_END\n";
}


void LoadConfig()
{
    std::wifstream file(
        std::filesystem::path{ConfigPath()}
    );

    if (!file.is_open())
        return;

    std::wstring key;

    partyPlayers.clear();

    bool readingParty = false;

    std::wstring line;

    while (std::getline(
        file,
        line))
    {
        if (line == L"PARTY_BEGIN")
        {
            readingParty = true;
            continue;
        }

        if (line == L"PARTY_END")
        {
            readingParty = false;
            continue;
        }

        if (readingParty)
        {
            if (IsValidPlayerTag(line))
            {
                partyPlayers.push_back(
                    line
                );
            }

            continue;
        }

        std::wistringstream ss(line);

        ss >> key;

        if (key == L"FEATURES")
        {
            ss >> keystrokesEnabled
               >> cpsEnabled
               >> armorHudEnabled
               >> arrowCounterEnabled
               >> fpsEnabled
               >> partyAutoAcceptEnabled;

            // Compatible with old config files.
            if (!(ss >> blockOutlineEnabled))
            {
                blockOutlineEnabled = false;
            }

            if (!(ss >> zoomEnabled))
            {
                zoomEnabled = false;
            }
        }
        else if (key == L"SHORTCUTS")
        {
            ss >> shortcuts.keystrokes
               >> shortcuts.cps
               >> shortcuts.armor
               >> shortcuts.arrows
               >> shortcuts.fps
               >> shortcuts.party;

            // Compatible with old config files.
            if (!(ss >> shortcuts.blockOutline))
            {
                shortcuts.blockOutline = 0;
            }

            if (!(ss >> shortcuts.zoom))
            {
                shortcuts.zoom = 'C';
            }
        }
        else if (key == L"FULLBRIGHT")
        {
            // Optional line: configs written by older versions simply lack it.
            bool enabled = false;
            if (ss >> enabled)
            {
                fullbrightEnabled = enabled;
            }
        }
        else if (key == L"AUTORQ")
        {
            bool enabled = false;
            int mode = 0;
            if (ss >> enabled >> mode)
            {
                autoRqEnabled = enabled;
                autoRqMode = std::clamp(mode, 0, (int)MyClientAutoRq::kModeCount - 1);
            }
        }
        else if (key == L"TEXTURES")
        {
            bool enabled = false;
            if (ss >> enabled)
            {
                texturesEnabled = enabled;
            }
        }
        else if (key == L"NAMETAG")
        {
            bool enabled = false;
            if (ss >> enabled)
            {
                nametagEnabled = enabled;
            }
        }
        else if (key == L"ZOOM")
        {
            int zoomKey = 'C';
            bool enabled = false;

            ss >> zoomKey >> enabled;

            if (zoomKey == 'G' || zoomKey == VK_ESCAPE)
            {
                zoomKey = 'C';
            }

            shortcuts.zoom = zoomKey;
            zoomEnabled = enabled;
        }
        else if (key == L"BLOCK_OUTLINE")
        {
            int lineWidth = 2;

            int r = 80;
            int g = 180;
            int b = 255;

            bool targetedOnly = true;

            ss >> lineWidth
               >> r
               >> g
               >> b
               >> targetedOnly;

            blockOutlineSettings.lineWidth =
                std::clamp(
                    lineWidth,
                    1,
                    8
                );

            r = std::clamp(
                r,
                0,
                255
            );

            g = std::clamp(
                g,
                0,
                255
            );

            b = std::clamp(
                b,
                0,
                255
            );

            blockOutlineSettings.color =
                RGB(
                    r,
                    g,
                    b
                );

            blockOutlineSettings.showOnlyWhenTargeted =
                targetedOnly;
        }
        else if (key == L"HUD")
        {
            ss >> keystrokesHud.x
               >> keystrokesHud.y
               >> keystrokesHud.scale

               >> cpsHud.x
               >> cpsHud.y
               >> cpsHud.scale

               >> armorHud.x
               >> armorHud.y
               >> armorHud.scale

               >> arrowHud.x
               >> arrowHud.y
               >> arrowHud.scale

               >> fpsHud.x
               >> fpsHud.y
               >> fpsHud.scale;
        }
        else if (key == L"RESOURCES")
        {
            // Optional line: configs written by older versions simply lack it.
            bool enabled = resourceCounterEnabled;
            HudElement element = resourceHud;
            if (ss >> enabled >> element.x >> element.y >> element.scale)
            {
                resourceCounterEnabled = enabled;
                resourceHud = element;
            }
        }
        else if (key == L"PING")
        {
            // Optional line: configs written by older versions simply lack it.
            bool enabled = pingEnabled;
            HudElement element = pingHud;
            if (ss >> enabled >> element.x >> element.y >> element.scale)
            {
                pingEnabled = enabled;
                pingHud = element;
            }
        }
        else if (key == L"FEATURELIST")
        {
            bool enabled = featureListEnabled;
            HudElement element = featureListHud;
            if (ss >> enabled >> element.x >> element.y >> element.scale)
            {
                featureListEnabled = enabled;
                featureListHud = element;
            }
        }
        else if (key == L"CLIENT")
        {
            std::getline(
                ss,
                clientName
            );

            if (!clientName.empty() &&
                clientName.front() == L' ')
            {
                clientName.erase(
                    clientName.begin()
                );
            }
        }
        else if (key == L"GUI_SCALE")
        {
            ss >> guiScale;

            guiScale =
                std::clamp(
                    guiScale,
                    0.75f,
                    1.5f
                );
        }
        else if (key == L"MENU_ANIMATION")
        {
            ss >> menuAnimation;
        }
        else if (key == L"FAVORITE")
        {
            int v = -1;
            ss >> v;

            if (v >= 0 &&
                std::find(
                    favoriteFeatures.begin(),
                    favoriteFeatures.end(),
                    v) == favoriteFeatures.end())
            {
                favoriteFeatures.push_back(v);
            }
        }
        else if (key == L"THEME")
        {
            ss >> themeIndex;

            themeIndex =
                std::clamp(
                    themeIndex,
                    0,
                    9
                );
        }
    }
}


void ResetHudLayout()
{
    keystrokesHud = {
        25,
        130,
        1.0f
    };

    cpsHud = {
        25,
        235,
        1.0f
    };

    armorHud = {
        25,
        315,
        1.0f
    };

    arrowHud = {
        25,
        455,
        1.0f
    };

    fpsHud = {
        25,
        505,
        1.0f
    };

    resourceHud = {
        25,
        555,
        1.0f
    };

    pingHud = {
        190,
        505,
        1.0f
    };

    featureListHud = {
        450,
        25,
        1.0f
    };

    SaveConfig();
}


// ------------------------- Input -----------------------------

bool IsKeyDown(int key)
{
    return (
        GetAsyncKeyState(key)
        & 0x8000
    ) != 0;
}

bool IsZoomRequested()
{
    return zoomEnabled && !menuOpen && !hudEditMode &&
           GetForegroundWindow() == minecraftWindow &&
           shortcuts.zoom != 0 &&
           IsKeyDown(shortcuts.zoom);
}


void ShowMouseCursor()
{
    while (ShowCursor(TRUE) < 0)
    {
    }

    ClipCursor(nullptr);
}


void HideMouseCursor()
{
    while (ShowCursor(FALSE) >= 0)
    {
    }

    ClipCursor(nullptr);
}


// ------------------------- Minecraft window -----------------

// The launcher pins the HUD to the exact game process it started/verified.
DWORD requestedMinecraftPid = 0;

BOOL CALLBACK FindRequestedMinecraft(HWND window, LPARAM parameter)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t title[256]{};
    GetWindowTextW(window, title, 256);
    if (pid == requestedMinecraftPid && IsWindowVisible(window) &&
        std::wstring(title) == L"Minecraft")
    {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindMinecraft()
{
    if (requestedMinecraftPid)
    {
        HWND found = nullptr;
        EnumWindows(FindRequestedMinecraft, reinterpret_cast<LPARAM>(&found));
        return found;
    }
    HWND hwnd =
        FindWindowW(
            nullptr,
            L"Minecraft"
        );

    if (hwnd &&
        IsWindowVisible(hwnd))
    {
        return hwnd;
    }

    return nullptr;
}


// ------------------------- CPS ------------------------------

void CleanClicks(
    std::vector<
        std::chrono::steady_clock::time_point
    >& clicks)
{
    auto limit =
        std::chrono::steady_clock::now()
        - std::chrono::seconds(1);

    clicks.erase(
        std::remove_if(
            clicks.begin(),
            clicks.end(),
            [limit](const auto& t)
            {
                return t < limit;
            }
        ),
        clicks.end()
    );
}


void AddLeftClick()
{
    leftClicks.push_back(
        std::chrono::steady_clock::now()
    );
}


void AddRightClick()
{
    rightClicks.push_back(
        std::chrono::steady_clock::now()
    );
}


int GetLeftCPS()
{
    CleanClicks(leftClicks);

    return static_cast<int>(
        leftClicks.size()
    );
}


int GetRightCPS()
{
    CleanClicks(rightClicks);

    return static_cast<int>(
        rightClicks.size()
    );
}


// ------------------------- Drawing ---------------------------

void DrawTextSimple(
    HDC hdc,
    int x,
    int y,
    const std::wstring& text,
    COLORREF color,
    int size = 18)
{
    color = MenuTextColor(color);

    if (size < 1)
        size = 1;

    HFONT font =
        CreateFontW(
            size,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    HFONT oldFont =
        (HFONT)SelectObject(
            hdc,
            font
        );

    SetTextColor(
        hdc,
        color
    );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    TextOutW(
        hdc,
        x,
        y,
        text.c_str(),
        static_cast<int>(
            text.size()
        )
    );

    SelectObject(
        hdc,
        oldFont
    );

    DeleteObject(font);
}


void DrawBox(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    COLORREF color)
{
    color = MenuBoxColor(color);

    HBRUSH brush =
        CreateSolidBrush(color);

    RECT r{
        x,
        y,
        x + w,
        y + h
    };

    FillRect(
        hdc,
        &r,
        brush
    );

    DeleteObject(brush);
}


void DrawOutline(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    COLORREF color)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            2,
            color
        );

    HPEN oldPen =
        (HPEN)SelectObject(
            hdc,
            pen
        );

    HBRUSH oldBrush =
        (HBRUSH)SelectObject(
            hdc,
            GetStockObject(
                HOLLOW_BRUSH
            )
        );

    Rectangle(
        hdc,
        x,
        y,
        x + w,
        y + h
    );

    SelectObject(
        hdc,
        oldBrush
    );

    SelectObject(
        hdc,
        oldPen
    );

    DeleteObject(pen);
}


// ---- Menu themes: each theme is just a background + an accent colour;
// every other menu colour is derived from those two.
struct MenuTheme
{
    const wchar_t* name;
    COLORREF bg;
    COLORREF accent;
    bool light;   // light background (dark text) instead of the usual dark theme
};

const MenuTheme kThemes[] =
{
    { L"Purple",   RGB(15,11,26), RGB(139,92,246), false },
    { L"Red",      RGB(12,10,10), RGB(220,38,38), false },
    { L"Black",    RGB(14,14,16), RGB(96,106,122), false },
    { L"Blue",     RGB(8,13,26),  RGB(37,99,235), false },
    { L"Green",    RGB(8,16,12),  RGB(22,163,74), false },
    { L"Cyan",     RGB(6,15,19),  RGB(8,145,178), false },
    { L"Pink",     RGB(20,10,17), RGB(219,39,119), false },
    { L"Orange",   RGB(18,12,8),  RGB(234,88,12), false },
    { L"Yellow",   RGB(16,14,6),  RGB(245,197,24), false },
    { L"Brown",    RGB(20,14,10), RGB(150,98,58), false }
};

const int kThemeCount = 10;

COLORREF MixColor(
    COLORREF a,
    COLORREF b,
    float t)
{
    auto ch =
        [&](int (*get)(COLORREF))
        {
            return (int)(
                get(a) +
                (get(b) - get(a)) * t
            );
        };

    return RGB(
        ch([](COLORREF c) { return (int)GetRValue(c); }),
        ch([](COLORREF c) { return (int)GetGValue(c); }),
        ch([](COLORREF c) { return (int)GetBValue(c); })
    );
}

struct ThemeColors
{
    COLORREF bg;
    COLORREF card;
    COLORREF tile;
    COLORREF button;
    COLORREF accent;
    COLORREF accentHi;
    COLORREF text;
    COLORREF muted;
    COLORREF off;
    COLORREF onAccent;   // text colour used on top of the accent colour
};

ThemeColors CurrentTheme()
{
    const MenuTheme& m =
        kThemes[
            std::clamp(
                themeIndex,
                0,
                kThemeCount - 1
            )
        ];

    ThemeColors t{};

    if (m.light)
    {
        t.bg = m.bg;
        t.card = MixColor(m.bg, m.accent, 0.08f);
        t.tile = MixColor(m.bg, m.accent, 0.20f);
        t.button = MixColor(m.bg, m.accent, 0.13f);
        t.accent = m.accent;
        t.accentHi = m.accent;
        t.text = RGB(28,30,40);
        t.muted = MixColor(m.bg, RGB(0,0,0), 0.50f);
        t.off = MixColor(m.bg, RGB(110,110,125), 0.28f);
        t.onAccent = RGB(255,255,255);
        return t;
    }

    t.bg = m.bg;
    t.card = MixColor(m.bg, m.accent, 0.13f);
    t.tile = MixColor(m.bg, m.accent, 0.32f);
    t.button = MixColor(m.bg, m.accent, 0.24f);
    t.accent = m.accent;
    t.accentHi = MixColor(m.accent, RGB(255,255,255), 0.45f);
    t.text = MixColor(RGB(255,255,255), m.accent, 0.08f);
    t.muted = MixColor(m.bg, RGB(255,255,255), 0.55f);
    t.off = MixColor(m.bg, RGB(150,150,160), 0.30f);

    // Bright accents (white, yellow) need dark text on top of them.
    const int luminance =
        (299 * GetRValue(m.accent) +
         587 * GetGValue(m.accent) +
         114 * GetBValue(m.accent)) / 1000;

    t.onAccent =
        luminance > 150
            ? RGB(18,16,24)
            : RGB(255,255,255);

    return t;
}


bool MenuLightMode()
{
    return menuDrawing &&
           kThemes[
               std::clamp(
                   themeIndex,
                   0,
                   kThemeCount - 1
               )
           ].light;
}

// Menu text colours are written for dark themes (white / light grey).
// On a light theme they are flipped to dark so they stay readable.
COLORREF MenuTextColor(COLORREF c)
{
    if (!MenuLightMode())
        return c;

    const int r = GetRValue(c);
    const int g = GetGValue(c);
    const int b = GetBValue(c);

    const bool grey =
        abs(r - g) < 24 &&
        abs(g - b) < 24 &&
        abs(r - b) < 24;

    if (grey)
    {
        const int v = std::max(28, 255 - r);
        return RGB(v, v, std::min(255, v + 6));
    }

    return RGB(r * 55 / 100, g * 55 / 100, b * 55 / 100);
}

COLORREF MenuBoxColor(COLORREF c)
{
    return MenuLightMode()
               ? MixColor(c, RGB(255,255,255), 0.80f)
               : c;
}


// Gaming-purple menu theme. The menu is drawn on a magenta colour-key buffer,
// so rounded corners simply show the game behind the overlay.
void DrawRoundBox(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    int radius,
    COLORREF fill)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    RoundRect(
        hdc,
        x,
        y,
        x + w + 1,
        y + h + 1,
        radius * 2,
        radius * 2
    );

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}


void DrawTextCentered(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    const std::wstring& text,
    COLORREF color,
    int size,
    const wchar_t* face = L"Segoe UI")
{
    HFONT font =
        CreateFontW(
            size,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            face
        );

    HFONT oldFont =
        (HFONT)SelectObject(hdc, font);

    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);

    RECT r{x, y, x + w, y + h};

    DrawTextW(
        hdc,
        text.c_str(),
        static_cast<int>(text.size()),
        &r,
        DT_CENTER | DT_VCENTER |
            DT_SINGLELINE | DT_NOPREFIX
    );

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}


void DrawButton(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    const std::wstring& text,
    bool selected)
{
    const ThemeColors t = CurrentTheme();

    DrawRoundBox(
        hdc,
        x,
        y,
        w,
        h,
        10,
        selected
            ? t.accent
            : t.button
    );

    DrawTextSimple(
        hdc,
        x + 15,
        y + 10,
        text,
        selected ? t.onAccent : t.text,
        17
    );
}


// ------------------------- HUD drawing ----------------------

void DrawKeystrokes(HDC hdc)
{
    if (!keystrokesEnabled)
        return;

    float s =
        keystrokesHud.scale;

    int x =
        keystrokesHud.x;

    int y =
        keystrokesHud.y;

    int size =
        std::max(
            10,
            (int)(40 * s)
        );

    int gap =
        std::max(
            2,
            (int)(5 * s)
        );

    auto kc =
        [](bool p)
        {
            return p
                ? RGB(70,150,220)
                : RGB(35,35,35);
        };

    struct K
    {
        int px;
        int py;
        wchar_t c;
        int vk;
    };

    K keys[] =
    {
        {1,0,L'W','W'},
        {0,1,L'A','A'},
        {1,1,L'S','S'},
        {2,1,L'D','D'}
    };

    for (auto& k : keys)
    {
        int bx =
            x + k.px *
            (size + gap);

        int by =
            y + k.py *
            (size + gap);

        DrawBox(
            hdc,
            bx,
            by,
            size,
            size,
            kc(
                IsKeyDown(k.vk)
            )
        );

        DrawTextSimple(
            hdc,
            bx + (int)(12 * s),
            by + (int)(8 * s),
            std::wstring(
                1,
                k.c
            ),
            RGB(255,255,255),
            std::max(
                10,
                (int)(20 * s)
            )
        );
    }

    if (hudEditMode &&
        selectedHud ==
            HudElementId::Keystrokes)
    {
        DrawOutline(
            hdc,
            x,
            y,
            size * 3 +
                gap * 2,
            size * 2 +
                gap,
            RGB(80,180,255)
        );
    }
}


void DrawCPS(HDC hdc)
{
    if (!cpsEnabled)
        return;

    float s =
        cpsHud.scale;

    int x =
        cpsHud.x;

    int y =
        cpsHud.y;

    int w =
        std::max(
            60,
            (int)(165 * s)
        );

    int h =
        std::max(
            30,
            (int)(65 * s)
        );

    DrawBox(
        hdc,
        x,
        y,
        w,
        h,
        RGB(20,20,20)
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(7 * s),
        L"LMB: " +
            std::to_wstring(
                GetLeftCPS()
            ),
        RGB(255,255,255),
        std::max(
            10,
            (int)(17 * s)
        )
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(34 * s),
        L"RMB: " +
            std::to_wstring(
                GetRightCPS()
            ),
        RGB(255,255,255),
        std::max(
            10,
            (int)(17 * s)
        )
    );

    if (hudEditMode &&
        selectedHud ==
            HudElementId::CPS)
    {
        DrawOutline(
            hdc,
            x,
            y,
            w,
            h,
            RGB(80,180,255)
        );
    }
}


std::wstring ArmorText(unsigned slot)
{
    const auto armor = MinecraftDataProvider::GetArmor(slot);
    switch (armor.state) {
    case MyClientFeatures::Empty: return L"EMPTY";
    case MyClientFeatures::NoDurability: return L"N/A";
    case MyClientFeatures::Durable: return std::to_wstring(armor.remaining) + L"/" + std::to_wstring(armor.maximum);
    default: return L"---";
    }
}

void DrawArmor(HDC hdc)
{
    if (!armorHudEnabled)
        return;

    float s =
        armorHud.scale;

    int x =
        armorHud.x;

    int y =
        armorHud.y;

    int w =
        std::max(
            100,
            (int)(190 * s)
        );

    int h =
        std::max(
            70,
            (int)(125 * s)
        );

    DrawBox(
        hdc,
        x,
        y,
        w,
        h,
        RGB(20,20,20)
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(8 * s),
        L"ARMOR HUD",
        RGB(255,255,255),
        std::max(
            10,
            (int)(17 * s)
        )
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(35 * s),
        L"HEAD  " + ArmorText(0),
        RGB(220,220,220),
        std::max(
            9,
            (int)(15 * s)
        )
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(58 * s),
        L"CHEST  " + ArmorText(1),
        RGB(220,220,220),
        std::max(
            9,
            (int)(15 * s)
        )
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(81 * s),
        L"LEGS  " + ArmorText(2),
        RGB(220,220,220),
        std::max(
            9,
            (int)(15 * s)
        )
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(104 * s),
        L"FEET  " + ArmorText(3),
        RGB(220,220,220),
        std::max(
            9,
            (int)(15 * s)
        )
    );

    if (hudEditMode &&
        selectedHud ==
            HudElementId::Armor)
    {
        DrawOutline(
            hdc,
            x,
            y,
            w,
            h,
            RGB(80,180,255)
        );
    }
}


void DrawResources(HDC hdc)
{
    if (!resourceCounterEnabled)
        return;

    MyClientResources::Counts counts{};
    const bool known = MinecraftDataProvider::GetResources(counts);

    struct Row { const wchar_t* name; uint32_t value; COLORREF color; };
    const Row rows[] =
    {
        { L"Iron",    counts.iron,    RGB(215,215,215) },
        { L"Gold",    counts.gold,    RGB(255,205,70)  },
        { L"Diamond", counts.diamond, RGB(90,235,235)  },
        { L"Emerald", counts.emerald, RGB(85,225,110)  }
    };

    const float s = resourceHud.scale;
    const int x = resourceHud.x;
    const int y = resourceHud.y;
    const int w = std::max(100, (int)(150 * s));
    const int rowH = std::max(14, (int)(26 * s));
    const int h = std::max(60, (int)(14 * s) + rowH * 4);

    DrawBox(hdc, x, y, w, h, RGB(20,20,20));

    for (int i = 0; i < 4; i++)
    {
        const std::wstring label = std::wstring(rows[i].name) + L": " +
            (known ? std::to_wstring(rows[i].value) : std::wstring(L"---"));

        DrawTextSimple(
            hdc,
            x + (int)(12 * s),
            y + (int)(8 * s) + i * rowH,
            label,
            rows[i].color,
            std::max(10, (int)(17 * s))
        );
    }

    if (hudEditMode && selectedHud == HudElementId::Resources)
        DrawOutline(hdc, x, y, w, h, RGB(80,180,255));
}


std::vector<std::wstring> EnabledFeatureNames();   // defined with the feature list below

void DrawFeatureList(HDC hdc)
{
    if (!featureListEnabled)
        return;

    const auto names = EnabledFeatureNames();

    const float s = featureListHud.scale;
    const int x = featureListHud.x;
    const int y = featureListHud.y;
    const int rowH = std::max(18, (int)(26 * s));
    const int w = std::max(110, (int)(170 * s));
    const int count = std::max(1, (int)names.size());
    const int h = rowH * count;

    if (names.empty())
    {
        DrawBox(hdc, x, y, w, rowH, RGB(20,20,20));
        DrawTextSimple(hdc, x + (int)(10 * s), y + (int)(4 * s), L"(no features on)", RGB(170,170,170), std::max(10, (int)(15 * s)));
    }

    for (size_t i = 0; i < names.size(); i++)
    {
        const int ry = y + (int)i * rowH;
        DrawBox(hdc, x, ry, w, rowH, RGB(20,20,20));
        DrawTextSimple(hdc, x + (int)(10 * s), ry + (int)(4 * s), names[i], CurrentTheme().text, std::max(10, (int)(15 * s)));
    }

    if (hudEditMode && selectedHud == HudElementId::FeatureList)
        DrawOutline(hdc, x, y, w, h, RGB(80,180,255));
}


void DrawPing(HDC hdc)
{
    if (!pingEnabled)
        return;

    int ms = 0;
    const bool known = PingMonitor::Get(ms);

    const COLORREF color =
        !known ? RGB(170,170,170)
               : (ms < 60 ? RGB(100,220,130)
                          : (ms < 120 ? RGB(255,205,70)
                                      : RGB(255,100,90)));

    const float s = pingHud.scale;
    const int x = pingHud.x;
    const int y = pingHud.y;
    const int w = std::max(90, (int)(120 * s));
    const int h = std::max(24, (int)(34 * s));

    DrawBox(hdc, x, y, w, h, RGB(20,20,20));

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(7 * s),
        known ? L"Ping: " + std::to_wstring(ms) + L" ms"
              : std::wstring(L"Ping: ---"),
        color,
        std::max(10, (int)(18 * s))
    );

    if (hudEditMode && selectedHud == HudElementId::Ping)
        DrawOutline(hdc, x, y, w, h, RGB(80,180,255));
}


void DrawArrows(HDC hdc)
{
    if (!arrowCounterEnabled)
        return;

    uint32_t count = 0;
    const std::wstring label = MinecraftDataProvider::GetArrowCount(count)
        ? L"Arrows: " + std::to_wstring(count)
        : L"Arrows: ---";

    float s =
        arrowHud.scale;

    int x =
        arrowHud.x;

    int y =
        arrowHud.y;

    int w =
        std::max(
            80,
            (int)(150 * s)
        );

    int h =
        std::max(
            25,
            (int)(42 * s)
        );

    DrawBox(
        hdc,
        x,
        y,
        w,
        h,
        RGB(20,20,20)
    );

    DrawTextSimple(
        hdc,
        x + (int)(12 * s),
        y + (int)(9 * s),
        label,
        RGB(255,255,255),
        std::max(
            10,
            (int)(17 * s)
        )
    );

    if (hudEditMode &&
        selectedHud ==
            HudElementId::Arrows)
    {
        DrawOutline(
            hdc,
            x,
            y,
            w,
            h,
            RGB(80,180,255)
        );
    }
}


void DrawFPS(HDC hdc)
{
    float gameFps = 0;
    const bool knownFps = MinecraftDataProvider::GetFps(gameFps);
    if (!fpsEnabled)
        return;

    float s =
        fpsHud.scale;

    int x =
        fpsHud.x;

    int y =
        fpsHud.y;

    int w =
        std::max(
            70,
            (int)(120 * s)
        );

    int h =
        std::max(
            25,
            (int)(38 * s)
        );

    DrawBox(
        hdc,
        x,
        y,
        w,
        h,
        RGB(20,20,20)
    );

    DrawTextSimple(
        hdc,
        x + (int)(10 * s),
        y + (int)(8 * s),
        knownFps ? L"FPS: " + std::to_wstring(static_cast<int>(std::lround(gameFps))) : L"FPS: ---",
        RGB(255,255,255),
        std::max(
            10,
            (int)(16 * s)
        )
    );

    if (hudEditMode &&
        selectedHud ==
            HudElementId::FPS)
    {
        DrawOutline(
            hdc,
            x,
            y,
            w,
            h,
            RGB(80,180,255)
        );
    }
}


// The block outline colour is now changed inside the game (DLL hook on the game's own outline
// rendering). Nothing is drawn over the game any more: the old "Block: x, y, z" label and the
// projected lines were never connected to real game data.
void DrawBlockOutline(HDC)
{
}


void DrawHud(HDC hdc)
{
    // Show the HUD only inside a world/server. The position editor still shows it.
    if (!hudEditMode && !MinecraftDataProvider::IsInWorld()) return;
    DrawBlockOutline(hdc);
    DrawKeystrokes(hdc);
    DrawCPS(hdc);
    DrawArmor(hdc);
    DrawArrows(hdc);
    DrawFPS(hdc);
    DrawResources(hdc);
    DrawPing(hdc);
    DrawFeatureList(hdc);
}


// ------------------------- HUD hit testing ------------------

HudElement* GetHudElement(
    HudElementId id)
{
    switch (id)
    {
    case HudElementId::Keystrokes:
        return &keystrokesHud;

    case HudElementId::CPS:
        return &cpsHud;

    case HudElementId::Armor:
        return &armorHud;

    case HudElementId::Arrows:
        return &arrowHud;

    case HudElementId::FPS:
        return &fpsHud;

    case HudElementId::Resources:
        return &resourceHud;

    case HudElementId::Ping:
        return &pingHud;

    case HudElementId::FeatureList:
        return &featureListHud;

    default:
        return nullptr;
    }
}


HudElementId GetHudAt(
    int mx,
    int my)
{
    auto hit =
        [&](HudElementId id,
            HudElement& e,
            int w,
            int h)
        {
            int ww =
                std::max(
                    1,
                    (int)(w * e.scale)
                );

            int hh =
                std::max(
                    1,
                    (int)(h * e.scale)
                );

            return IsInside(
                mx,
                my,
                e.x,
                e.y,
                ww,
                hh
            )
                ? id
                : HudElementId::None;
        };

    if (keystrokesEnabled)
    {
        int size =
            std::max(
                10,
                (int)(
                    40 *
                    keystrokesHud.scale
                )
            );

        int gap =
            std::max(
                2,
                (int)(
                    5 *
                    keystrokesHud.scale
                )
            );

        auto r =
            hit(
                HudElementId::Keystrokes,
                keystrokesHud,
                size * 3 +
                    gap * 2,
                size * 2 +
                    gap
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (cpsEnabled)
    {
        auto r =
            hit(
                HudElementId::CPS,
                cpsHud,
                165,
                65
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (armorHudEnabled)
    {
        auto r =
            hit(
                HudElementId::Armor,
                armorHud,
                190,
                125
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (arrowCounterEnabled)
    {
        auto r =
            hit(
                HudElementId::Arrows,
                arrowHud,
                150,
                42
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (fpsEnabled)
    {
        auto r =
            hit(
                HudElementId::FPS,
                fpsHud,
                120,
                38
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (featureListEnabled)
    {
        auto r =
            hit(
                HudElementId::FeatureList,
                featureListHud,
                170,
                (int)std::max<size_t>(1, EnabledFeatureNames().size()) * 26
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (pingEnabled)
    {
        auto r =
            hit(
                HudElementId::Ping,
                pingHud,
                120,
                34
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    if (resourceCounterEnabled)
    {
        auto r =
            hit(
                HudElementId::Resources,
                resourceHud,
                150,
                118
            );

        if (r !=
            HudElementId::None)
        {
            return r;
        }
    }

    return HudElementId::None;
}


// ------------------------- Feature UI -----------------------

std::wstring FeatureTitle(
    DetailFeature f)
{
    switch (f)
    {
    case DetailFeature::Keystrokes:
        return L"Keystrokes";

    case DetailFeature::CPS:
        return L"CPS Counter";

    case DetailFeature::Armor:
        return L"Armor HUD";

    case DetailFeature::Arrows:
        return L"Arrow Counter";

    case DetailFeature::FPS:
        return L"FPS";

    case DetailFeature::PartyAutoAccept:
        return L"Party Auto Accept";

    case DetailFeature::BlockOutline:
        return L"Block Outline";

    case DetailFeature::Zoom:
        return L"Zoom";

    case DetailFeature::Resources:
        return L"Resource Counter";

    case DetailFeature::Fullbright:
        return L"Full Bright";

    case DetailFeature::Nametag:
        return L"Nametag";

    case DetailFeature::Ping:
        return L"Ping";

    case DetailFeature::Textures:
        return L"Textures";

    case DetailFeature::AutoRq:
        return L"Auto RQ";

    case DetailFeature::FeatureList:
        return L"Feature List";

    default:
        return L"";
    }
}


bool FeatureEnabled(
    DetailFeature f)
{
    switch (f)
    {
    case DetailFeature::Keystrokes:
        return keystrokesEnabled;

    case DetailFeature::CPS:
        return cpsEnabled;

    case DetailFeature::Armor:
        return armorHudEnabled;

    case DetailFeature::Arrows:
        return arrowCounterEnabled;

    case DetailFeature::FPS:
        return fpsEnabled;

    case DetailFeature::PartyAutoAccept:
        return partyAutoAcceptEnabled;

    case DetailFeature::BlockOutline:
        return blockOutlineEnabled;

    case DetailFeature::Zoom:
        return zoomEnabled;

    case DetailFeature::Resources:
        return resourceCounterEnabled;

    case DetailFeature::Fullbright:
        return fullbrightEnabled;

    case DetailFeature::Nametag:
        return nametagEnabled;

    case DetailFeature::Ping:
        return pingEnabled;

    case DetailFeature::Textures:
        return texturesEnabled;

    case DetailFeature::AutoRq:
        return autoRqEnabled;

    case DetailFeature::FeatureList:
        return featureListEnabled;

    default:
        return false;
    }
}


void ToggleFeature(
    DetailFeature f)
{
    switch (f)
    {
    case DetailFeature::Keystrokes:
        keystrokesEnabled =
            !keystrokesEnabled;
        break;

    case DetailFeature::CPS:
        cpsEnabled =
            !cpsEnabled;
        break;

    case DetailFeature::Armor:
        armorHudEnabled =
            !armorHudEnabled;
        break;

    case DetailFeature::Arrows:
        arrowCounterEnabled =
            !arrowCounterEnabled;
        break;

    case DetailFeature::FPS:
        fpsEnabled =
            !fpsEnabled;
        break;

    case DetailFeature::PartyAutoAccept:
        partyAutoAcceptEnabled =
            !partyAutoAcceptEnabled;
        break;

    case DetailFeature::BlockOutline:
        blockOutlineEnabled =
            !blockOutlineEnabled;

        if (!blockOutlineEnabled)
        {
            ClearBlockOutlineData();
        }

        break;

    case DetailFeature::Zoom:
        zoomEnabled = !zoomEnabled;

        if (!zoomEnabled)
        {
            zoomActive = false;
        }

        break;

    case DetailFeature::Resources:
        resourceCounterEnabled =
            !resourceCounterEnabled;
        break;

    case DetailFeature::Fullbright:
        fullbrightEnabled =
            !fullbrightEnabled;
        break;

    case DetailFeature::Nametag:
        nametagEnabled =
            !nametagEnabled;
        break;

    case DetailFeature::Ping:
        pingEnabled =
            !pingEnabled;
        break;

    case DetailFeature::Textures:
        texturesEnabled =
            !texturesEnabled;
        break;

    case DetailFeature::AutoRq:
        autoRqEnabled =
            !autoRqEnabled;
        break;

    case DetailFeature::FeatureList:
        featureListEnabled =
            !featureListEnabled;
        break;

    default:
        break;
    }

    SaveConfig();
}


void CheckShortcut(
    int vk,
    bool& previousState,
    DetailFeature f)
{
    if (vk == 0 ||
        vk == 'G' ||
        vk == VK_ESCAPE)
    {
        previousState = false;
        return;
    }

    bool down =
        IsKeyDown(vk);

    if (down &&
        !previousState &&
        gameActive &&
        !menuOpen &&
        !waitingForShortcut)
    {
        ToggleFeature(f);
    }

    previousState = down;
}


// ------------------------- Navigation / menu ----------------

void DrawNavigation(HDC hdc)
{
    int x = 25;
    int y = 80;

    DrawTextSimple(
        hdc,
        x,
        y - 45,
        clientName,
        RGB(255,255,255),
        25
    );

    DrawButton(
        hdc,
        x,
        y,
        135,
        55,
        L"Feature",
        selectedCategory ==
            MenuCategory::Feature
    );

    DrawButton(
        hdc,
        x,
        y + 70,
        135,
        55,
        L"HUD",
        selectedCategory ==
            MenuCategory::HUD
    );

    DrawButton(
        hdc,
        x,
        y + 140,
        135,
        55,
        L"Settings",
        selectedCategory ==
            MenuCategory::Settings
    );

}


// Module cards: 3 columns x N rows while no details panel is open
// (Flarial-style grid); a single compact column while details are open.
const DetailFeature kFeatureList[] =
{
    DetailFeature::Keystrokes,
    DetailFeature::CPS,
    DetailFeature::Armor,
    DetailFeature::Arrows,
    DetailFeature::FPS,
    DetailFeature::PartyAutoAccept,
    DetailFeature::BlockOutline,
    DetailFeature::Zoom,
    DetailFeature::Resources,
    DetailFeature::Fullbright,
    DetailFeature::Nametag,
    DetailFeature::Ping,
    DetailFeature::Textures,
    DetailFeature::AutoRq,
    DetailFeature::FeatureList
};

const int kFeatureCount = 15;

// Names of the switched-on features (the list itself excluded), longest name first.
std::vector<std::wstring> EnabledFeatureNames()
{
    std::vector<std::wstring> names;

    for (int i = 0; i < kFeatureCount; i++)
    {
        const DetailFeature f = kFeatureList[i];

        if (f != DetailFeature::FeatureList && FeatureEnabled(f))
            names.push_back(FeatureTitle(f));
    }

    std::stable_sort(names.begin(), names.end(),
        [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });

    return names;
}

bool ContainsNoCase(
    const std::wstring& text,
    const std::wstring& part)
{
    if (part.empty())
        return true;

    auto lower =
        [](std::wstring v)
        {
            for (auto& c : v)
                c = (wchar_t)towlower(c);

            return v;
        };

    return lower(text).find(lower(part)) !=
           std::wstring::npos;
}

bool IsFavorite(DetailFeature f)
{
    return std::find(
               favoriteFeatures.begin(),
               favoriteFeatures.end(),
               static_cast<int>(f)
           ) != favoriteFeatures.end();
}

void ToggleFavorite(DetailFeature f)
{
    const int v = static_cast<int>(f);

    auto it =
        std::find(
            favoriteFeatures.begin(),
            favoriteFeatures.end(),
            v
        );

    if (it == favoriteFeatures.end())
        favoriteFeatures.push_back(v);
    else
        favoriteFeatures.erase(it);

    SaveConfig();
}

// Indices into kFeatureList that match the search text.
std::vector<int> VisibleFeatures()
{
    std::vector<int> visible;

    for (int i = 0; i < kFeatureCount; i++)
    {
        if (ContainsNoCase(
                FeatureTitle(kFeatureList[i]),
                searchQuery))
        {
            visible.push_back(i);
        }
    }

    // Favourites first, then alphabetical (case-insensitive) inside each group.
    std::sort(
        visible.begin(),
        visible.end(),
        [](int a, int b)
        {
            const bool fa = IsFavorite(kFeatureList[a]);
            const bool fb = IsFavorite(kFeatureList[b]);

            if (fa != fb)
                return fa;

            return _wcsicmp(
                       FeatureTitle(kFeatureList[a]).c_str(),
                       FeatureTitle(kFeatureList[b]).c_str()
                   ) < 0;
        }
    );

    return visible;
}

bool FeatureGridMode()
{
    return detailFeature ==
           DetailFeature::None;
}


// The card grid scrolls by whole rows (mouse wheel): the cards keep their size, however many
// features there are. 4 rows fit inside the menu panel.
int featureScrollRow = 0;
const int kFeatureRowsVisible = 4;

int FeatureRowCount(int cardCount)
{
    return (cardCount + 2) / 3;
}

// Clamp to the rows that exist for `cardCount` cards.
void ClampFeatureScroll(int cardCount)
{
    featureScrollRow =
        std::clamp(
            featureScrollRow,
            0,
            std::max(0, FeatureRowCount(cardCount) - kFeatureRowsVisible)
        );
}

// Is the card at list position `i` inside the visible rows? (Hidden cards are neither drawn nor clickable.)
bool FeatureCardOnScreen(int i)
{
    if (!FeatureGridMode())
        return true;

    const int row = i / 3 - featureScrollRow;

    return row >= 0 && row < kFeatureRowsVisible;
}

void FeatureCardRect(
    int i,
    int& x,
    int& y,
    int& w,
    int& h)
{
    if (FeatureGridMode())
    {
        const int cols = 3;
        const int cardW = 200;
        const int cardH = 92;
        const int gap = 12;

        x = 190 + (i % cols) * (cardW + gap);
        y = 150 + (i / cols - featureScrollRow) * (cardH + gap);
        w = cardW;
        h = cardH;
    }
    else
    {
        x = 190;
        y = 160 + i * 36;
        w = 320;
        h = 34;
    }
}


// Star button of a card (favourite on/off).
void FeatureStarRect(
    int i,
    int& x,
    int& y,
    int& w,
    int& h)
{
    int cx, cy, cw, ch;
    FeatureCardRect(i, cx, cy, cw, ch);

    if (FeatureGridMode())
    {
        x = cx + 14;
        y = cy + 16;
        w = 36;
        h = 36;
    }
    else
    {
        x = cx + 6;
        y = cy + 4;
        w = 28;
        h = 28;
    }
}


// Gear button of a grid card (opens the details/settings panel).
void FeatureGearRect(
    int i,
    int& x,
    int& y,
    int& w,
    int& h)
{
    int cx, cy, cw, ch;
    FeatureCardRect(i, cx, cy, cw, ch);

    x = cx + 12;
    y = cy + ch - 42;
    w = 34;
    h = 30;
}


void DrawFeatureCard(
    HDC hdc,
    int pos,
    int idx)
{
    const ThemeColors t = CurrentTheme();

    const DetailFeature f =
        kFeatureList[idx];

    const bool on =
        FeatureEnabled(f);

    const std::wstring title =
        FeatureTitle(f);

    int x, y, w, h;
    FeatureCardRect(pos, x, y, w, h);

    DrawRoundBox(
        hdc, x, y, w, h, 12,
        t.card
    );

    const COLORREF pillColor =
        on ? t.accent
           : t.off;

    const std::wstring state =
        on ? L"Enabled"
           : L"Disabled";

    if (FeatureGridMode())
    {
        // Star sits in the title row, left edge in line with the gear below it.
        DrawTextCentered(
            hdc, x + 18, y + 18, 28, 32,
            IsFavorite(f) ? L"\u2605" : L"\u2606",
            IsFavorite(f) ? RGB(255,205,70) : t.muted,
            24,
            L"Segoe UI Symbol"
        );

        DrawTextSimple(
            hdc,
            x + 52,
            y + 24,
            title,
            t.text,
            title.size() > 16 ? 13 : (title.size() > 14 ? 14 : 16)
        );

        int gx, gy, gw, gh;
        FeatureGearRect(pos, gx, gy, gw, gh);

        DrawRoundBox(
            hdc, gx, gy, gw, gh, 8,
            t.button
        );

        DrawTextCentered(
            hdc, gx, gy, gw, gh,
            L"\u2699",
            t.accentHi,
            18,
            L"Segoe UI Symbol"
        );

        const int px = gx + gw + 8;
        const int pw = x + w - 12 - px;

        DrawRoundBox(
            hdc, px, gy, pw, gh, 8,
            pillColor
        );

        DrawTextCentered(
            hdc, px, gy, pw, gh,
            state,
            on ? t.onAccent : t.text,
            14
        );
    }
    else
    {
        DrawTextCentered(
            hdc, x + 6, y + 4, 28, 28,
            IsFavorite(f) ? L"\u2605" : L"\u2606",
            IsFavorite(f) ? RGB(255,205,70) : t.muted,
            20,
            L"Segoe UI Symbol"
        );

        DrawTextSimple(
            hdc,
            x + 40,
            y + 8,
            title,
            t.text,
            16
        );

        DrawRoundBox(
            hdc, x + w - 96, y + 5, 86, 26, 8,
            pillColor
        );

        DrawTextCentered(
            hdc, x + w - 96, y + 5, 86, 26,
            state,
            on ? t.onAccent : t.text,
            13
        );
    }
}


void DrawFeatureMenu(HDC hdc)
{
    const ThemeColors t = CurrentTheme();

    int x = 190;
    int y = 70;

    DrawTextSimple(
        hdc,
        x,
        y,
        L"Feature",
        RGB(255,255,255),
        26
    );

    if (FeatureGridMode())
    {
        DrawTextSimple(
            hdc,
            x,
            y + 40,
            L"Click = ON / OFF     Star = Favorite     Gear / Right click = Settings     Wheel = scroll",
            t.muted,
            14
        );
    }
    else
    {
        DrawRoundBox(
            hdc, x, 108, 120, 30, 8,
            t.button
        );

        DrawTextCentered(
            hdc, x, 108, 120, 30,
            L"< Back",
            t.text,
            14
        );
    }

    // Search box (top right of the page, aligned with the card grid)
    DrawRoundBox(
        hdc, 410, 64, 404, 36, 10,
        searchActive ? t.accent : t.button
    );

    DrawRoundBox(
        hdc, 412, 66, 400, 32, 9,
        t.card
    );

    if (searchQuery.empty() && !searchActive)
    {
        DrawTextSimple(
            hdc,
            424,
            72,
            L"Search...",
            t.muted,
            16
        );
    }
    else
    {
        DrawTextSimple(
            hdc,
            424,
            72,
            searchQuery +
                (searchActive ? L"|" : L""),
            t.text,
            16
        );
    }

    const std::vector<int> visible =
        VisibleFeatures();

    if (visible.empty())
    {
        DrawTextSimple(
            hdc,
            190,
            160,
            L"No matching features",
            t.muted,
            16
        );
    }

    ClampFeatureScroll((int)visible.size());

    for (int pos = 0;
         pos < (int)visible.size();
         pos++)
    {
        if (!FeatureCardOnScreen(pos))
            continue;

        DrawFeatureCard(
            hdc,
            pos,
            visible[pos]
        );
    }

    // Scrollbar (only when some rows are hidden)
    const int rows = FeatureRowCount((int)visible.size());

    if (rows > kFeatureRowsVisible)
    {
        const int trackTop = 150;
        const int trackHeight = kFeatureRowsVisible * 104 - 12;
        const int thumbHeight = std::max(30, trackHeight * kFeatureRowsVisible / rows);
        const int maxScroll = rows - kFeatureRowsVisible;
        const int thumbTop =
            trackTop + (trackHeight - thumbHeight) * featureScrollRow / maxScroll;

        DrawRoundBox(hdc, 828, trackTop, 8, trackHeight, 4, t.card);
        DrawRoundBox(hdc, 828, thumbTop, 8, thumbHeight, 4, t.accent);
    }
}


void DrawPartyDetails(
    HDC hdc,
    int x,
    int y)
{
    DrawTextSimple(
        hdc,
        x,
        y + 50,
        L"Party Auto Accept",
        RGB(255,255,255),
        20
    );

    DrawTextSimple(
        hdc,
        x,
        y + 82,
        L"Registered GameTags (max 64, ASCII)",
        RGB(220,220,220),
        15
    );

    int listY =
        y + 108;

    const int maxVisible = 6;

    int count = 0;

    for (
        size_t i = 0;
        i < partyPlayers.size() &&
        count < maxVisible;
        i++, count++)
    {
        int yy =
            listY +
            count * 34;

        DrawBox(
            hdc,
            x,
            yy,
            230,
            28,
            RGB(38,38,38)
        );

        DrawTextSimple(
            hdc,
            x + 8,
            yy + 5,
            partyPlayers[i],
            RGB(235,235,235),
            14
        );

        DrawButton(
            hdc,
            x + 238,
            yy,
            72,
            28,
            L"削除",
            false
        );
    }

    if (partyPlayers.size() >
        maxVisible)
    {
        DrawTextSimple(
            hdc,
            x,
            listY +
                maxVisible * 34,
            L"More players saved...",
            RGB(130,130,130),
            12
        );
    }

    int addY =
        listY +
        std::min<int>(
            static_cast<int>(
                partyPlayers.size()
            ),
            maxVisible
        ) * 34 +
        8;

    DrawButton(
        hdc,
        x,
        addY,
        310,
        38,
        L"+ プレイヤー追加",
        false
    );

    DrawTextSimple(
        hdc,
        x,
        y + 355,
        MinecraftDataProvider::GetPartyStatusText(),
        RGB(140,140,140),
        13
    );

    DrawTextSimple(
        hdc,
        x,
        y + 378,
        L"Registered tags only. Live Hive test pending.",
        RGB(140,140,140),
        13
    );
}


void DrawAutoRqDetails(HDC hdc, int x, int y);
void DrawTexturesDetails(HDC hdc, int x, int y);   // defined below, with the other Textures helpers

// "Game calls per second" line for the detail screens: shows whether the game runs a hooked function.
std::wstring HookRateLine(int which)
{
    MyClientFeatures::Stats r{};
    if (!MinecraftDataProvider::GetHookRates(r))
        return L"Game calls per second: ---";

    auto n = [](uint32_t v) { return std::to_wstring(v); };

    switch (which)
    {
    case 0:
        return L"Game calls per second: light map " + n(r.lightBuild) + L" / " + n(r.lightSplit) +
               L", light data " + n(r.lightCreate);
    case 1:
        return L"Game calls per second: name tags " + n(r.nameTag) + L", about you " + n(r.nameTagLocal);
    default:
        return L"Game calls per second: outline " + n(r.outline) + L", colour " + n(r.tessColor);
    }
}


void DrawFeatureDetails(HDC hdc)
{
    if (detailFeature ==
        DetailFeature::None)
    {
        return;
    }

    int x = 535;
    int y = 70;

    DrawTextSimple(
        hdc,
        x,
        y,
        L"Details",
        RGB(255,255,255),
        24
    );

    DrawTextSimple(
        hdc,
        x,
        y + 50,
        FeatureTitle(
            detailFeature
        ),
        RGB(255,255,255),
        20
    );


    if (detailFeature ==
        DetailFeature::Keystrokes)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"W / A / S / D",
            RGB(190,190,190),
            15
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Move and resize in HUD editor.",
            RGB(150,150,150),
            13
        );
    }
    else if (
        detailFeature ==
        DetailFeature::CPS)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"LMB and RMB are counted separately.",
            RGB(190,190,190),
            14
        );
    }
    else if (
        detailFeature ==
        DetailFeature::Armor)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Durability: ON",
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Live remaining / maximum durability.",
            RGB(150,150,150),
            13
        );
    }
    else if (
        detailFeature ==
        DetailFeature::Arrows)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Arrow count display",
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Live inventory and offhand arrow total.",
            RGB(150,150,150),
            13
        );
    }
    else if (
        detailFeature ==
        DetailFeature::FPS)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"FPS display",
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Minecraft render FPS (1-second average).",
            RGB(150,150,150),
            13
        );
    }
    else if (
        detailFeature ==
        DetailFeature::PartyAutoAccept)
    {
        DrawPartyDetails(
            hdc,
            x,
            y
        );
    }
    else if (
        detailFeature ==
        DetailFeature::BlockOutline)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Changes the colour of the game's own block outline.",
            RGB(220,220,220),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Look at a block to see it. Pick a colour:",
            RGB(150,150,150),
            13
        );

        for (int i = 0; i < 12; i++)
        {
            const RECT r = OutlineSwatchRect(i);

            const bool selected =
                blockOutlineSettings.color ==
                kOutlinePalette[i];

            // selection frame, then the colour itself
            DrawRoundBox(
                hdc, r.left - 3, r.top - 3,
                (r.right - r.left) + 6, (r.bottom - r.top) + 6, 9,
                selected ? RGB(255,255,255) : RGB(70,70,70)
            );

            DrawRoundBox(
                hdc, r.left, r.top,
                r.right - r.left, r.bottom - r.top, 7,
                kOutlinePalette[i]
            );
        }

        DrawTextSimple(
            hdc,
            x,
            y + 232,
            L"Color: RGB(" +
                std::to_wstring(GetRValue(blockOutlineSettings.color)) + L", " +
                std::to_wstring(GetGValue(blockOutlineSettings.color)) + L", " +
                std::to_wstring(GetBValue(blockOutlineSettings.color)) + L")",
            RGB(190,190,190),
            14
        );

        const bool available =
            MinecraftDataProvider::IsOutlineAvailable();
        const bool running =
            MinecraftDataProvider::IsOutlineRunning();
        const bool colored =
            MinecraftDataProvider::IsOutlineColored();

        const wchar_t* state =
            !blockOutlineEnabled
                ? L"State: OFF"
                : (!available
                    ? L"State: UNAVAILABLE (needs the updated DLL)"
                    : (colored
                        ? L"State: APPLIED"
                        : (running
                            ? L"State: RUNNING (colour not confirmed - check the outline)"
                            : L"State: WAITING (look at a block)")));

        DrawTextSimple(
            hdc,
            x,
            y + 260,
            state,
            (blockOutlineEnabled && colored)
                ? RGB(100,220,130)
                : (blockOutlineEnabled && !available
                    ? RGB(230,120,100)
                    : (blockOutlineEnabled && running
                        ? RGB(230,200,100)
                        : RGB(190,190,190))),
            14
        );

        {
            bool leaseFresh = false;
            unsigned int echoRgb = 0;
            const bool haveEcho = MinecraftDataProvider::GetOutlineEcho(leaseFresh, echoRgb);

            DrawTextSimple(
                hdc,
                x,
                y + 278,
                haveEcho
                    ? L"DLL holds: RGB(" + std::to_wstring((echoRgb >> 16) & 0xFF) + L", " +
                          std::to_wstring((echoRgb >> 8) & 0xFF) + L", " + std::to_wstring(echoRgb & 0xFF) +
                          L"), request " + (leaseFresh ? L"active" : L"NOT active")
                    : std::wstring(L"DLL holds: ---"),
                RGB(130,130,130),
                12
            );
        }

        DrawTextSimple(
            hdc,
            x,
            y + 294,
            HookRateLine(2),
            RGB(130,130,130),
            12
        );
    }
    else if (
        detailFeature ==
        DetailFeature::Textures)
    {
        DrawTexturesDetails(hdc, x, y);
    }
    else if (
        detailFeature ==
        DetailFeature::AutoRq)
    {
        DrawAutoRqDetails(hdc, x, y);
    }
    else if (
        detailFeature ==
        DetailFeature::FeatureList)
    {
        DrawTextSimple(
            hdc, x, y + 82,
            L"Shows the names of the features you have switched on.",
            RGB(220,220,220), 14
        );

        DrawTextSimple(
            hdc, x, y + 106,
            L"Move it in the HUD editor like the other elements.",
            RGB(170,170,170), 13
        );
    }


    else if (
        detailFeature ==
        DetailFeature::Ping)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Shows your ping (delay) to the server you are on.",
            RGB(220,220,220),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Measured by MyClient with one small UDP ping per second.",
            RGB(150,150,150),
            13
        );

        std::string pingHost;
        int pingPort = 0;
        const bool haveTarget =
            MinecraftDataProvider::GetPingTarget(pingHost, pingPort);

        int pingMs = 0;
        const bool known = PingMonitor::Get(pingMs);

        DrawTextSimple(
            hdc,
            x,
            y + 140,
            haveTarget
                ? L"Server: " + std::wstring(pingHost.begin(), pingHost.end()) + L":" + std::to_wstring(pingPort)
                : std::wstring(L"Server: ---"),
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 168,
            known
                ? L"Ping: " + std::to_wstring(pingMs) + L" ms"
                : std::wstring(L"Ping: ---"),
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 198,
            !pingEnabled
                ? L"State: OFF"
                : (!haveTarget
                    ? L"State: WAITING (join a server)"
                    : (known
                        ? L"State: MEASURING"
                        : L"State: NO REPLY (the server may ignore pings)")),
            (pingEnabled && known)
                ? RGB(100,220,130)
                : (pingEnabled && haveTarget
                    ? RGB(230,200,100)
                    : RGB(190,190,190)),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 228,
            L"Green: under 60 ms    Yellow: under 120 ms    Red: 120 ms or more",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 248,
            L"Not available on servers that do not use the standard UDP transport.",
            RGB(130,130,130),
            12
        );
    }


    else if (
        detailFeature ==
        DetailFeature::Nametag)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Shows your own name tag above your head.",
            RGB(220,220,220),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Meant for third-person view (front / back camera).",
            RGB(150,150,150),
            13
        );

        const bool available =
            MinecraftDataProvider::IsNametagAvailable();

        const bool applied =
            MinecraftDataProvider::IsNametagApplied();

        DrawTextSimple(
            hdc,
            x,
            y + 138,
            !nametagEnabled
                ? L"State: OFF"
                : (!available
                    ? L"State: UNAVAILABLE (needs the updated DLL)"
                    : (applied
                        ? L"State: APPLIED"
                        : L"State: WAITING (switch to third person)")),
            (nametagEnabled && applied)
                ? RGB(100,220,130)
                : (nametagEnabled && !available
                    ? RGB(230,120,100)
                    : RGB(190,190,190)),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 172,
            L"Turning it OFF or closing MyClient",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 190,
            L"returns the game to normal.",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 226,
            HookRateLine(1),
            RGB(130,130,130),
            12
        );
    }


    else if (
        detailFeature ==
        DetailFeature::Fullbright)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Keeps the game at full brightness.",
            RGB(220,220,220),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Dark caves and nights are lit like night vision.",
            RGB(150,150,150),
            13
        );

        const bool available =
            MinecraftDataProvider::IsFullbrightAvailable();

        const bool applied =
            MinecraftDataProvider::IsFullbrightApplied();

        DrawTextSimple(
            hdc,
            x,
            y + 138,
            !fullbrightEnabled
                ? L"State: OFF"
                : (!available
                    ? L"State: UNAVAILABLE (game hook not found)"
                    : (applied
                        ? L"State: APPLIED"
                        : L"State: WAITING FOR GAME")),
            (fullbrightEnabled && applied)
                ? RGB(100,220,130)
                : (fullbrightEnabled && !available
                    ? RGB(230,120,100)
                    : RGB(190,190,190)),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 172,
            L"Turning it OFF or closing MyClient",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 192,
            L"returns the game to normal within a second.",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 226,
            HookRateLine(0),
            RGB(130,130,130),
            12
        );
    }


    else if (
        detailFeature ==
        DetailFeature::Resources)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Iron / Gold / Diamond / Emerald totals",
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 108,
            L"Counts the whole inventory, hotbar and offhand.",
            RGB(150,150,150),
            13
        );

        DrawTextSimple(
            hdc,
            x,
            y + 130,
            L"Move and resize in the HUD editor.",
            RGB(150,150,150),
            13
        );
    }
    else if (
        detailFeature ==
        DetailFeature::Zoom)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 82,
            L"Hold the Zoom key for 3x zoom in game.",
            RGB(220,220,220),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 110,
            L"Key: " +
                KeyName(shortcuts.zoom),
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 138,
            MinecraftDataProvider::IsZoomApplied()
                ? L"State: APPLIED"
                : (zoomActive ? L"State: WAITING FOR GAME" : L"State: READY"),
            zoomActive
                ? RGB(100,220,130)
                : RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 172,
            L"Release the key or leave the game",
            RGB(130,130,130),
            12
        );

        DrawTextSimple(
            hdc,
            x,
            y + 192,
            L"window to restore normal FOV.",
            RGB(130,130,130),
            12
        );
    }

    if (detailFeature !=
        DetailFeature::PartyAutoAccept)
    {
        DrawTextSimple(
            hdc,
            x,
            y + 285,
            L"Shortcut Key",
            RGB(255,255,255),
            17
        );

        if (waitingForShortcut)
        {
            DrawBox(
                hdc,
                x,
                y + 315,
                260,
                42,
                RGB(130,85,45)
            );

            DrawTextSimple(
                hdc,
                x + 15,
                y + 325,
                L"Press a key...",
                RGB(255,255,255),
                16
            );
        }
        else
        {
            DrawButton(
                hdc,
                x,
                y + 315,
                260,
                42,
                L"Set Shortcut",
                false
            );
        }

        int* sc =
            GetShortcutPtr(
                detailFeature
            );

        DrawTextSimple(
            hdc,
            x,
            y + 370,
            L"Current: " +
                KeyName(
                    sc ? *sc : 0
                ),
            RGB(190,190,190),
            14
        );

        DrawTextSimple(
            hdc,
            x,
            y + 398,
            L"ESC = cancel",
            RGB(120,120,120),
            12
        );
    }
    else
    {
        int* sc =
            GetShortcutPtr(
                detailFeature
            );

        int sy =
            y + 400;

        DrawTextSimple(
            hdc,
            x,
            sy,
            L"Shortcut: " +
                KeyName(
                    sc ? *sc : 0
                ),
            RGB(190,190,190),
            14
        );

        if (waitingForShortcut)
        {
            DrawBox(
                hdc,
                x,
                sy + 25,
                260,
                42,
                RGB(130,85,45)
            );

            DrawTextSimple(
                hdc,
                x + 15,
                sy + 35,
                L"Press a key...",
                RGB(255,255,255),
                16
            );
        }
        else
        {
            DrawButton(
                hdc,
                x,
                sy + 25,
                260,
                42,
                L"Set Shortcut",
                false
            );
        }

        DrawTextSimple(
            hdc,
            x,
            sy + 75,
            L"G is reserved for MyClient menu.",
            RGB(120,120,120),
            12
        );
    }
}


// HUD editor: no menu panel (it used to cover the HUD at the top-left).
// Only a small toolbar at the bottom-centre of the game window with Reset / Done.
bool EditorToolbarRect(
    int& x,
    int& y,
    int& w,
    int& h)
{
    RECT r;

    if (!overlayWindow ||
        !GetClientRect(overlayWindow, &r))
    {
        return false;
    }

    w = 620;
    h = 64;
    x = std::max(0, (int)(r.right - w) / 2);
    y = std::max(0, (int)r.bottom - h - 28);

    return true;
}


void EditorButtonRects(
    int tx,
    int ty,
    int tw,
    RECT& reset,
    RECT& menu,
    RECT& done)
{
    done  = RECT{tx + tw - 16 - 96, ty + 15,
                 tx + tw - 16,      ty + 15 + 34};

    menu  = RECT{done.left - 12 - 88, ty + 15,
                 done.left - 12,      ty + 15 + 34};

    reset = RECT{menu.left - 12 - 88, ty + 15,
                 menu.left - 12,      ty + 15 + 34};
}


void DrawHudEditor(HDC hdc)
{
    int x, y, w, h;

    if (!EditorToolbarRect(x, y, w, h))
        return;

    const ThemeColors t = CurrentTheme();

    DrawRoundBox(hdc, x, y, w, h, 14, t.accent);
    DrawRoundBox(hdc, x + 2, y + 2, w - 4, h - 4, 12, t.bg);

    DrawTextSimple(
        hdc,
        x + 20,
        y + 9,
        L"HUD Editor",
        t.text,
        18
    );

    const bool notice =
        GetTickCount64() < editorNoticeUntil;

    DrawTextSimple(
        hdc,
        x + 20,
        y + 36,
        notice
            ? L"HUD positions were reset to the defaults."
            : L"Drag: move   Wheel: size   Esc: done",
        notice ? t.accentHi : t.muted,
        13
    );

    RECT reset, menu, done;
    EditorButtonRects(x, y, w, reset, menu, done);

    DrawRoundBox(
        hdc, menu.left, menu.top,
        menu.right - menu.left, menu.bottom - menu.top,
        8, t.button
    );

    DrawTextCentered(
        hdc, menu.left, menu.top,
        menu.right - menu.left, menu.bottom - menu.top,
        L"Menu", t.text, 15
    );

    DrawRoundBox(
        hdc, reset.left, reset.top,
        reset.right - reset.left, reset.bottom - reset.top,
        8, t.button
    );

    DrawTextCentered(
        hdc, reset.left, reset.top,
        reset.right - reset.left, reset.bottom - reset.top,
        L"Reset", t.text, 15
    );

    DrawRoundBox(
        hdc, done.left, done.top,
        done.right - done.left, done.bottom - done.top,
        8, t.accent
    );

    DrawTextCentered(
        hdc, done.left, done.top,
        done.right - done.left, done.bottom - done.top,
        L"Done", t.onAccent, 15
    );
}


void DrawSettings(HDC hdc)
{
    int x = 190;
    int y = 70;

    DrawTextSimple(
        hdc,
        x,
        y,
        L"Settings",
        RGB(255,255,255),
        26
    );

    DrawTextSimple(
        hdc,
        x,
        y + 48,
        L"Client Name: " +
            clientName,
        RGB(220,220,220),
        16
    );

    DrawTextSimple(
        hdc,
        x,
        y + 82,
        L"GUI Scale: " +
            std::to_wstring(
                guiScale
            ).substr(0,4),
        RGB(190,190,190),
        14
    );

    DrawTextSimple(
        hdc,
        x,
        y + 112,
        L"Menu Animation: " +
            std::wstring(
                menuAnimation
                    ? L"ON"
                    : L"OFF"
            ),
        RGB(190,190,190),
        14
    );

    DrawButton(
        hdc,
        x,
        y + 145,
        260,
        42,
        L"GUI Scale +",
        false
    );

    DrawButton(
        hdc,
        x,
        y + 195,
        260,
        42,
        L"GUI Scale -",
        false
    );

    DrawButton(
        hdc,
        x,
        y + 245,
        260,
        42,
        L"Toggle Animation",
        menuAnimation
    );

    DrawButton(
        hdc,
        x,
        y + 295,
        260,
        42,
        L"Reset HUD Layout",
        false
    );

    DrawButton(
        hdc,
        x,
        y + 345,
        260,
        42,
        std::wstring(L"Theme: ") +
            kThemes[
                std::clamp(
                    themeIndex,
                    0,
                    kThemeCount - 1
                )
            ].name,
        true
    );

    DrawTextSimple(
        hdc,
        x,
        y + 405,
        L"External Windows Client",
        RGB(130,130,130),
        13
    );
}


// ------------------------- Auto RQ page ----------------------------------------------------------
std::wstring AsciiToWide(const char* text)
{
    std::wstring out;
    while (text && *text) out.push_back((wchar_t)(unsigned char)*text++);
    return out;
}

// Two columns of five mode buttons (layout space like the Textures page: details origin x = 535).
RECT AutoRqModeRect(int index)
{
    const int col = index / 5;
    const int row = index % 5;
    const int left = 535 + col * 290;
    const int top = 200 + row * 46;
    return RECT{left, top, left + 270, top + 38};
}

void DrawAutoRqDetails(HDC hdc, int x, int y)
{
    const ThemeColors t = CurrentTheme();

    DrawTextSimple(
        hdc, x, y + 82,
        L"When you die on Hive, queue the next game for you.",
        t.muted, 13
    );

    DrawTextSimple(
        hdc, x, y + 106,
        autoRqEnabled
            ? L"Auto RQ: ON"
            : L"Auto RQ: OFF  (turn the Auto RQ card ON first)",
        autoRqEnabled ? RGB(100,220,130) : RGB(230,160,100),
        13
    );

    DrawTextSimple(
        hdc, x, y + 126,
        L"Solo only for now: it runs whenever you die. Party rules are not added yet.",
        t.muted, 12
    );

    for (int i = 0; i < (int)MyClientAutoRq::kModeCount; i++)
    {
        const RECT r = AutoRqModeRect(i);
        const bool chosen = i == autoRqMode;

        DrawRoundBox(
            hdc, r.left, r.top,
            r.right - r.left, r.bottom - r.top, 8,
            chosen ? t.accent : t.button
        );

        DrawTextCentered(
            hdc, r.left, r.top,
            r.right - r.left, r.bottom - r.top,
            AsciiToWide(MyClientAutoRq::kModes[i].name),
            t.text, 14
        );
    }

    DrawTextSimple(
        hdc, x, 200 + 5 * 46 + 4,
        L"Sends:  /" + AsciiToWide(MyClientAutoRq::kModes[autoRqMode].command),
        t.muted, 13
    );
}

// ------------------------- Textures page ---------------------------------------------------------
std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return L"";

    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);

    if (length <= 0)
        return std::wstring(text.begin(), text.end());

    std::wstring wide(length, L'\0');

    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &wide[0], length);

    return wide;
}

void RefreshTextures()
{
    texState =
        TexturePacks::Scan(
            TexturePacks::CandidateRootsFromEnvironment()
        );

    texScanned = true;
    texScroll = 0;
    texStatus = Utf8ToWide(texState.note);
}

// index 0 = default textures (no pack); 1.. = texState.packs[index - 1]
void ApplyTexturePack(int index)
{
    if (!texturesEnabled)
    {
        texStatus = L"Turn switching ON first.";
        return;
    }

    if (!texState.found ||
        index < 0 ||
        index > (int)texState.packs.size())
    {
        texStatus = L"Minecraft's data folder was not found.";
        return;
    }

    const TexturePacks::Pack* chosen =
        index == 0
            ? nullptr
            : &texState.packs[index - 1];

    std::string error;

    if (!TexturePacks::WriteActive(texState, chosen, error))
    {
        texStatus = L"Could not switch: " + Utf8ToWide(error);
        return;
    }

    texturesSeq++;   // asks the DLL to make the game re-read the list

    texStatus =
        L"Switched to: " +
        (chosen ? Utf8ToWide(chosen->name)
                : std::wstring(L"Default textures")) +
        L"  (reload requested)";
}

// Pack list inside the Textures settings page (layout space: details origin x = 535, y = 70).
const int kTexRowsVisible = 9;
const int kTexListTop = 246;
const int kTexRowPitch = 34;
const int kTexRowHeight = 30;
const RECT kTexRefreshRect{985, 148, 1095, 176};
const RECT kPackInstallRect{985, 180, 1095, 208};
const RECT kPackInboxRect{985, 212, 1095, 240};

std::filesystem::path PackInboxFolder()
{
    return TexturePacks::EnvPath("LOCALAPPDATA", L"LOCALAPPDATA") / L"MyClient" / L"packs-inbox";
}

// Unpacks a zip-based archive (.mcpack / .mcaddon / .zip) with the tar.exe that ships with Windows 10+.
bool ExtractWithTar(const std::filesystem::path& archive, const std::filesystem::path& destination)
{
    std::wstring command =
        L"tar.exe -xf \"" + archive.wstring() + L"\" -C \"" + destination.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;

    const DWORD waited = WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 1;

    if (waited == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    else
        TerminateProcess(process.hProcess, 1);

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return waited == WAIT_OBJECT_0 && code == 0;
}

void InstallInboxPacks()
{
    RefreshTextures();

    if (!texState.found)
    {
        texStatus = L"Minecraft data folder not found - cannot install packs.";
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(PackInboxFolder(), ec);

    const PackInstall::Summary r =
        PackInstall::InstallInbox(PackInboxFolder(), texState.root, ExtractWithTar);

    RefreshTextures();

    if (r.files == 0 && r.failed == 0)
    {
        texStatus = L"Inbox is empty. Put .mcpack / .mcaddon / .zip files in it first.";
        return;
    }

    texStatus =
        L"Installed " + std::to_wstring(r.Installed()) +
        L" (resource " + std::to_wstring(r.resource) +
        L", add-on " + std::to_wstring(r.behavior) +
        L", skin " + std::to_wstring(r.skin) + L")" +
        (r.failed ? L"  failed " + std::to_wstring(r.failed) : std::wstring()) +
        (r.skipped ? L"  skipped " + std::to_wstring(r.skipped) : std::wstring()) +
        L" - restart Minecraft for add-ons";
}

void DrawTexturesDetails(HDC hdc, int x, int y)
{
    if (!texScanned)
        RefreshTextures();

    const ThemeColors t = CurrentTheme();

    DrawTextSimple(
        hdc, x, y + 82,
        L"Pick a pack to switch to it right away.",
        t.muted, 13
    );

    DrawRoundBox(
        hdc, kTexRefreshRect.left, kTexRefreshRect.top,
        kTexRefreshRect.right - kTexRefreshRect.left,
        kTexRefreshRect.bottom - kTexRefreshRect.top, 8, t.button
    );

    DrawTextCentered(
        hdc, kTexRefreshRect.left, kTexRefreshRect.top,
        kTexRefreshRect.right - kTexRefreshRect.left,
        kTexRefreshRect.bottom - kTexRefreshRect.top,
        L"Refresh", t.text, 14
    );

    for (const RECT* r : {&kPackInstallRect, &kPackInboxRect})
    {
        DrawRoundBox(
            hdc, r->left, r->top,
            r->right - r->left, r->bottom - r->top, 8, t.button
        );

        DrawTextCentered(
            hdc, r->left, r->top,
            r->right - r->left, r->bottom - r->top,
            r == &kPackInstallRect ? L"Install all" : L"Open inbox",
            t.text, 14
        );
    }

    DrawTextSimple(
        hdc, x, y + 106,
        texturesEnabled
            ? L"Switching: ON"
            : L"Switching: OFF  (turn the Textures card ON first)",
        texturesEnabled ? RGB(100,220,130) : RGB(230,160,100),
        13
    );

    const bool available =
        MinecraftDataProvider::IsTexturesAvailable();

    unsigned int reloads = 0;
    const bool haveReloads =
        MinecraftDataProvider::GetTextureReloads(reloads);

    DrawTextSimple(
        hdc, x, y + 126,
        std::wstring(L"Reload function: ") +
            (available
                ? L"AVAILABLE"
                : L"UNAVAILABLE (needs the updated DLL)") +
            (haveReloads
                ? L"    Reloads done: " + std::to_wstring(reloads)
                : std::wstring()),
        available ? RGB(100,220,130) : RGB(230,120,100),
        13
    );

    DrawTextSimple(
        hdc, x, y + 148,
        texStatus.empty()
            ? L"A server that forces its own pack can still override this."
            : texStatus,
        t.muted, 12
    );

    // row 0 = default textures, then the installed packs
    const int total = (int)texState.packs.size() + 1;

    texScroll = std::clamp(texScroll, 0, std::max(0, total - kTexRowsVisible));

    for (int r = 0; r < kTexRowsVisible; r++)
    {
        const int index = texScroll + r;

        if (index >= total)
            break;

        const int rowY = kTexListTop + r * kTexRowPitch;

        const bool isDefault = index == 0;
        const TexturePacks::Pack* pack =
            isDefault ? nullptr : &texState.packs[index - 1];

        const bool active =
            isDefault
                ? texState.active.empty()
                : TexturePacks::IsActive(texState, *pack);

        DrawRoundBox(hdc, x, rowY, 560, kTexRowHeight, 8, active ? t.accent : t.card);

        DrawTextSimple(
            hdc, x + 14, rowY + 5,
            isDefault ? std::wstring(L"Default textures (no pack)")
                      : Utf8ToWide(pack->name),
            active ? t.onAccent : t.text, 16
        );

        if (pack)
        {
            DrawTextSimple(
                hdc, x + 400, rowY + 8,
                L"v" + std::to_wstring(pack->version[0]) + L"." +
                    std::to_wstring(pack->version[1]) + L"." +
                    std::to_wstring(pack->version[2]),
                active ? t.onAccent : t.muted, 13
            );
        }

        if (active)
            DrawTextSimple(hdc, x + 485, rowY + 8, L"ACTIVE", t.onAccent, 13);
    }

    if (total > kTexRowsVisible)
        DrawTextSimple(hdc, x, kTexListTop + kTexRowsVisible * kTexRowPitch, L"Scroll: mouse wheel", t.muted, 11);
}


// The G menu panel is 920x590 and starts at (10,10) in its own coordinates.
// Shift it so it sits in the middle of the game window. (The HUD layout editor
// keeps raw coordinates because it edits positions on the game screen.)
void MenuOffset(
    int& ox,
    int& oy)
{
    ox = 0;
    oy = 0;

    if (!overlayWindow || hudEditMode)
        return;

    RECT r;

    if (!GetClientRect(
            overlayWindow,
            &r))
    {
        return;
    }

    ox = std::max(
        0,
        (int)(r.right - 920) / 2 - 10
    );

    oy = std::max(
        0,
        (int)(r.bottom - 590) / 2 - 10
    );
}


void DrawMenu(HDC hdc)
{
    if (hudEditMode)
    {
        menuDrawing = false;   // the real in-game HUD is shown and edited here

        DrawHudEditor(hdc);

        return;
    }

    int offsetX = 0;
    int offsetY = 0;

    MenuOffset(offsetX, offsetY);

    menuDrawing = true;

    SetViewportOrgEx(
        hdc,
        offsetX,
        offsetY,
        nullptr
    );

    DrawRoundBox(
        hdc,
        10,
        10,
        920,
        590,
        18,
        CurrentTheme().bg
    );

    DrawNavigation(hdc);

    if (selectedCategory ==
        MenuCategory::Feature)
    {
        if (detailFeature ==
            DetailFeature::None)
        {
            DrawFeatureMenu(hdc);
        }
        else
        {
            // Settings page: only the settings, shifted to the left, plus a Back button.
            POINT origin{};
            GetViewportOrgEx(hdc, &origin);

            SetViewportOrgEx(
                hdc,
                origin.x - kDetailShift,
                origin.y,
                nullptr
            );

            const ThemeColors t = CurrentTheme();

            DrawRoundBox(hdc, 665, 66, 110, 32, 8, t.button);

            DrawTextCentered(
                hdc, 665, 66, 110, 32,
                L"< Back", t.text, 14
            );

            DrawFeatureDetails(hdc);

            SetViewportOrgEx(
                hdc,
                origin.x,
                origin.y,
                nullptr
            );
        }
    }
    else if (
        selectedCategory ==
        MenuCategory::Settings)
    {
        DrawSettings(hdc);
    }

    menuDrawing = false;

    SetViewportOrgEx(
        hdc,
        0,
        0,
        nullptr
    );
}


// ------------------------- Overlay control ------------------

// SetForegroundWindow alone is often refused for a background process; attaching
// to the foreground window's input thread for a moment makes it succeed.
void ForceForeground(HWND hwnd)
{
    HWND foreground =
        GetForegroundWindow();

    DWORD foregroundThread =
        foreground
            ? GetWindowThreadProcessId(
                  foreground,
                  nullptr
              )
            : 0;

    DWORD thisThread =
        GetCurrentThreadId();

    const bool attach =
        foregroundThread != 0 &&
        foregroundThread != thisThread;

    if (attach)
    {
        AttachThreadInput(
            thisThread,
            foregroundThread,
            TRUE
        );
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    if (attach)
    {
        AttachThreadInput(
            thisThread,
            foregroundThread,
            FALSE
        );
    }
}


void ActivateOverlay()
{
    if (!overlayWindow)
        return;

    LONG_PTR style =
        GetWindowLongPtrW(
            overlayWindow,
            GWL_EXSTYLE
        );

    style &= ~WS_EX_NOACTIVATE;

    SetWindowLongPtrW(
        overlayWindow,
        GWL_EXSTYLE,
        style
    );

    SetWindowPos(
        overlayWindow,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_SHOWWINDOW
    );

    // Cover the whole game window so the mouse stays with the menu.
    if (catcherWindow && minecraftWindow)
    {
        RECT gr;

        if (GetWindowRect(
                minecraftWindow,
                &gr))
        {
            SetWindowPos(
                catcherWindow,
                overlayWindow,
                gr.left,
                gr.top,
                gr.right - gr.left,
                gr.bottom - gr.top,
                SWP_NOACTIVATE |
                SWP_SHOWWINDOW
            );
        }
    }

    ForceForeground(
        overlayWindow
    );

    SetFocus(
        overlayWindow
    );

    ShowMouseCursor();
}


void DeactivateOverlay()
{
    if (!overlayWindow)
        return;

    if (catcherWindow)
    {
        ShowWindow(
            catcherWindow,
            SW_HIDE
        );
    }

    LONG_PTR style =
        GetWindowLongPtrW(
            overlayWindow,
            GWL_EXSTYLE
        );

    style |= WS_EX_NOACTIVATE;

    SetWindowLongPtrW(
        overlayWindow,
        GWL_EXSTYLE,
        style
    );

    ReleaseCapture();

    if (minecraftWindow)
    {
        SetForegroundWindow(
            minecraftWindow
        );
    }

    HideMouseCursor();
}


void OpenMenu()
{
    menuOpen = true;
    hudEditMode = false;

    selectedCategory =
        MenuCategory::Feature;

    detailFeature =
        DetailFeature::None;

    selectedHud =
        HudElementId::None;

    draggingHud =
        HudElementId::None;

    waitingForShortcut = false;
    partyAddingPlayer = false;

    ActivateOverlay();
}


void OpenHudEditor()
{
    menuOpen = true;
    hudEditMode = true;

    selectedCategory =
        MenuCategory::HUD;

    detailFeature =
        DetailFeature::None;

    selectedHud =
        HudElementId::None;

    draggingHud =
        HudElementId::None;

    waitingForShortcut = false;
    partyAddingPlayer = false;

    ActivateOverlay();
}


void CloseMenu()
{
    menuOpen = false;
    searchActive = false;
    hudEditMode = false;

    detailFeature =
        DetailFeature::None;

    selectedHud =
        HudElementId::None;

    draggingHud =
        HudElementId::None;

    waitingForShortcut = false;
    partyAddingPlayer = false;

    SaveConfig();

    DeactivateOverlay();
}


// ------------------------- Window Procedure -----------------

// HUD editor toolbar click. Returns true when the click was on the toolbar (and so is consumed).
//   Done  = finish editing and go back to the game (same result as pressing G)
//   Menu  = back to the normal menu
//   Reset = HUD layout back to the defaults
bool HandleEditorClick(int mx, int my)
{
    if (!menuOpen || !hudEditMode)
        return false;

    int tx, ty, tw, th;

    if (!EditorToolbarRect(tx, ty, tw, th))
        return false;

    if (mx < tx || mx >= tx + tw || my < ty || my >= ty + th)
        return false;

    RECT reset, menu, done;
    EditorButtonRects(tx, ty, tw, reset, menu, done);

    const POINT p{mx, my};

    if (PtInRect(&done, p))
        CloseMenu();
    else if (PtInRect(&menu, p))
        OpenMenu();
    else if (PtInRect(&reset, p))
    {
        ResetHudLayout();
        editorNoticeUntil = GetTickCount64() + 1800;
    }

    if (overlayWindow)
        InvalidateRect(overlayWindow, nullptr, FALSE);

    return true;
}


LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;


    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        HDC dc =
            BeginPaint(
                hwnd,
                &ps
            );

        RECT r;

        GetClientRect(
            hwnd,
            &r
        );

        int w = r.right;
        int h = r.bottom;

        // The back buffer is kept between paints (no per-frame allocation) and the window is only
        // blitted when the finished picture really differs from what is already on screen. A colour-keyed
        // layered window flickers when it is redrawn with identical content 60 times a second.
        static HDC back = nullptr;
        static HBITMAP dib = nullptr;
        static HBITMAP oldBitmap = nullptr;
        static uint32_t* bits = nullptr;
        static int bufferW = 0;
        static int bufferH = 0;
        static std::vector<uint32_t> shown;
        static bool shownValid = false;

        if (w > 0 && h > 0 && (!back || w != bufferW || h != bufferH))
        {
            if (back && oldBitmap)
                SelectObject(back, oldBitmap);

            if (dib)
                DeleteObject(dib);

            if (!back)
                back = CreateCompatibleDC(dc);

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -h;   // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            void* pixels = nullptr;
            dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            bits = static_cast<uint32_t*>(pixels);
            bufferW = w;
            bufferH = h;
            shownValid = false;
            shown.clear();

            if (dib)
                oldBitmap = (HBITMAP)SelectObject(back, dib);
        }

        if (back && dib && bits)
        {
            HBRUSH bg =
                CreateSolidBrush(
                    RGB(255,0,255)
                );

            FillRect(
                back,
                &r,
                bg
            );

            DeleteObject(bg);

            DrawHud(back);

            if (menuOpen)
            {
                DrawMenu(back);
            }

            GdiFlush();

            const size_t count = (size_t)bufferW * (size_t)bufferH;
            const bool same =
                shownValid &&
                shown.size() == count &&
                std::memcmp(shown.data(), bits, count * sizeof(uint32_t)) == 0;

            if (!same)
            {
                BitBlt(
                    dc,
                    0,
                    0,
                    w,
                    h,
                    back,
                    0,
                    0,
                    SRCCOPY
                );

                shown.assign(bits, bits + count);
                shownValid = true;
            }
        }


        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }


    case WM_NCHITTEST:
        if (!menuOpen)
            return HTTRANSPARENT;

        return HTCLIENT;


    case WM_KEYDOWN:
        if (menuOpen &&
            hudEditMode)
        {
            const int vk = (int)wParam;

            if (vk == VK_ESCAPE ||
                vk == VK_RETURN)
            {
                CloseMenu();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        if (menuOpen &&
            searchActive)
        {
            const int vk = (int)wParam;

            if (vk == VK_ESCAPE ||
                vk == VK_RETURN)
            {
                searchActive = false;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        if (menuOpen &&
            waitingForShortcut)
        {
            int vk =
                (int)wParam;

            if (vk == VK_ESCAPE)
            {
                waitingForShortcut = false;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (IsReservedShortcut(vk))
            {
                MessageBeep(
                    MB_ICONWARNING
                );

                return 0;
            }

            int* target =
                GetShortcutPtr(
                    detailFeature
                );

            if (target)
            {
                *target = vk;

                SaveConfig();

                waitingForShortcut = false;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        break;


    case WM_CHAR:
        if (menuOpen &&
            searchActive)
        {
            const wchar_t c =
                (wchar_t)wParam;

            if (c == L'\b')
            {
                if (!searchQuery.empty())
                    searchQuery.pop_back();
            }
            else if (c >= 32 &&
                     c != 127 &&
                     searchQuery.size() < 24)
            {
                searchQuery.push_back(c);
            }

            featureScrollRow = 0;

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }

        if (menuOpen &&
            partyAddingPlayer)
        {
            wchar_t c =
                (wchar_t)wParam;

            if (c == L'\r')
            {
                if (IsValidPlayerTag(
                        partyInput))
                {
                    bool duplicate = false;

                    for (const auto& p :
                         partyPlayers)
                    {
                        if (_wcsicmp(
                                p.c_str(),
                                partyInput.c_str()
                            ) == 0)
                        {
                            duplicate = true;
                        }
                    }

                    if (!duplicate && partyPlayers.size() < MyClientParty::MaxPlayers)
                    {
                        partyPlayers.push_back(
                            partyInput
                        );
                    }

                    partyInput.clear();
                    partyAddingPlayer = false;

                    SaveConfig();
                }
                else
                {
                    MessageBeep(
                        MB_ICONWARNING
                    );
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (c == L'\b')
            {
                if (!partyInput.empty())
                {
                    partyInput.pop_back();
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (c >= 32 &&
                c != 127 &&
                partyInput.size() < 32)
            {
                partyInput.push_back(c);

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }
        }

        break;


    case WM_LBUTTONDOWN:
    {
        if (!menuOpen)
            return 0;

        int mx =
            LOWORD(lParam);

        int my =
            HIWORD(lParam);


        if (hudEditMode)
        {
            if (HandleEditorClick(mx, my))
                return 0;

            HudElementId hit =
                GetHudAt(
                    mx,
                    my
                );

            if (hit !=
                HudElementId::None)
            {
                selectedHud = hit;
                draggingHud = hit;

                HudElement* e =
                    GetHudElement(hit);

                if (e)
                {
                    dragOffset.x =
                        mx - e->x;

                    dragOffset.y =
                        my - e->y;
                }

                SetCapture(hwnd);
            }
            else
            {
                selectedHud =
                    HudElementId::None;

                draggingHud =
                    HudElementId::None;
            }

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }


        {
            int ox, oy;

            MenuOffset(ox, oy);

            mx -= ox;
            my -= oy;
        }


        {
            const bool inSearch =
                selectedCategory ==
                    MenuCategory::Feature &&
                FeatureGridMode() &&
                IsInside(mx, my, 410, 64, 404, 36);

            searchActive = inSearch;

            if (inSearch)
            {
                waitingForShortcut = false;
                partyAddingPlayer = false;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }
        }


        if (IsInside(
                mx,
                my,
                25,
                80,
                135,
                55))
        {
            selectedCategory =
                MenuCategory::Feature;

            detailFeature =
                DetailFeature::None;

            waitingForShortcut = false;
            partyAddingPlayer = false;

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }


        if (IsInside(
                mx,
                my,
                25,
                150,
                135,
                55))
        {
            OpenHudEditor();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }


        if (IsInside(
                mx,
                my,
                25,
                220,
                135,
                55))
        {
            selectedCategory =
                MenuCategory::Settings;

            detailFeature =
                DetailFeature::None;

            waitingForShortcut = false;
            partyAddingPlayer = false;

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }


        if (selectedCategory ==
            MenuCategory::Feature)
        {
            if (!FeatureGridMode())
            {
                // Settings page: the page was drawn shifted left; bring the mouse back to its layout.
                mx += kDetailShift;

                if (IsInside(mx, my, 665, 66, 110, 32))
                {
                    detailFeature =
                        DetailFeature::None;

                    texScanned = false;

                    waitingForShortcut = false;
                    partyAddingPlayer = false;

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE
                    );

                    return 0;
                }
            }

            const std::vector<int> visible =
                VisibleFeatures();

            for (int pos = 0;
                 FeatureGridMode() &&
                 pos < (int)visible.size();
                 pos++)
            {
                const int i = visible[pos];

                if (!FeatureCardOnScreen(pos))
                    continue;

                int cx, cy, cw, ch;
                FeatureCardRect(pos, cx, cy, cw, ch);

                if (!IsInside(mx, my, cx, cy, cw, ch))
                    continue;

                int sx, sy, sw, sh;
                FeatureStarRect(pos, sx, sy, sw, sh);

                if (IsInside(mx, my, sx, sy, sw, sh))
                {
                    ToggleFavorite(
                        kFeatureList[i]
                    );

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE
                    );

                    return 0;
                }

                int gx, gy, gw, gh;
                FeatureGearRect(pos, gx, gy, gw, gh);

                if (FeatureGridMode() &&
                    IsInside(mx, my, gx, gy, gw, gh))
                {
                    detailFeature =
                        kFeatureList[i];

                    waitingForShortcut = false;
                    partyAddingPlayer = false;
                }
                else
                {
                    ToggleFeature(
                        kFeatureList[i]
                    );
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }


            if (detailFeature ==
                DetailFeature::PartyAutoAccept)
            {
                int listY =
                    70 + 108;

                for (
                    size_t i = 0;
                    i < partyPlayers.size() &&
                    i < 6;
                    i++)
                {
                    int yy =
                        listY +
                        (int)i * 34;

                    if (IsInside(
                            mx,
                            my,
                            535 + 238,
                            yy,
                            72,
                            28))
                    {
                        partyPlayers.erase(
                            partyPlayers.begin() +
                            i
                        );

                        SaveConfig();

                        InvalidateRect(
                            hwnd,
                            nullptr,
                            FALSE
                        );

                        return 0;
                    }
                }


                int addY =
                    listY +
                    std::min<int>(
                        (int)partyPlayers.size(),
                        6
                    ) * 34 +
                    8;

                if (IsInside(
                        mx,
                        my,
                        535,
                        addY,
                        310,
                        38))
                {
                    partyAddingPlayer = true;
                    partyInput.clear();

                    SetFocus(hwnd);

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE
                    );

                    return 0;
                }


                int sy =
                    70 + 400;

                if (IsInside(
                        mx,
                        my,
                        535,
                        sy + 25,
                        260,
                        42))
                {
                    waitingForShortcut = true;

                    SetFocus(hwnd);

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE
                    );

                    return 0;
                }
            }
            else if (
                detailFeature !=
                DetailFeature::None)
            {
                if (detailFeature ==
                    DetailFeature::AutoRq)
                {
                    for (int i = 0; i < (int)MyClientAutoRq::kModeCount; i++)
                    {
                        const RECT r = AutoRqModeRect(i);

                        if (IsInside(mx, my, r.left, r.top, r.right - r.left, r.bottom - r.top))
                        {
                            autoRqMode = i;
                            SaveConfig();
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }
                }

                if (detailFeature ==
                    DetailFeature::Textures)
                {
                    const POINT p{mx, my};

                    if (PtInRect(&kPackInboxRect, p))
                    {
                        std::error_code ec;
                        std::filesystem::create_directories(PackInboxFolder(), ec);
                        ShellExecuteW(nullptr, L"open", PackInboxFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        texStatus = L"Put your .mcpack / .mcaddon / .zip files in the folder, then press Install all.";
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    if (PtInRect(&kPackInstallRect, p))
                    {
                        InstallInboxPacks();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    if (PtInRect(&kTexRefreshRect, p))
                    {
                        RefreshTextures();

                        InvalidateRect(hwnd, nullptr, FALSE);

                        return 0;
                    }

                    const int total = (int)texState.packs.size() + 1;

                    for (int r = 0; r < kTexRowsVisible; r++)
                    {
                        const int index = texScroll + r;

                        if (index >= total)
                            break;

                        if (IsInside(mx, my, 535, kTexListTop + r * kTexRowPitch, 560, kTexRowHeight))
                        {
                            ApplyTexturePack(index);

                            InvalidateRect(hwnd, nullptr, FALSE);

                            return 0;
                        }
                    }
                }

                if (detailFeature ==
                    DetailFeature::BlockOutline)
                {
                    for (int i = 0; i < 12; i++)
                    {
                        const RECT r = OutlineSwatchRect(i);

                        if (IsInside(
                                mx,
                                my,
                                r.left,
                                r.top,
                                r.right - r.left,
                                r.bottom - r.top))
                        {
                            blockOutlineSettings.color =
                                kOutlinePalette[i];

                            SaveConfig();

                            InvalidateRect(
                                hwnd,
                                nullptr,
                                FALSE
                            );

                            return 0;
                        }
                    }
                }

                if (IsInside(
                        mx,
                        my,
                        535,
                        70 + 315,
                        260,
                        42))
                {
                    waitingForShortcut = true;

                    SetFocus(hwnd);

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE
                    );

                    return 0;
                }
            }
        }


        if (selectedCategory ==
            MenuCategory::Settings)
        {
            int x = 190;
            int y = 70;

            if (IsInside(
                    mx,
                    my,
                    x,
                    y + 145,
                    260,
                    42))
            {
                guiScale =
                    std::min(
                        1.5f,
                        guiScale + 0.05f
                    );

                SaveConfig();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (IsInside(
                    mx,
                    my,
                    x,
                    y + 195,
                    260,
                    42))
            {
                guiScale =
                    std::max(
                        0.75f,
                        guiScale - 0.05f
                    );

                SaveConfig();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (IsInside(
                    mx,
                    my,
                    x,
                    y + 245,
                    260,
                    42))
            {
                menuAnimation =
                    !menuAnimation;

                SaveConfig();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (IsInside(
                    mx,
                    my,
                    x,
                    y + 295,
                    260,
                    42))
            {
                ResetHudLayout();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }

            if (IsInside(
                    mx,
                    my,
                    x,
                    y + 345,
                    260,
                    42))
            {
                themeIndex =
                    (themeIndex + 1) %
                    kThemeCount;

                SaveConfig();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }
        }

        return 0;
    }


    case WM_RBUTTONDOWN:
    {
        if (!menuOpen ||
            hudEditMode ||
            !FeatureGridMode() ||
            selectedCategory !=
                MenuCategory::Feature)
        {
            return 0;
        }

        int mx =
            LOWORD(lParam);

        int my =
            HIWORD(lParam);

        {
            int ox, oy;

            MenuOffset(ox, oy);

            mx -= ox;
            my -= oy;
        }

        const std::vector<int> visible =
            VisibleFeatures();

        for (int pos = 0;
             pos < (int)visible.size();
             pos++)
        {
            if (!FeatureCardOnScreen(pos))
                continue;

            int cx, cy, cw, ch;
            FeatureCardRect(pos, cx, cy, cw, ch);

            if (IsInside(mx, my, cx, cy, cw, ch))
            {
                detailFeature =
                    kFeatureList[visible[pos]];

                waitingForShortcut = false;
                partyAddingPlayer = false;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );

                return 0;
            }
        }

        return 0;
    }


    case WM_MOUSEMOVE:
    {
        if (hudEditMode &&
            draggingHud !=
                HudElementId::None)
        {
            if (!(wParam & MK_LBUTTON))
            {
                draggingHud =
                    HudElementId::None;

                ReleaseCapture();

                return 0;
            }

            int mx =
                LOWORD(lParam);

            int my =
                HIWORD(lParam);

            HudElement* e =
                GetHudElement(
                    draggingHud
                );

            if (e)
            {
                e->x =
                    std::max(
                        0,
                        mx -
                            static_cast<int>(dragOffset.x)
                    );

                e->y =
                    std::max(
                        0,
                        my -
                            static_cast<int>(dragOffset.y)
                    );
            }

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }

        return 0;
    }


    case WM_LBUTTONUP:
        if (hudEditMode)
        {
            draggingHud =
                HudElementId::None;

            ReleaseCapture();

            SaveConfig();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }

        return 0;


    case WM_MOUSEWHEEL:
    {
        if (menuOpen &&
            !hudEditMode &&
            selectedCategory ==
                MenuCategory::Feature &&
            detailFeature ==
                DetailFeature::None)
        {
            featureScrollRow -=
                GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;

            ClampFeatureScroll(
                (int)VisibleFeatures().size()
            );

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }

        if (menuOpen &&
            !hudEditMode &&
            selectedCategory ==
                MenuCategory::Feature &&
            detailFeature ==
                DetailFeature::Textures)
        {
            texScroll -=
                GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;
        }

        if (!hudEditMode ||
            selectedHud ==
                HudElementId::None)
        {
            return 0;
        }

        int delta =
            GET_WHEEL_DELTA_WPARAM(
                wParam
            );

        HudElement* e =
            GetHudElement(
                selectedHud
            );

        if (e)
        {
            e->scale +=
                delta > 0
                    ? 0.05f
                    : -0.05f;

            e->scale =
                std::clamp(
                    e->scale,
                    0.5f,
                    2.5f
                );

            SaveConfig();
        }

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE
        );

        return 0;
    }


    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}


// ------------------------- Create overlay -------------------

LRESULT CALLBACK CatcherProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

void CreateOverlay()
{
    HINSTANCE instance =
        GetModuleHandleW(nullptr);

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        instance;

    wc.lpszClassName =
        L"MyClientOverlay";

    wc.hIcon =
        LoadIconW(
            instance,
            MAKEINTRESOURCEW(1)
        );

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    RegisterClassW(
        &wc
    );


    overlayWindow =
        CreateWindowExW(
            WS_EX_LAYERED |
            WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,

            L"MyClientOverlay",
            L"MyClient",
            WS_POPUP,

            0,
            0,
            100,
            100,

            nullptr,
            nullptr,
            instance,
            nullptr
        );


    SetLayeredWindowAttributes(
        overlayWindow,
        RGB(255,0,255),
        255,
        LWA_COLORKEY
    );


    ShowWindow(
        overlayWindow,
        SW_SHOWNOACTIVATE
    );


    WNDCLASSW cc{};

    cc.lpfnWndProc =
        CatcherProc;

    cc.hInstance =
        instance;

    cc.lpszClassName =
        L"MyClientInputCatcher";

    cc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    cc.hbrBackground =
        (HBRUSH)GetStockObject(
            BLACK_BRUSH
        );

    RegisterClassW(
        &cc
    );

    catcherWindow =
        CreateWindowExW(
            WS_EX_LAYERED |
            WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,

            L"MyClientInputCatcher",
            L"MyClientInput",
            WS_POPUP,

            0,
            0,
            100,
            100,

            nullptr,
            nullptr,
            instance,
            nullptr
        );

    if (catcherWindow)
    {
        // Alpha 1/255: invisible to the eye, but it still receives the mouse.
        SetLayeredWindowAttributes(
            catcherWindow,
            0,
            1,
            LWA_ALPHA
        );
    }
}


// ------------------------- Main ------------------------------

int WINAPI WinMain(
    HINSTANCE,
    HINSTANCE,
    LPSTR,
    int)
{
    HANDLE sessionMutex = nullptr;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    std::wstring argument = argc == 2 ? argv[1] : L"";
    LocalFree(argv);
    if (argc != 1 && argc != 2) return 1;
    if (argc == 2)
    {
        uint32_t pid = 0;
        if (!MyClientLauncher::ParsePidOption(argument, pid)) return 1;
        requestedMinecraftPid = pid;
        const auto mutexName = L"Local\\MyClient.Hud." + std::to_wstring(requestedMinecraftPid);
        sessionMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        const DWORD error = GetLastError();
        if (!sessionMutex) return 1;
        if (error == ERROR_ALREADY_EXISTS) { CloseHandle(sessionMutex); return 0; }
    }
    struct SessionGuard {
        HANDLE handle;
        ~SessionGuard() { if (handle) CloseHandle(handle); }
    } sessionGuard{sessionMutex};
    LoadConfig();

    CreateOverlay();

    MSG msg{};

    RECT previousMinecraftRect{};

    bool hasPreviousRect =
        false;


    bool prevKS = false;
    bool prevCPS = false;
    bool prevArmor = false;
    bool prevArrows = false;
    bool prevFPS = false;
    bool prevParty = false;
    bool prevBlockOutline = false;


    bool previousLeft = false;
    bool previousRight = false;
    bool providerInitialized = false;


    while (true)
    {
        while (PeekMessageW(
            &msg,
            nullptr,
            0,
            0,
            PM_REMOVE))
        {
            if (msg.message ==
                WM_QUIT)
            {
                HideMouseCursor();
                MinecraftDataProvider::Shutdown();
                return 0;
            }

            TranslateMessage(
                &msg
            );

            DispatchMessageW(
                &msg
            );
        }


        minecraftWindow =
            FindMinecraft();

        if (minecraftWindow && !providerInitialized)
        {
            providerInitialized =
                MinecraftDataProvider::Initialize(
                    minecraftWindow
                );
        }


        if (!minecraftWindow)
        {
            if (menuOpen)
            {
                CloseMenu();
            }

            if (catcherWindow)
            {
                DestroyWindow(
                    catcherWindow
                );
            }

            if (overlayWindow)
            {
                DestroyWindow(
                    overlayWindow
                );
            }

            MinecraftDataProvider::Shutdown();
            return 0;
        }

        MinecraftDataProvider::Tick(
            minecraftWindow
        );


        RECT mr;

        GetWindowRect(
            minecraftWindow,
            &mr
        );


        // The overlay is topmost, so hide it whenever Minecraft is not the active
        // window (another app in front, or Minecraft minimized). The overlay itself
        // counts as active while the G menu is open.
        const HWND foregroundWindow = GetForegroundWindow();
        const bool minecraftActive =
            !IsIconic(minecraftWindow) &&
            (foregroundWindow == minecraftWindow ||
             foregroundWindow == overlayWindow ||
             (catcherWindow &&
              foregroundWindow == catcherWindow));
        gameActive = minecraftActive;
        {
            const bool overlayShown = IsWindowVisible(overlayWindow) != FALSE;
            // Hide only after Minecraft has been inactive for a moment: a notification or another
            // window briefly taking focus must not make the overlay vanish and pop back (flicker).
            static ULONGLONG inactiveSince = 0;

            if (minecraftActive)
                inactiveSince = 0;
            else if (inactiveSince == 0)
                inactiveSince = GetTickCount64();

            if (!minecraftActive && overlayShown &&
                GetTickCount64() - inactiveSince >= 400)
            {
                ShowWindow(overlayWindow, SW_HIDE);
            }
            else if (minecraftActive && !overlayShown)
            {
                ShowWindow(overlayWindow, SW_SHOWNOACTIVATE);
                hasPreviousRect = false; // Force repositioning over the game.
            }
        }


        bool changed =
            !hasPreviousRect ||
            mr.left !=
                previousMinecraftRect.left ||
            mr.top !=
                previousMinecraftRect.top ||
            mr.right !=
                previousMinecraftRect.right ||
            mr.bottom !=
                previousMinecraftRect.bottom;


        if (changed)
        {
            int width =
                mr.right -
                mr.left;

            int height =
                mr.bottom -
                mr.top;


            SetWindowPos(
                overlayWindow,
                HWND_TOPMOST,
                mr.left,
                mr.top,
                width,
                height,
                SWP_NOACTIVATE |
                SWP_NOSENDCHANGING
            );


            if (catcherWindow && menuOpen)
            {
                SetWindowPos(
                    catcherWindow,
                    overlayWindow,
                    mr.left,
                    mr.top,
                    width,
                    height,
                    SWP_NOACTIVATE
                );
            }

            previousMinecraftRect =
                mr;

            hasPreviousRect =
                true;


            InvalidateRect(
                overlayWindow,
                nullptr,
                FALSE
            );
        }


        // G is a global key state: ignore it while typing in another app.
        // ...and typing a "g" into a text box must not close the menu.
        const bool typingText =
            menuOpen &&
            (searchActive ||
             partyAddingPlayer);

        bool g =
            minecraftActive &&
            !typingText &&
            IsKeyDown('G');


        if (g &&
            !previousGState)
        {
            if (!menuOpen)
            {
                OpenMenu();
            }
            else
            {
                CloseMenu();
            }

            InvalidateRect(
                overlayWindow,
                nullptr,
                FALSE
            );
        }


        previousGState =
            g;


        // Fallback for the HUD editor toolbar: read the mouse button state directly, in case the
        // click message goes to another window (the shared handler ignores everything except the toolbar).
        {
            static bool previousLeft = false;

            const bool left =
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            if (left && !previousLeft &&
                menuOpen && hudEditMode && overlayWindow)
            {
                POINT cursor;

                if (GetCursorPos(&cursor) &&
                    ScreenToClient(overlayWindow, &cursor))
                {
                    HandleEditorClick(cursor.x, cursor.y);
                }
            }

            previousLeft = left;
        }


        CheckShortcut(
            shortcuts.keystrokes,
            prevKS,
            DetailFeature::Keystrokes
        );


        CheckShortcut(
            shortcuts.cps,
            prevCPS,
            DetailFeature::CPS
        );


        CheckShortcut(
            shortcuts.armor,
            prevArmor,
            DetailFeature::Armor
        );


        CheckShortcut(
            shortcuts.arrows,
            prevArrows,
            DetailFeature::Arrows
        );


        CheckShortcut(
            shortcuts.fps,
            prevFPS,
            DetailFeature::FPS
        );


        CheckShortcut(
            shortcuts.party,
            prevParty,
            DetailFeature::PartyAutoAccept
        );


        CheckShortcut(
            shortcuts.blockOutline,
            prevBlockOutline,
            DetailFeature::BlockOutline
        );

        // Zoom is a hold action rather than a toggle.
        // Heartbeat expires in the DLL if the HUD stops or loses focus.
        zoomActive =
            IsZoomRequested();

        MinecraftDataProvider::SetZoomRequested(
            zoomActive, static_cast<uint32_t>(shortcuts.zoom)
        );
        // Full Bright is a level, not a hold: refreshed every tick so the
        // DLL's 250ms lease expires if this HUD stops or crashes.
        MinecraftDataProvider::SetFullbrightRequested(
            fullbrightEnabled
        );
        MinecraftDataProvider::SetNametagRequested(
            nametagEnabled
        );
        MinecraftDataProvider::SetTextures(
            texturesEnabled,
            texturesSeq
        );
        MinecraftDataProvider::SetAutoRq(
            autoRqEnabled ? (unsigned int)autoRqMode + 1u : 0u
        );
        {
            std::string pingHost;
            int pingPort = 0;
            const bool havePingTarget =
                pingEnabled &&
                MinecraftDataProvider::GetPingTarget(pingHost, pingPort);

            PingMonitor::SetTarget(
                havePingTarget ? pingHost : std::string(),
                pingPort,
                havePingTarget
            );
        }
        MinecraftDataProvider::SetOutlineRequested(
            blockOutlineEnabled,
            (unsigned)GetRValue(blockOutlineSettings.color) << 16 |
            (unsigned)GetGValue(blockOutlineSettings.color) << 8 |
            (unsigned)GetBValue(blockOutlineSettings.color)
        );
        MinecraftDataProvider::SetPartyRequested(partyAutoAcceptEnabled, partyPlayers);


        bool left =
            IsKeyDown(
                VK_LBUTTON
            );

        bool right =
            IsKeyDown(
                VK_RBUTTON
            );


        // Mouse buttons are polled globally: only count clicks made in Minecraft.
        if (!menuOpen && minecraftActive)
        {
            if (left &&
                !previousLeft)
            {
                AddLeftClick();
            }

            if (right &&
                !previousRight)
            {
                AddRightClick();
            }
        }


        previousLeft =
            left;

        previousRight =
            right;


        InvalidateRect(
            overlayWindow,
            nullptr,
            FALSE
        );


        Sleep(16);
    }


    return 0;
}
