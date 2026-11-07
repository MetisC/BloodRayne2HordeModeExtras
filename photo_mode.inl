// BR1 controls on BR2's native pause and world renderer. All state lives on
// the game thread. The photo pose is submitted only to the render camera.
namespace {

constexpr DWORD MainPhotoView = 0x7C0A08;
constexpr DWORD CurrentPhotoView = 0x7C0A0C;
constexpr DWORD PhotoScript = 0x7C5928;
constexpr DWORD NativeMouseX = 0x5E214B0;
constexpr DWORD NativeMouseY = 0x5E214B4;
constexpr WORD PhotoChord = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
constexpr WORD PhotoExitButtons = XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START;

struct PhotoVec { float x, y, z; };
struct PhotoPose { PhotoVec position, angles; float fov; };
struct PhotoPad {
    float strafe, forward, yaw, pitch, lift;
    WORD buttons;
};

bool g_photoReady;
bool g_photoRequested;
bool g_photoDrawing;
bool g_photoFromGameplay;
bool g_photoExitArmed;
bool g_photoExitPending;
bool g_photoExitReady;
bool g_photoF12Down;
WORD g_photoPreviousPad[4];
int g_photoPadSlot;
DWORD g_photoTickAt;
DWORD g_photoHero;
DWORD g_photoSituation;
DWORD g_photoLastHero;
BYTE* g_photoLastView;
PhotoPose g_photoNativePose;
PhotoPose g_photoPose;

template <class T> T& PhotoGlobal(DWORD address) {
    return *reinterpret_cast<T*>(address);
}

template <class T> T& PhotoField(BYTE* object, DWORD offset) {
    return *reinterpret_cast<T*>(object + offset);
}

using PhotoGameCall = void (__thiscall*)(BYTE*);
using PhotoSetCamera = void (__stdcall*)(const PhotoVec*, const PhotoVec*, float, const PhotoVec*);

bool PhotoKey(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

float PhotoAxis(float value, float deadzone) {
    value = max(-1.0f, min(1.0f, value));
    if (value > deadzone) return (value - deadzone) / (1.0f - deadzone);
    if (value < -deadzone) return (value + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

float PhotoTrigger(BYTE value) {
    return max(0, value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
           / float(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
}

bool ReadPhotoPad(int slot, PhotoPad& out) {
    // Same XInput thunk and DirectInput pad slots as BR2's input poll.
    using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    XINPUT_STATE state = {};
    if (reinterpret_cast<GetState>(0x6CB192)(slot, &state) == ERROR_SUCCESS) {
        const auto& p = state.Gamepad;
        out = { PhotoAxis(p.sThumbLX / 32768.0f, 0.24f),
                PhotoAxis(p.sThumbLY / 32768.0f, 0.24f),
                PhotoAxis(p.sThumbRX / 32768.0f, 0.26f),
                PhotoAxis(p.sThumbRY / 32768.0f, 0.26f),
                PhotoTrigger(p.bRightTrigger) - PhotoTrigger(p.bLeftTrigger), p.wButtons };
        return true;
    }
    auto* device = PhotoGlobal<IDirectInputDevice8A*>(0x61B5948 + slot * 4);
    static IDirectInputDevice8A* previous[4];
    static bool sony[4];
    if (device != previous[slot]) {
        previous[slot] = device;
        sony[slot] = false;
        if (device) {
            DIDEVICEINSTANCEA info = {};
            info.dwSize = sizeof(info);
            sony[slot] = SUCCEEDED(device->GetDeviceInfo(&info))
                         && LOWORD(info.guidProduct.Data1) == 0x054C;
        }
    }
    const DWORD pad = 0x7E10A0 + slot * 0x1E4;
    if (!device || !sony[slot] || !PhotoGlobal<DWORD>(pad + 4)) return false;
    const auto* buttons = &PhotoGlobal<DWORD>(pad + 0xE0);
    out = { PhotoAxis(PhotoGlobal<float>(pad + 0x28), 0.24f),
            PhotoAxis(-PhotoGlobal<float>(pad + 0x2C), 0.24f),
            PhotoAxis(PhotoGlobal<float>(pad + 0x30), 0.26f),
            PhotoAxis(-PhotoGlobal<float>(pad + 0x3C), 0.26f),
            float((buttons[7] || buttons[5]) - (buttons[6] || buttons[4])),
            WORD((buttons[2] ? XINPUT_GAMEPAD_B : 0)
               | (buttons[9] ? XINPUT_GAMEPAD_START : 0)
               | (buttons[10] ? XINPUT_GAMEPAD_LEFT_THUMB : 0)
               | (buttons[11] ? XINPUT_GAMEPAD_RIGHT_THUMB : 0)) };
    return true;
}

void MovePhotoCamera(const PhotoPad& pad, LONG mouseX, LONG mouseY, float dt) {
    const float pitchLimit = 1.553343f;
    auto& pose = g_photoPose;
    pose.angles.y = remainderf(pose.angles.y + pad.yaw * 1.9f * dt + mouseX * 0.0025f,
                               6.2831853f);
    pose.angles.x = max(-pitchLimit, min(pitchLimit,
        pose.angles.x - pad.pitch * 1.9f * dt + mouseY * 0.0025f));

    const float right = pad.strafe + PhotoKey('D') - PhotoKey('A');
    const float front = pad.forward + PhotoKey('W') - PhotoKey('S');
    const float lift = pad.lift + (PhotoKey(VK_SPACE) || PhotoKey('E'))
                               - (PhotoKey(VK_CONTROL) || PhotoKey('Q'));
    const float length = sqrtf(right * right + front * front + lift * lift);
    const float speed = (PhotoKey(VK_SHIFT) ? 65.0f : 24.0f) * dt / max(1.0f, length);
    const float sy = sinf(pose.angles.y), cy = cosf(pose.angles.y);
    const float cp = cosf(pose.angles.x), sp = sinf(pose.angles.x);
    pose.position.x += speed * (cy * right + sy * cp * front);
    pose.position.y += speed * (lift - sp * front);
    pose.position.z += speed * (-sy * right + cy * cp * front);
}

bool PhotoCanEnter() {
    auto* view = PhotoGlobal<BYTE*>(MainPhotoView);
    auto* script = PhotoGlobal<BYTE*>(PhotoScript);
    return g_photoReady && PhotoGlobal<BYTE*>(G_GAME_STATE)
        && PhotoGlobal<DWORD>(0x7DCD68) && PhotoGlobal<DWORD>(0x7C2478)
        && view && PhotoField<DWORD>(view, 0) == 0x6F9AEC
        && script && !PhotoField<DWORD>(script, 8)
        && !PhotoGlobal<BYTE>(0x6030408)
        && isfinite(PhotoField<float>(view, 0x1C)) && PhotoField<float>(view, 0x1C) > 0;
}

void SetPhotoAudioPause(bool paused) {
    using PauseSound = void (__thiscall*)(BYTE*, int);
    reinterpret_cast<PauseSound>(0x629DC0)(PhotoGlobal<BYTE*>(0x7C6308), paused);
    reinterpret_cast<void (__cdecl*)()>(paused ? 0x5CD1D0 : 0x5CD100)();
}

void BeginPhoto(DWORD now, int padSlot, bool fromGameplay) {
    auto* view = PhotoGlobal<BYTE*>(MainPhotoView);
    g_photoHero = PhotoGlobal<DWORD>(0x7DCD68);
    g_photoSituation = PhotoGlobal<DWORD>(0x7C2478);
    g_photoPose = g_photoLastView == view && g_photoLastHero == g_photoHero
        ? g_photoNativePose : PhotoPose{PhotoField<PhotoVec>(view, 4),
                                      PhotoField<PhotoVec>(view, 0x10),
                                      PhotoField<float>(view, 0x1C)};
    g_photoPadSlot = padSlot;
    g_photoTickAt = now;
    g_photoExitArmed = g_photoExitPending = g_photoExitReady = false;
    g_photoFromGameplay = fromGameplay;
    if (fromGameplay) SetPhotoAudioPause(true);
    PhotoGlobal<LONG>(NativeMouseX) = PhotoGlobal<LONG>(NativeMouseY) = 0;
    g_photoBlockingMenu = true;
}

void ResumePhotoGameplay(BYTE* game) {
    // The shortcut keeps the native menu ID at zero. Clear the gamepad edge
    // queue and the native Escape latch before yielding back to gameplay.
    reinterpret_cast<void (__thiscall*)(BYTE*, int)>(0x4F5640)(game, 0);
    PhotoGlobal<BYTE>(0x5E00829) = 0;
    SetPhotoAudioPause(false);
}

void __fastcall PhotoBeforeSimulation(BYTE* game, void*) {
    if (!g_photoReady) {
        reinterpret_cast<PhotoGameCall>(0x4F4740)(game);
        return;
    }

    const int menu = PhotoGlobal<int>(G_MENU_STATE);
    const int photoMenu = g_photoFromGameplay ? 0 : MENU_PAUSE;
    if (g_photoBlockingMenu && (menu != photoMenu || !PhotoCanEnter()
            || g_photoHero != PhotoGlobal<DWORD>(0x7DCD68)
            || g_photoSituation != PhotoGlobal<DWORD>(0x7C2478))) {
        if (g_photoFromGameplay && menu == 0) ResumePhotoGameplay(game);
        g_photoBlockingMenu = false;
    }

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool focused = foregroundPid == GetCurrentProcessId();
    const bool f12 = focused && PhotoKey(VK_F12);
    const bool f12Pressed = f12 && !g_photoF12Down;
    g_photoF12Down = f12;
    PhotoPad pads[4] = {};
    bool connected[4] = {};
    int firstPad = -1, chordSlot = -1;
    bool padExit = false;
    if (focused && (menu == 0 || menu == MENU_PAUSE)) {
        for (int i = 0; i < 4; ++i) {
            connected[i] = ReadPhotoPad(i, pads[i]);
            if (connected[i] && firstPad < 0) firstPad = i;
            const WORD buttons = pads[i].buttons;
            if ((buttons & PhotoChord) == PhotoChord &&
                (g_photoPreviousPad[i] & PhotoChord) != PhotoChord)
                chordSlot = i;
            g_photoPreviousPad[i] = buttons;
            padExit |= (buttons & PhotoExitButtons) != 0;
        }
    }

    const DWORD now = GetTickCount();
    if (!g_photoBlockingMenu) {
        const bool shortcut = menu == 0 && !PhotoGlobal<BYTE>(0x5E335BD)
                           && (f12Pressed || chordSlot >= 0);
        const bool requested = (g_photoRequested && menu == MENU_PAUSE) || shortcut;
        g_photoRequested = false;
        if (!requested || !PhotoCanEnter()) {
            reinterpret_cast<PhotoGameCall>(0x4F4740)(game);
            return;
        }
        BeginPhoto(now, chordSlot >= 0 ? chordSlot : firstPad, shortcut);
    }

    const bool exitHeld = padExit || PhotoKey(VK_ESCAPE);
    if (focused) {
        if (exitHeld && g_photoExitArmed) g_photoExitPending = true;
        if (!exitHeld) {
            g_photoExitArmed = true;
            g_photoExitReady = g_photoExitPending;
        }
    }
    LONG& mouseX = PhotoGlobal<LONG>(NativeMouseX);
    LONG& mouseY = PhotoGlobal<LONG>(NativeMouseY);
    const float dt = min(now - g_photoTickAt, 50ul) * 0.001f;
    g_photoTickAt = now;
    if (focused && !g_photoExitPending) {
        const int slot = g_photoPadSlot >= 0 && connected[g_photoPadSlot]
                       ? g_photoPadSlot : firstPad;
        MovePhotoCamera(slot >= 0 ? pads[slot] : PhotoPad{}, mouseX, mouseY, dt);
    }
    mouseX = mouseY = 0;
}

void __fastcall PhotoUpdateWorld(BYTE* game, void*) {
    // This call is reached whenever the native menu ID is zero. A photo
    // session entered by shortcut freezes the update here, outside the UI.
    if (!g_photoBlockingMenu)
        reinterpret_cast<PhotoGameCall>(0x4F8CD0)(game);
}

void __stdcall PhotoSubmitCamera(const PhotoVec* pos, const PhotoVec* angles,
                                 float fov, const PhotoVec* listener) {
    const auto original = reinterpret_cast<PhotoSetCamera>(0x5E1A10);
    if (g_photoDrawing) {
        original(&g_photoPose.position, &g_photoPose.angles, g_photoPose.fov, &g_photoPose.position);
        return;
    }
    auto* view = PhotoGlobal<BYTE*>(CurrentPhotoView);
    if (view == PhotoGlobal<BYTE*>(MainPhotoView)) {
        g_photoLastView = view;
        g_photoLastHero = PhotoGlobal<DWORD>(0x7DCD68);
        g_photoNativePose = {*pos, *angles, fov};
    }
    original(pos, angles, fov, listener);
}

void __fastcall PhotoViewHud(BYTE* hud, void*, int flags) {
    if (!g_photoDrawing)
        reinterpret_cast<void (__thiscall*)(BYTE*, int)>(0x594D40)(hud, flags);
}

void __fastcall PhotoPostProcess(BYTE* effect, void*, float x, float y, float w, float h) {
    if (!g_photoDrawing)
        reinterpret_cast<void (__thiscall*)(BYTE*, float, float, float, float)>(0x50AE40)(
            effect, x, y, w, h);
}

void DrawPhotoWorld(BYTE* game) {
    auto* view = PhotoGlobal<BYTE*>(MainPhotoView);
    auto* script = PhotoGlobal<BYTE*>(PhotoScript);
    auto* vision = PhotoGlobal<BYTE*>(0x754690);
    BYTE savedView[0x790];  // CGameView size copied by the native pause code.
    BYTE savedVision[0x18];
    float savedBasis[9];
    memcpy(savedView, view, sizeof(savedView));
    memcpy(savedVision, vision + 0x80, sizeof(savedVision));
    memcpy(savedBasis, reinterpret_cast<void*>(0x61049EC), sizeof(savedBasis));
    const auto savedPosition = PhotoGlobal<PhotoVec>(0x6121F34);
    const auto savedEye = PhotoGlobal<PhotoVec>(0x6122008);
    const auto savedFov = PhotoGlobal<float>(0x6121F04);
    auto* const savedCurrentView = PhotoGlobal<BYTE*>(CurrentPhotoView);
    const int savedMenu = PhotoGlobal<int>(G_MENU_STATE);
    const DWORD savedScriptVision = PhotoField<DWORD>(script, 0x18);
    const float savedTint = PhotoGlobal<float>(0x7C5D54);
    const float savedScaled = PhotoField<float>(game, 0x3D8);
    const float savedReal = PhotoField<float>(game, 0x3DC);
    const float savedRenderDelta = PhotoGlobal<float>(0x5E21498);
    const DWORD savedRenderTicks = PhotoGlobal<DWORD>(0x5E00E8C);

    // Only the world pass runs. The regular frame adds scripted text, menus,
    // cinematic overlays and focus fade after CGameView::Render.
    PhotoGlobal<int>(G_MENU_STATE) = 0;
    PhotoField<DWORD>(script, 0x18) = 0;
    PhotoField<DWORD>(view, 0x20C) = 0;
    PhotoGlobal<float>(0x7C5D54) = 0.5f;
    PhotoField<float>(game, 0x3D8) = PhotoField<float>(game, 0x3DC) = 0;
    PhotoGlobal<float>(0x5E21498) = 0;
    PhotoGlobal<DWORD>(0x5E00E8C) = 0;
    g_photoDrawing = true;
    __try {
        using Viewport = void (__thiscall*)(BYTE*, float, float, float, float);
        reinterpret_cast<Viewport>(0x413730)(view, 0.0f, 0.0f,
            float(PhotoGlobal<int>(0x7BF140)), float(PhotoGlobal<int>(0x7BF144)));
        reinterpret_cast<PhotoGameCall>(0x4FD620)(view);
        reinterpret_cast<void (__cdecl*)(float)>(0x56D360)(1.0f);
        reinterpret_cast<void (__cdecl*)()>(0x56BE20)();
    } __finally {
        g_photoDrawing = false;
        PhotoGlobal<int>(G_MENU_STATE) = savedMenu;
        PhotoField<DWORD>(script, 0x18) = savedScriptVision;
        PhotoGlobal<float>(0x7C5D54) = savedTint;
        PhotoField<float>(game, 0x3D8) = savedScaled;
        PhotoField<float>(game, 0x3DC) = savedReal;
        PhotoGlobal<float>(0x5E21498) = savedRenderDelta;
        PhotoGlobal<DWORD>(0x5E00E8C) = savedRenderTicks;
        PhotoGlobal<BYTE*>(CurrentPhotoView) = savedCurrentView;
        PhotoGlobal<PhotoVec>(0x6121F34) = savedPosition;
        PhotoGlobal<PhotoVec>(0x6122008) = savedEye;
        PhotoGlobal<float>(0x6121F04) = savedFov;
        memcpy(reinterpret_cast<void*>(0x61049EC), savedBasis, sizeof(savedBasis));
        memcpy(vision + 0x80, savedVision, sizeof(savedVision));
        memcpy(view, savedView, sizeof(savedView));
    }
}

void __fastcall PhotoDrawFrame(BYTE* game, void*) {
    if (g_photoBlockingMenu && !g_photoExitReady) {
        DrawPhotoWorld(game);
        return;
    }
    if (g_photoBlockingMenu && g_photoFromGameplay) ResumePhotoGameplay(game);
    // The exit button has been released and the original camera can be drawn
    // before the next simulation tick.
    reinterpret_cast<PhotoGameCall>(0x4F8220)(game);
    g_photoBlockingMenu = false;
}

void __fastcall PhotoPausedClock(BYTE* game, void*, int frameLimit) {
    if (!g_photoBlockingMenu) {
        reinterpret_cast<void (__thiscall*)(BYTE*, int)>(0x4F4400)(game, frameLimit);
        return;
    }
    // Keep the native wall-clock baseline fresh, without updating the
    // per-power time interpolators or the world accumulator at 0x4F4539.
    const DWORD elapsed = GetTickCount() - g_photoTickAt;
    if (elapsed < 16) Sleep(16 - elapsed);
    PhotoField<DWORD>(game, 0x3D0) = reinterpret_cast<DWORD (__cdecl*)()>(0x68A0B0)();
    PhotoField<DWORD>(game, 0x3D4) = 0;
    PhotoField<float>(game, 0x3D8) = PhotoField<float>(game, 0x3DC) = 0;
    PhotoGlobal<DWORD>(0x5E00E8C) = 0;
    PhotoGlobal<float>(0x5E21498) = 0;
}

void AddPhotoMenuItem() {
    const int n = PhotoGlobal<int>(G_ITEM_COUNT);
    constexpr int itemSize = 0xD8;
    auto* items = reinterpret_cast<BYTE*>(G_ITEM_ARRAY);
    if (!g_photoReady || !PhotoCanEnter() || n < 1 ||
        PhotoField<DWORD>(items, 4) != 0x32) return;
    static const char* labels[] = {"Photo Mode", "Mode Photo", "Modalit\xE0 Foto",
        "Modo Foto", "Fotomodus", "Photo Mode", "\xD4\xEE\xF2\xEE\xF0\xE5\xE6\xE8\xEC"};
    AddMenuItem(labels[DetectLang() - 1], AC_PHOTO, nullptr);
    BYTE item[itemSize];
    memcpy(item, items + n * itemSize, itemSize);
    memmove(items + 2 * itemSize, items + itemSize, (n - 1) * itemSize);
    memcpy(items + itemSize, item, itemSize);
}

void RequestPhotoFromPause() {
    g_photoRequested = PhotoGlobal<int>(G_MENU_STATE) == MENU_PAUSE && PhotoCanEnter();
}

} // namespace

static void PatchCallChecked(DWORD site, const char* expected, int length, void* target);

static void InstallPhotoMode() {
    const struct { DWORD site; const char* original; void* hook; } patches[] = {
        {0x4F910D, "\xE8\x2E\xB6\xFF\xFF", PhotoBeforeSimulation},
        {0x4F911C, "\xE8\xAF\xFB\xFF\xFF", PhotoUpdateWorld},
        {0x4F9155, "\xE8\xC6\xF0\xFF\xFF", PhotoDrawFrame},
        {0x4F926E, "\xE8\x8D\xB1\xFF\xFF", PhotoPausedClock},
        {0x4F927A, "\xE8\x81\xB1\xFF\xFF", PhotoPausedClock},
        {0x4FD7A5, "\xE8\x66\x42\x0E\x00", PhotoSubmitCamera},
        {0x4FD921, "\xE8\x1A\x74\x09\x00", PhotoViewHud},
        {0x5E6520, "\xE8\x1B\x49\xF2\xFF", PhotoPostProcess},
    };
    for (const auto& p : patches) {
        if (memcmp(reinterpret_cast<const void*>(p.site), p.original, 5)) {
            Trace("PHOTO incompatible call site %08lX", p.site);
            return;
        }
    }
    for (const auto& p : patches) {
        PatchCallChecked(p.site, p.original, 5, p.hook);
        if (PhotoGlobal<BYTE>(p.site) != 0xE8 ||
            PhotoGlobal<DWORD>(p.site + 1) + p.site + 5 != reinterpret_cast<DWORD>(p.hook))
            return;
    }
    g_photoReady = true;
}
