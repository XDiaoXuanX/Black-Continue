// Hooks for default.xbe
#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <thread>

#include "app/app.h"
#include "common/filelog.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/input.h"
#include "game/layout.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace d3d {
void ViewScale(float xy);
void GunDraw(bool on, const float move[4]);
void GunScale(float& sx, float& sy);  // d3d8.cpp: the projection's scale the gun was last drawn with
}

namespace game {
void EspCamera();  // esp.cpp
}

namespace touch {
bool Firing();  // touch.cpp: a fire button of the touch controls, or a controller's right trigger, is held
}

namespace {

constexpr uint32_t kDevice = SYM(D3D_Device);
constexpr uint32_t kInitDefaultState = 0x271710;

void WriteFloat(uint32_t va, float f) { std::memcpy(HostPtr(va), &f, 4); }

// what the library's vertical blank interrupt does that the game sees: count, event, callback
void VerticalBlank(X86Context& ctx) {
    uint32_t info = mem::AllocSystem(12);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        // the film decoder paces on the count: it stands still through a focus pause
        while (game::g_focus_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next = std::chrono::steady_clock::now();
        }
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        uint32_t count = R32(kDevice + XDK_CDevice_VBlankCount) + 1;
        W32(kDevice + XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = R32(kDevice + XDK_CDevice_VBlankCallback)) {
            W32(info, count);
            W32(info + 4, R32(kDevice + XDK_CDevice_SwapCount));
            W32(info + 8, 0);
            CallGuest(ctx, callback, {info});
        }
        // the game clears the event itself before each wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

// the viewport as D3DDevice_SetViewport stores it, for the game's own readers
void StoreViewport(X86Context&, uint8_t*) {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    const gpu::Viewport& v = d3d::State().viewport;
    uint32_t p = kDevice + XDK_CDevice_Viewport;
    W32(p, v.x);
    W32(p + 4, v.y);
    W32(p + 8, v.w);
    W32(p + 12, v.h);
    WriteFloat(p + 16, v.min_z);
    WriteFloat(p + 20, v.max_z);
}

// D3DDevice_GetViewportOffsetAndScale
X86_EXTERN_FUNC(__imp__sub_0026E9C0);
X86_FUNC(sub_0026E9C0) {
    StoreViewport(ctx, base);
    __imp__sub_0026E9C0(ctx, base);
}

// device fields the library's init sets and the game or its inlined library code reads, then the default states
void AfterCreateDevice(X86Context& ctx, uint8_t* base) {
    if (ctx.eax) return;
    W32(kDevice + XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
    W32(kDevice + XDK_CDevice_AutoDepth, R32(kDevice + XDK_CDevice_Depth));
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale, 1.0f);
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale + 4, 1.0f);
    W32(kDevice + XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));  // 60 Hz, widescreen
    W32(kDevice + XDK_CDevice_VBlankEvent, 0x00040000);  // notification event
    W32(kDevice + XDK_CDevice_VBlankEvent + 4, 1);
    StoreViewport(ctx, base);
    uint32_t ecx = ctx.ecx;
    uint32_t edx = ctx.edx;
    CallGuest(ctx, kInitDefaultState, {});
    ctx.eax = 0;
    ctx.ecx = ecx;
    ctx.edx = edx;
    static bool started = false;
    if (!started) StartHostThread("vertical blank", VerticalBlank);
    started = true;
}

constexpr uint32_t kFrameCount = 0x2F4368;

// the game speed cheat: the game's steps take this share of the real time
static float Speed() { return float(std::clamp(game::Opt().game_speed, 25u, 200u)) / 100.0f; }

// seconds the last frame took, the frame time the game was made for at most, at the game speed
static float FrameTime() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static uint32_t frame;
    static float dt = G<float>(kGameFrameTime);
    if (uint32_t f = R32(kFrameCount); f != frame) {
        auto now = clock::now();
        dt = std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime));
        frame = f;
        last = now;
    }
    return dt * Speed();
}

// The game's timers step one fixed frame time a frame: they step the real one, so any frame rate plays at speed
X86_EXTERN_FUNC(__imp__sub_000BE3F0);
X86_FUNC(sub_000BE3F0) {
    uint32_t t = ctx.ecx;
    auto steps = [t] {
        double parts = double(R32(t + kTimerPartSteps)) / std::max(R32(t + kTimerDivisor), 1u);
        return R32(t + kTimerWholeSteps) + parts;
    };
    double before = steps();
    float time = G<float>(t + kTimerTime);
    __imp__sub_000BE3F0(ctx, base);
    float dt = FrameTime();
    G<float>(t + kTimerDt) = dt / std::max(R32(t + kTimerDivisor), 1u);
    G<float>(t + kTimerTime) = time + float((steps() - before) * dt);
}

// frame throttle: sleeps that gave the console's one CPU to the loader
X86_FUNC(sub_001F9130) { Return(ctx, ctx.eax, 1); }

// The Flash menus and HUD tick in milliseconds, 33 a call: the real ones, the part of one carried over
void WeaponsTick(X86Context& ctx);  // weapons.cpp: the weapons page's requests
void MenuTick(X86Context& ctx, uint8_t*) {
    WeaponsTick(ctx);
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static float carry;
    auto now = clock::now();
    carry += std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime)) * 1000.0f;
    last = now;
    ctx.eax = uint32_t(carry);
    carry -= float(ctx.eax);
}

// what a step made once a frame at 30 becomes over the real frame: factor to the power of this
static float FrameShare() { return FrameTime() / G<float>(kGameFrameTime); }

static uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

// weapon and its data
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponOwner = 0xF0;
constexpr uint32_t kDataAimZoom = 0xB4;
// player
// the view's axes: right, up, forward and eye look like the rows of one matrix at 0xD0 (forward and eye are known,
// right and up are read from the rows before them and checked at run time)
constexpr uint32_t kPlayerRight = 0xD0;
constexpr uint32_t kPlayerUp = 0xE0;
constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerEye = 0x100;
constexpr uint32_t kPlayerWeapon = 0x29C;
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

// The zoom of the aim grows or shrinks by a fixed share a frame
void ZoomStep(X86Context& ctx, uint8_t*) {
    float old = G<float>(R32(ctx.ecx + kWeaponOwner) + kPlayerZoom);
    float now = ctx.xmm[0].f[0];
    if (now == old || old <= 0) return;
    bool out = !G<uint8_t>(ctx.ecx + 0x105) && old > 1;  // back towards 1
    ctx.xmm[0].f[0] = out ? 1 + (old - 1) * std::pow((now - 1) / (old - 1), FrameShare())
                          : old * std::pow(now / old, FrameShare());
}

constexpr float kHalf = 3.14159265f / 360.0f;

// tangent of a setting's half angle over that of the game's 70 degrees
static float Widen(uint32_t degrees) {
    return std::tan(std::clamp(degrees, 45u, 140u) * kHalf) / std::tan(70 * kHalf);
}

static float g_gun = 1.0f;  // the gun's picture over the view's

// ---- log (ads_test.log, the run before in ads_test_prev.log): at most 300 lines a run. Only writes, changes nothing ----
static long DbgMs() {
    static const auto start = std::chrono::steady_clock::now();
    return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
static void DbgLog(const char* fmt, ...) {
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    const char* path = "/storage/emulated/0/Black/SaveData/ads_test.log";
    static bool first = true;
    static int lines = 0;
    if (first) {  // every run starts a fresh file; the run before is kept as ads_test_prev.log
        first = false;
        std::rename(path, "/storage/emulated/0/Black/SaveData/ads_test_prev.log");
    }
    if (lines >= 300) return;  // a runaway line cannot fill the storage
    lines++;
    FILE* log = std::fopen(path, "a");
    if (!log) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(log, fmt, args);
    va_end(args);
    std::fclose(log);
}

struct AdsMove {
    float v[10] = {0, 0, 0, 0, 0, 1, 0, 1, 1, -1};
};
static std::map<std::string, AdsMove> g_ads_shifts;
static time_t g_ads_stamp = -1;
static std::chrono::steady_clock::time_point g_ads_checked;
static float g_vm[5] = {};
static float g_vm_blend = 0;
static long g_vm_ms = -1000000;
static float g_shell = 0;
static float g_cam = 0;
static float g_shift[4] = {};
static float g_effect = 0;

static const char* const kTableNames[] = {"PST", "SHG", "SNR", "SMG", "ASR", "AK1", "RPG", "GRL", "SM3",
                                          "P90", "HVY", "MGN", "M16", "RM1", "GK1", "MP1", "BNS"};
constexpr uint32_t kTableBase = 0x8227D330;
constexpr uint32_t kTableStride = 0x1E0;
constexpr uint32_t kTableCount = sizeof(kTableNames) / sizeof(kTableNames[0]);

static const char* TableName(uint32_t data) {
    if (data < kTableBase || (data - kTableBase) % kTableStride) return nullptr;
    uint32_t index = (data - kTableBase) / kTableStride;
    return index < kTableCount ? kTableNames[index] : nullptr;
}

static int g_ads_src = -1;

static void LogAdsLoad(uint32_t data, const char* type, int src) {
    static bool tagged = false;
    if (!tagged) {
        tagged = true;
        DbgLog("build: dbg21 (clean: menu AIM VIEW / NO WEAPON KICK / ADS MODE, steady aim; kick held on every lap of the clip)\n");
    }
    DbgLog("aim in: data=%08X type=%s, %u lines, source=%s\n", unsigned(data), type, unsigned(g_ads_shifts.size()),
           src ? "ADS_Custom.txt" : "built in");
}

struct AdsDefaultRow { const char* name; float v[9]; };
static const AdsDefaultRow kAdsDefaults[] = {
    {"ALL", {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 1, 1}},
    {"PST", {0.0800f, 0.065f, 0.290f, -0.40f, 2.0f, 1, 0, 1, 1}},
    {"SMG", {0.117f, 0.032f, -0.021f, 1.00f, 2.9f, 1, 0, 1, 0}},
    {"ASR", {0.078f, 0.020f, 0.035f, -0.38f, 0.56f, 1, 0, 1, 0}},
    {"HVY", {0.060f, -0.001f, 0.2f, -3.50f, 1.5f, 1, 0, 1, 0}},
    {"GK1", {0.0800f, 0.060f, 0.1f, -0.30f, -0.12f, 1, 0, 1, 1}},
    {"MGN", {0.0750f, 0.030f, 0.15f, -5.0f, 1.0f, 1, 0, 1, 1}},
    {"SM3", {0.125f, 0.002f, -0.01f, -1.00f, -1.0f, 1, 0, 1, 0}},
    {"P90", {0.0810f, -0.031f, -0.080f, -3.80f, -2.0f, 1, 0, 1, 0}},
    {"MP1", {0.0458f, 0.00f, -0.065f, -2.50f, -2.5f, 1, 0, 1, 0}},
    {"AK1", {0.101f, 0.029f, -0.010f, 3.60f, 0.50f, 1, 0, 1, 0}},
    {"M16", {0.0738f, 0.015f, 0.180f, 0.28f, 5.0f, 1, 0, 1, 0}},
    {"BNS", {0.114f, -0.010f, 0.043f, 0.28f, 1.30f, 1, 0, 1, 0}},
    {"GRL", {0.119f, 0.035f, 0.035f, -3.95f, -4.0f, 1, 0, 1, 1}},
    {"SHG", {0.072f, 0.030f, 0.1f, -2.00f, 4.0f, 1, 0, 1, 1}},
    {"RM1", {0.053f, 0.023f, 0.1f, -2.00f, 1.0f, 1, 0, 1, 1}},
    {"RPG", {0, 0, 0, 0, 0, 0, 0, 1, 1}},
};

static void LoadAdsDefaults(uint32_t data, const char* type) {
    g_ads_shifts.clear();
    for (const AdsDefaultRow& r : kAdsDefaults) {
        AdsMove m;
        for (int i = 0; i < 9; i++) m.v[i] = r.v[i];
        m.v[9] = 0;
        g_ads_shifts[r.name] = m;
    }
    LogAdsLoad(data, type, 0);
}

static const char* const kAdsCustomPath = "/storage/emulated/0/Black/SaveData/ADS_Custom.txt";
static void WriteAdsCustomTemplate() {
    FILE* w = std::fopen(kAdsCustomPath, "w");
    if (!w) return;
    std::fputs(
        "#==================== ADS_Custom.txt: ADS MODE = CUSTOM ====================\n"
        "# Tiap baris: SENJATA lalu 7 angka, dipisah tanda | (boleh juga spasi saja). Awalan # = catatan, tidak dibaca.\n"
        "# Semua nilai awal 0. Simpan file, tunggu 1-2 detik, hasil langsung terlihat (tidak perlu buka ulang game).\n"
        "#\n"
        "# KOLOM (kiri ke kanan):\n"
        "#  dx     : geser senjata KIRI (+) / KANAN (-)\n"
        "#  dy     : geser senjata NAIK (+) / TURUN (-)\n"
        "#  dz     : geser senjata MENJAUH dari mata (+) / MENDEKAT (-)\n"
        "#  yaw    : putar moncong ke KIRI (+) / KANAN (-), derajat\n"
        "#  pitch  : putar moncong NAIK (+) / TURUN (-), derajat\n"
        "#  shell  : selongsong ikut bergeser: 1 ikut, 0 tidak, di antaranya sebagian\n"
        "#  cam    : kamera tetap tidak zoom: 1 tetap, 0 zoom bawaan game, di antaranya sebagian\n"
        "# Langkah geser kira-kira 0.01 sampai 0.05. Kalau 0 semua, senjata tidak bergeser.\n"
        "# Baris yang tidak ada = senjata itu bawaan game. Baris ALL berlaku untuk senjata tanpa baris sendiri.\n"
        "#\n"
        "#------+--------+--------+--------+--------+--------+--------+--------+---------------------\n"
        "# KEY  |   dx   |   dy   |   dz   |  yaw   | pitch  | shell  |  cam   | Nama senjata\n"
        "#------+--------+--------+--------+--------+--------+--------+--------+---------------------\n"
        "PST    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # PISTOL\n"
        "GK1    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Glock\n"
        "MGN    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Magnum\n"
        "SMG    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # UZI\n"
        "SM3    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Mac-10\n"
        "MP1    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # MP5\n"
        "P90    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # P90\n"
        "ASR    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # G36C\n"
        "AK1    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # AKM\n"
        "M16    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # M16\n"
        "BNS    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # M16A2 + M203\n"
        "HVY    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # M249\n"
        "GRL    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Grenade Launcher\n"
        "SHG    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Spas 12 (bidik lewat tombol AIM)\n"
        "RM1    |   0    |   0    |   0    |   0    |   0    |   0    |   0    | # Remington (bidik lewat tombol AIM)\n"
        "#------+--------+--------+--------+--------+--------+--------+--------+---------------------\n",
        w);
    std::fclose(w);
}

static void LoadAdsShifts(uint32_t data, const char* type) {
    g_ads_shifts.clear();
    std::ifstream f(kAdsCustomPath);
    if (!f) {
        WriteAdsCustomTemplate();
        LogAdsLoad(data, type, 1);
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        for (char& c : line) if (c == '|') c = ' ';
        std::istringstream in(line);
        std::string name;
        AdsMove m;
        if (!(in >> name >> m.v[0] >> m.v[1])) continue;
        for (int i = 2; i < 7; i++) {
            float t;
            if (!(in >> t)) break;
            m.v[i] = t;
        }
        g_ads_shifts[name] = m;
    }
    LogAdsLoad(data, type, 1);
}

static const AdsMove* AdsRow(uint32_t data) {
    if (!data) return nullptr;
    const game::Options& opt = game::Opt();
    if (opt.aim_view == 0) return nullptr;
    char type[4] = {};
    std::memcpy(type, HostPtr(data + 0x130), 3);
    int src = opt.ads_custom ? 1 : 0;
    auto now = std::chrono::steady_clock::now();
    if (src != g_ads_src) {
        g_ads_src = src;
        g_ads_stamp = -1;
        g_ads_checked = now - std::chrono::seconds(1);
    }
    if (g_ads_stamp == -1 || now - g_ads_checked > std::chrono::milliseconds(500)) {
        g_ads_checked = now;
        if (src == 0) {
            if (g_ads_stamp == -1) {
                g_ads_stamp = 0;
                LoadAdsDefaults(data, type);
            }
        } else {
            struct stat st;
            time_t stamp = stat(kAdsCustomPath, &st) == 0 ? st.st_mtime : 0;
            if (stamp != g_ads_stamp) {
                g_ads_stamp = stamp;
                LoadAdsShifts(data, type);
            }
        }
    }
    const char* name = TableName(data);
    const char* used = name;
    auto it = name ? g_ads_shifts.find(name) : g_ads_shifts.end();
    if (it == g_ads_shifts.end()) {
        used = type;
        it = g_ads_shifts.find(type);
    }
    if (it == g_ads_shifts.end()) {
        used = "ALL";
        it = g_ads_shifts.find("ALL");
    }
    if (it == g_ads_shifts.end()) used = "none";
    static uint32_t logged_data = 0;
    static std::string logged_used;
    if (data != logged_data || logged_used != used) {
        logged_data = data;
        logged_used = used;
        DbgLog("aim row: data=%08X name=%s class=%s -> row %s\n", unsigned(data), name ? name : "?", type, used);
    }
    return it == g_ads_shifts.end() ? nullptr : &it->second;
}

static void AdsShift(uint32_t data, float hip) {
    float blend = 1 - hip;
    g_vm_blend = blend;
    g_vm_ms = DbgMs();
    for (float& v : g_vm) v = 0;
    g_shell = 0;
    g_cam = 0;
    if (!data || blend <= 0) return;
    const AdsMove* row = AdsRow(data);
    if (!row) return;
    for (int i = 0; i < 5; i++) g_vm[i] = row->v[i] * blend;
    g_shell = row->v[5];
    g_cam = row->v[6];
}

void ViewmodelShift(X86Context& ctx, uint8_t*) {
    if (ctx.ebx != Player()) return;
    bool moved = false;
    for (float v : g_vm) moved = moved || v != 0;
    if (!moved) return;
    float* m = &G<float>(ctx.ebx + 0x420);
    constexpr float kPi = 3.14159265f;
    constexpr float kPivot = 0.6f;
    float yaw = g_vm[3] * kPi / 180, pitch = g_vm[4] * kPi / 180;
    if (yaw != 0 || pitch != 0) {
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        for (int r = 0; r < 4; r++) {
            float* v = m + r * 4;
            if (r == 3) v[2] -= kPivot;
            float x = v[0] * cy + v[2] * sy;
            float z = -v[0] * sy + v[2] * cy;
            float y = v[1] * cp + z * sp;
            z = -v[1] * sp + z * cp;
            v[0] = x;
            v[1] = y;
            v[2] = z;
            if (r == 3) v[2] += kPivot;
        }
    }
    m[12] += g_vm[0];
    m[13] += g_vm[1];
    m[14] += g_vm[2];
}

void ShellShift(X86Context& ctx, uint8_t*) {
    uint32_t player = R32(ctx.ebp + 8);
    if (player != Player() || g_shell == 0 || (!g_vm[0] && !g_vm[1] && !g_vm[2] && !g_vm[3] && !g_vm[4])) return;
    float* pos = &G<float>(ctx.ebx + 0x40);
    constexpr float kPi = 3.14159265f;
    constexpr float kPivot = 0.6f;
    float yaw = g_vm[3] * g_shell * kPi / 180, pitch = g_vm[4] * g_shell * kPi / 180;
    if (yaw != 0 || pitch != 0) {
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        float z0 = pos[2] - kPivot;
        float x = pos[0] * cy + z0 * sy;
        float z = -pos[0] * sy + z0 * cy;
        float y = pos[1] * cp + z * sp;
        z = -pos[1] * sp + z * cp;
        pos[0] = x;
        pos[1] = y;
        pos[2] = z + kPivot;
    }
    for (int i = 0; i < 3; i++) pos[i] += g_shell * g_vm[i];
}

static bool GunMoved() {
    return g_gun != 1 || (g_effect != 0 && (g_shift[0] || g_shift[1] || g_shift[2] || g_shift[3]));
}

static uint32_t PadBits() {
    uint32_t bits = 0;
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        bits |= uint32_t(pad.buttons) & 0xFFFF;
        for (size_t i = 0; i < sizeof(pad.analog) / sizeof(pad.analog[0]) && i < 16; i++)
            if (pad.analog[i] > 30) bits |= 1u << (16 + i);
    }
    return bits;
}

// ---- fire input helpers (RPG / GRL / BNS M203 spam fire) ----

// primary fire held: left mouse button, or a touch fire button / a pad's right trigger (touch::Firing covers both)
static bool FireHeld() {
    return input::LeftButton() || touch::Firing();
}

// alternate fire held: a pad's left trigger (analog 5) or left bumper (analog 7 / button bit 8); the BNS M203
// under the barrel fires on this, like the grenade throw button of the game's controls
static bool AltFireHeld() {
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        if (pad.analog[5] > 30) return true;   // left trigger
        if (pad.analog[7] > 30) return true;   // left bumper analog
        if (pad.buttons & 0x0100) return true; // LB button
    }
    return false;
}

static uint32_t g_aim_bit = 0x80;
static const char* const kAimFile = "/storage/emulated/0/Black/SaveData/ads_aim.txt";

static void LearnAim(bool) {}

static bool AimHeld() { return (PadBits() & 0x80u) != 0; }

namespace input { bool AimPulseMode(); }
static bool g_own_prev = false, g_own_toggle = false;
static bool OwnAimHeld() {
    bool bit = AimHeld();
    if (!input::AimPulseMode()) {
        g_own_prev = bit;
        g_own_toggle = false;
        return bit;
    }
    if (bit && !g_own_prev) g_own_toggle = !g_own_toggle;
    g_own_prev = bit;
    return g_own_toggle;
}

static float g_base_fov = 0;
static float g_own_aim = 0;
static std::atomic<float> g_own_aim_pub{0};
constexpr float kOwnAimZoom = 1.15f;
namespace game {
bool OwnAiming() { return g_own_aim_pub.load() > 0.5f; }
}

void FieldOfView(X86Context& ctx, uint8_t*) {
    uint32_t data = R32(ctx.ecx + kWeaponData);
    bool mine = R32(ctx.ecx + kWeaponOwner) == Player();
    float aim = data ? G<float>(data + kDataAimZoom) : 1;
    float zoom = ctx.xmm[0].f[0];
    float& fov = ctx.xmm[1].f[0];
    float hip = aim > 1 ? std::clamp((aim - zoom) / (aim - 1), 0.0f, 1.0f) : 1.0f;
    const char* name = data ? TableName(data) : nullptr;
    bool own = mine && aim <= 1 && name && (!std::strcmp(name, "SHG") || !std::strcmp(name, "RM1"));
    if (own) {
        bool held = G<uint8_t>(ctx.ecx + 0x105) != 0 || OwnAimHeld();
        g_own_aim += ((held ? 1.0f : 0.0f) - g_own_aim) * (1 - std::pow(0.75f, FrameShare()));
        if (g_own_aim > 0.999f) g_own_aim = 1;
        if (g_own_aim < 0.001f) g_own_aim = 0;
        g_own_aim_pub = g_own_aim;
        hip = 1 - g_own_aim;
    } else if (mine) {
        if (aim > 1) LearnAim(G<uint8_t>(ctx.ecx + 0x105) != 0);
        g_own_aim = 0;
        g_own_aim_pub = 0;
        g_own_toggle = false;
        if (zoom <= 1.001f) g_base_fov = fov;
    }
    if (mine) AdsShift(data, hip);
    if (own) fov /= 1 + (kOwnAimZoom - 1) * g_own_aim;
    float keep = 0;
    if (mine && !own && g_cam != 0 && zoom > 1.001f) {
        keep = std::clamp(g_cam, 0.0f, 1.0f);
        float base = g_base_fov > 0 ? g_base_fov : fov * zoom;
        fov += (base - fov) * keep;
    }
    float shown = own ? 1.0f : hip + (1 - hip) * keep;
    float view = 1 + (Widen(game::Opt().fov) - 1) * shown;
    g_gun = view / (1 + (Widen(game::Opt().viewmodel_fov) - 1) * shown);
    if (view != 1) fov = std::atan(std::tan(fov * kHalf) * view) / kHalf;
}

X86_EXTERN_FUNC(__imp__sub_00176F60);
X86_FUNC(sub_00176F60) {
    if (ctx.esi == R32(kDisplay) + kDisplayDrawLists + kAlphaDrawList * kDrawListSize) game::EspCamera();
    bool list = ctx.esi == R32(kDisplay) + kDisplayDrawLists + kGunDrawList * kDrawListSize;
    bool gun = g_gun != 1 && list;
    if (list) d3d::GunDraw(true, g_shift);
    if (gun) d3d::ViewScale(g_gun);
    __imp__sub_00176F60(ctx, base);
    if (list) d3d::GunDraw(false, g_shift);
    if (gun) d3d::ViewScale(1);
}

static void AdsOffset(float* v, float depth) {
    if (!g_effect || (!g_shift[0] && !g_shift[1] && !g_shift[2] && !g_shift[3])) return;
    const float* right = &G<float>(Player() + kPlayerRight);
    const float* up = &G<float>(Player() + kPlayerUp);
    const float* forward = &G<float>(Player() + kPlayerForward);
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    bool axes = std::fabs(dot(right, right) - 1) < 0.02f && std::fabs(dot(up, up) - 1) < 0.02f &&
                std::fabs(dot(right, forward)) < 0.02f && std::fabs(dot(up, forward)) < 0.02f &&
                std::fabs(dot(right, up)) < 0.02f;
    if (!axes) return;
    float sx = 0, sy = 0;
    d3d::GunScale(sx, sy);
    float dx = g_shift[0] + (sx > 0 ? g_shift[2] * depth / sx : 0);
    float dy = g_shift[1] + (sy > 0 ? g_shift[3] * depth / sy : 0);
    for (int i = 0; i < 3; i++) v[i] += g_effect * (dx * right[i] + dy * up[i]);
}

static void OnGun(uint32_t at, bool point) {
    float* v = &G<float>(at);
    const float* forward = &G<float>(Player() + kPlayerForward);
    const float* eye = &G<float>(Player() + kPlayerEye);
    float d[3];
    float depth = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = v[i] - (point ? eye[i] : 0);
        depth += d[i] * forward[i];
    }
    for (int i = 0; i < 3; i++) v[i] += (g_gun - 1) * (d[i] - forward[i] * depth);
    if (point) AdsOffset(v, depth);
}

constexpr uint32_t kMatrixPosition = 48;

static uint32_t Follow(uint32_t matrix, uint32_t owner, bool axes) {
    if (!GunMoved() || matrix == 0x5655C0 || owner != Player()) return matrix;
    static uint32_t copy = mem::AllocSystem(64, 16);
    std::memcpy(HostPtr(copy), HostPtr(matrix), 64);
    for (uint32_t row = axes ? 0 : kMatrixPosition; row < 64; row += 16) OnGun(copy + row, row == kMatrixPosition);
    return copy;
}

constexpr uint32_t kEffectOwner = 0x44;

void FlashSprite(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, Player(), true); }
void GunEffect(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, ctx.edi, false); }
void GunEffectStart(X86Context& ctx, uint8_t*) { ctx.edx = Follow(ctx.edx, R32(ctx.ebx + kEffectOwner), false); }

constexpr uint32_t kBulletShot = 0x70;
constexpr uint32_t kShotWeapon = 4;

void TracerStart(X86Context& ctx, uint8_t*) {
    uint32_t shot = R32(ctx.esi + kBulletShot);
    uint32_t weapon = shot ? R32(shot + kShotWeapon) : 0;
    if (!GunMoved() || !weapon || R32(weapon + kWeaponOwner) != Player()) return;
    static uint32_t copy = mem::AllocSystem(16, 16);
    std::memcpy(HostPtr(copy), HostPtr(ctx.eax), 16);
    ctx.eax = copy;
    OnGun(copy, true);
}

constexpr uint32_t kFromTracerA = 0x1DBAAB;
constexpr uint32_t kFromTracerB = 0x1DC4D4;

X86_EXTERN_FUNC(__imp__sub_001D2860);
X86_FUNC(sub_001D2860) {
    uint32_t from = R32(ctx.esp);
    uint32_t out = R32(ctx.esp + 4);
    __imp__sub_001D2860(ctx, base);
    if (GunMoved() && (from == kFromTracerA || from == kFromTracerB)) OnGun(out + kMatrixPosition, true);
}

void RecoilDecay(X86Context& ctx, uint8_t*) {
    float kick = 1;
    if (ctx.ebx == Player()) {
        uint32_t weapon = R32(ctx.ebx + kPlayerWeapon);
        if (const AdsMove* row = weapon ? AdsRow(R32(weapon + kWeaponData)) : nullptr) kick = std::clamp(row->v[7], 0.0f, 1.0f);
    }
    bool none = game::Opt().no_recoil && ctx.ebx == Player();
    ctx.xmm[0].f[0] = none ? 0.0f : std::pow(ctx.xmm[0].f[0], FrameShare()) * kick;
}

constexpr uint32_t kDataSwayShares = 0x104;

static uint32_t g_sway;
static float g_sway_share[2];
void SwayBegin(X86Context& ctx, uint8_t*) {
    g_sway = ctx.eax + kDataSwayShares;
    for (int i = 0; i < 2; i++) {
        float& k = G<float>(g_sway + i * 4);
        g_sway_share[i] = k;
        k = 1 - std::pow(1 - k, FrameShare());
    }
}
void SwayEnd(X86Context&, uint8_t*) {
    if (!g_sway) return;
    for (int i = 0; i < 2; i++) G<float>(g_sway + i * 4) = g_sway_share[i];
    g_sway = 0;
}

static float Dt() { return G<float>(R32(kWorld) + kTimerDt); }

static float WholeFrames(float wait, float rest) {
    float step = G<float>(kGameFrameTime) / std::max(R32(R32(kWorld) + kTimerDivisor), 1u);
    if (!(Dt() < step)) return wait;
    int frames = 0;
    for (float left = wait; left > 0 && frames < 4096; left -= step) frames++;
    return std::max(frames, 1) * step + rest;
}

static bool g_idle_early;

// weapon
constexpr uint32_t kWeaponState = 0xD8;
constexpr uint32_t kWeaponWait = 0xDC;
constexpr uint32_t kWeaponAmmo = 0xF4;
constexpr uint32_t kWeaponAltAmmo = 0xF8;
constexpr uint32_t kAmmoTotal = 0x14;
constexpr uint32_t kAmmoClip = 0x18;

struct KeptAmmo {
    uint32_t ammo = 0;
    uint32_t total = 0;
    uint16_t clip = 0;
};

static KeptAmmo KeepAmmo(uint32_t weapon) {
    uint32_t ammo = R32(weapon + kWeaponAmmo);
    if (!game::Opt().infinite_ammo || !ammo || R32(weapon + kWeaponOwner) != Player()) return {};
    return {ammo, R32(ammo + kAmmoTotal), G<uint16_t>(ammo + kAmmoClip)};
}

static uint32_t RoundsOf(uint32_t weapon, uint32_t at) {
    uint32_t ammo = R32(weapon + at);
    return ammo ? R32(ammo + kAmmoTotal) : 0;
}

static uint32_t Rounds(uint32_t weapon) {
    return RoundsOf(weapon, kWeaponAmmo) + RoundsOf(weapon, kWeaponAltAmmo);
}

static void RestoreAmmo(uint32_t weapon, const KeptAmmo& kept) {
    if (!kept.ammo || R32(weapon + kWeaponAmmo) != kept.ammo) return;
    if (R32(kept.ammo + kAmmoTotal) < kept.total) W32(kept.ammo + kAmmoTotal, kept.total);
    if (G<uint16_t>(kept.ammo + kAmmoClip) < kept.clip) G<uint16_t>(kept.ammo + kAmmoClip) = kept.clip;
}

// diagnosis for NO SPREAD and for a launcher that fires again at once, only writes: weapon_dump.log has each kind of
// weapon the player holds once, the words of the weapon, of its data and of what their pointers lead to, to tell the
// weapons apart (a sniper's spread is near nothing, a shotgun's is wide); weapon_trace.log has the first shots, frame
// by frame, with the weapon's state, wait and rounds (try it with INFINITE AMMO off)
// the 64 MB of memory is at the bottom of the address space and again from 0x80000000 (the weapon table is there)
static bool GuestPointer(uint32_t v) { return !(v & 3) && ((v >= 0x10000 && v < 0x04000000) || (v >= 0x80000000 && v < 0x84000000)); }

static void DumpWords(const char* what, uint32_t at, uint32_t bytes) {
    for (uint32_t off = 0; off < bytes; off += 4) {
        uint32_t v = R32(at + off);
        if (!v) continue;
        float f;
        std::memcpy(&f, &v, 4);
        if (std::fabs(f) >= 1e-4f && std::fabs(f) < 1e6f)
            filelog::Write("weapon_dump", "%s +0x%03X %08X %.4f", what, off, v, f);
        else
            filelog::Write("weapon_dump", "%s +0x%03X %08X", what, off, v);
    }
}

static void DumpWeapon(uint32_t weapon) {
    static std::set<uint32_t> done;  // by the weapon's data: each kind once
    uint32_t data = R32(weapon + kWeaponData);
    if (!GuestPointer(data) || !done.insert(data).second) return;
    const char* name = TableName(data);
    filelog::Write("weapon_dump", "==== %s weapon %08X data %08X", name ? name : "?", weapon, data);
    DumpWords("weapon", weapon, 0x180);
    DumpWords("data", data, kTableStride);
    // one step into what they point at
    int followed = 0;
    for (uint32_t from : {weapon, data})
        for (uint32_t off = 0; off < (from == weapon ? 0x180u : kTableStride) && followed < 16; off += 4) {
            uint32_t v = R32(from + off);
            if (!GuestPointer(v) || v == weapon || v == data) continue;
            char tag[48];
            std::snprintf(tag, sizeof(tag), "ptr %08X from +0x%X", v, off);
            DumpWords(tag, v, 0x80);
            followed++;
        }
}

static void TraceShot(uint32_t weapon, bool shot) {
    static int shots = 0, left = 0;
    if (shot && shots < 12) {
        shots++;
        left = 90;
        const char* name = TableName(R32(weapon + kWeaponData));
        filelog::Write("weapon_trace", "---- shot %d %s", shots, name ? name : "?");
    }
    if (left <= 0) return;
    left--;
    uint32_t ammo = R32(weapon + kWeaponAmmo);
    filelog::Write("weapon_trace", "t%02d state %u wait %.4f clip %u total %u rounds %u", 90 - left, R32(weapon + kWeaponState),
                   G<float>(weapon + kWeaponWait), ammo ? unsigned(G<uint16_t>(ammo + kAmmoClip)) : 0u,
                   ammo ? R32(ammo + kAmmoTotal) : 0u, Rounds(weapon));
}

// the wait between a weapon's shots. The frame of a first shot is part of it: the fire clip runs that frame too
// and holds its next shot where the wait ends
X86_EXTERN_FUNC(__imp__sub_001EEFD0);
X86_FUNC(sub_001EEFD0) {
    uint32_t weapon = R32(ctx.esp + 4);
    float& wait = G<float>(weapon + kWeaponWait);
    float rest = wait;
    float dt = Dt();
    KeptAmmo kept = KeepAmmo(weapon);
    uint32_t main_before = RoundsOf(weapon, kWeaponAmmo);
    uint32_t alt_before  = RoundsOf(weapon, kWeaponAltAmmo);
    uint32_t rounds = main_before + alt_before;

    // ---- BEFORE original: keep the fire request alive for RPG / GRL while fire is held (spam fire without zoom) ----
    // The RPG and GRL are pure launchers: primary fire is the launcher itself. Set kPlayerFireRequest to 1 every frame
    // the fire button is held, and set the fast-forward request so the fire animation never blocks. This is what makes
    // the launcher fire continuously without the player releasing and pressing again
    {
        uint32_t me_pre = R32(weapon + kWeaponOwner);
        uint32_t data_pre = R32(weapon + kWeaponData);
        const char* name_pre = data_pre ? TableName(data_pre) : nullptr;
        uint32_t rfe_pre = std::clamp(game::Opt().rapid_fire_extra, 1u, 999u);
        if (me_pre == Player() && rfe_pre > 1 && name_pre) {
            bool pure_launcher = !std::strcmp(name_pre, "RPG") || !std::strcmp(name_pre, "GRL");
            bool bns = !std::strcmp(name_pre, "BNS");
            if (pure_launcher && FireHeld()) {
                W32(me_pre + kPlayerFireRequest, 1);
                game::g_launcher_skip_request = true;
            } else if (bns && (AltFireHeld() || FireHeld())) {
                // BNS: alt fire (LT/LB) is the M203 launcher, primary is the M16 rifle. When either is held, keep the
                // fire request alive and the fast-forward going: the M16 fires at RAPID FIRE speed (the fast-forward
                // is harmless there since the fire clip is short), the M203 fires at RAPID FIRE EXTRA speed (the
                // fast-forward is what lets it fire without reload animation delay)
                W32(me_pre + kPlayerFireRequest, 1);
                game::g_launcher_skip_request = true;
            }
        }
    }

    __imp__sub_001EEFD0(ctx, base);

    uint32_t main_after = RoundsOf(weapon, kWeaponAmmo);
    uint32_t alt_after  = RoundsOf(weapon, kWeaponAltAmmo);
    bool shot     = (main_after + alt_after) < rounds;
    bool alt_shot = alt_after < alt_before;  // the launcher under the barrel fired (M203)
    RestoreAmmo(weapon, kept);
    uint32_t me = R32(weapon + kWeaponOwner);
    // the rapid fire cheat: a shot's wait, its fire mode's +0x20, is that many times shorter
    uint32_t faster = std::clamp(game::Opt().rapid_fire, 1u, 10u);
    if (shot && me == Player() && wait > 0) wait /= float(faster);
    if (me == Player()) {
        DumpWeapon(weapon);
        TraceShot(weapon, shot);
        if (R32(weapon + kWeaponState) != 1 || wait != rest) g_idle_early = false;
        else if (g_idle_early) W32(me + kPlayerFireRequest, 0);
    }
    if (wait == rest) return;
    if (instant && shot && me == Player()) {
        wait = 0.0001f;
    } else {
        wait = WholeFrames(wait, rest <= 0 && rest > -dt ? rest : -dt);
    }
}

// animation controller
constexpr uint32_t kAnimOwner = 0;
constexpr uint32_t kAnimClip = 0x54;
constexpr uint32_t kAnimName = 0x5C;
constexpr uint32_t kAnimFrom = 0x90;
constexpr uint32_t kAnimClipHash = 0x18;
constexpr uint32_t kAnimClipFades = 0x88;
constexpr uint32_t kFadeSize = 0x28;
constexpr uint32_t kAnimClipFadeSlot = 0x1C8;
constexpr uint32_t kFireLoopHash = 0x6D9D2AC0;

static bool IsFireClip(const std::string& name) {
    static const char* fire[] = {"S_100", "S_110", "S_114", "S_130", "S_170", "S_176"};
    bool fired = false;
    for (auto f : fire) fired |= name.size() >= 5 && name.compare(name.size() - 5, 5, f) == 0;
    return fired;
}

static constexpr float kNoKickCap = 1.0f;
static float RecoilCap() {
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon) return 0;
    const char* name = TableName(R32(weapon + kWeaponData));
    if (!name) return 0;
    static const char* const kKeepKick[] = {"GRL", "RPG", "SHG", "RM1", "PST", "GK1", "MGN"};
    for (const char* k : kKeepKick)
        if (!std::strcmp(name, k)) return 0;
    if (!game::Opt().no_kick) return 0;
    return kNoKickCap;
}

static bool AimingNow() {
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon) return false;
    uint32_t state = R32(weapon + kWeaponState);
    bool moved = false;
    for (float v : g_vm) moved = moved || v != 0;
    moved = moved && g_vm_blend > 0.5f && DbgMs() - g_vm_ms < 150;
    return (state == 0 && g_own_aim_pub.load() > 0.5f) || state == 31 || moved;
}

static bool IdleHoldNow() {
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon || !AimingNow()) return false;
    return game::Opt().steady_aim;
}

static constexpr float kAimPin = 30;

static uint32_t g_kick_ac = 0;
static bool g_fetch_active = false;
static uint32_t g_fetch_clip = 0;
static float g_fetch_real = 0, g_fetch_cap = 0, g_fetch_len = 0;

X86_EXTERN_FUNC(__imp__sub_00098450);
X86_FUNC(sub_00098450) {
    if (!g_fetch_active) return __imp__sub_00098450(ctx, base);
    uint32_t esp0 = ctx.esp;
    if (R32(esp0 + 4) != g_fetch_clip) return __imp__sub_00098450(ctx, base);
    float cur = G<float>(esp0 + 0x10);
    bool own = std::fabs(cur - g_fetch_real) < 0.51f;
    if (!own && g_fetch_len > 1.0f) {
        float laps = std::floor((g_fetch_real - cur) / g_fetch_len + 0.5f);
        own = laps >= 1 && std::fabs(g_fetch_real - cur - laps * g_fetch_len) < 0.51f;
    }
    bool swap = g_fetch_cap > 0 && own && cur > g_fetch_cap;
    if (swap) G<float>(esp0 + 0x10) = g_fetch_cap;
    __imp__sub_00098450(ctx, base);
    if (swap) G<float>(esp0 + 0x10) = cur;
}

X86_EXTERN_FUNC(__imp__sub_00097E00);
X86_FUNC(sub_00097E00) {
    uint32_t clip = R32(ctx.esp + 4);
    uint32_t ac = g_kick_ac;
    bool mine = clip && ac && R32(ac + kAnimClip) == clip && R32(ac + kAnimOwner) == Player();
    bool fire = mine && IsFireClip(ReadCString(ac + kAnimName, 13));
    float cap = fire ? RecoilCap() : 0.0f;
    if (cap <= 0) return __imp__sub_00097E00(ctx, base);
    g_fetch_active = true;
    g_fetch_clip = clip;
    g_fetch_real = G<float>(clip + 0x14);
    g_fetch_len = G<float>(clip + 0x1C);
    g_fetch_cap = cap;
    __imp__sub_00097E00(ctx, base);
    g_fetch_active = false;
}

// A fire clip that ends before the wait does hands over to the idle early...
X86_EXTERN_FUNC(__imp__sub_0017B130);
X86_FUNC(sub_0017B130) {
    uint32_t ac = R32(ctx.esp + 4);
    uint32_t from = R32(ac + kAnimFrom);
    uint32_t clip = R32(ac + kAnimClip);
    if (R32(ac + kAnimOwner) != Player() || !clip) return __imp__sub_0017B130(ctx, base);
    int16_t slot = G<int16_t>(clip + kAnimClipFadeSlot);
    uint32_t fade = clip + kAnimClipFades + slot * kFadeSize;
    g_kick_ac = ac;
    // RAPID FIRE EXTRA: fast-forward the animation clip so the next action can start at once. Two independent tracks:
    //  - grenade throw (cheats.cpp's sub_001E8830 sets g_grenade_skip_request when the player throws one)
    //  - launcher fire (sub_001EEFD0 sets g_launcher_skip_request when RPG / GRL / BNS M203 fires, or while held)
    // Both capture the current non-idle clip (never the idle clip S_001 by mistake) and hold its time (t14 at +0x14)
    // at its length (+0x1C) so the game sees the animation as done and switches back to idle, allowing the next action
    if (game::Opt().rapid_fire_extra > 1) {
        // ---- grenade throw ----
        if (game::g_grenade_skip_request.exchange(false)) {
            std::string name = ReadCString(ac + kAnimName, 13);
            bool idle_name = name.size() >= 5 && name.compare(name.size() - 5, 5, "S_001") == 0;
            if (!idle_name) {
                game::g_grenade_skip_clip = clip;
                game::g_grenade_skip_frames = 10;
            }
        }
        int frames = game::g_grenade_skip_frames.load();
        if (frames > 0) {
            if (clip == game::g_grenade_skip_clip.load()) {
                float len = G<float>(clip + 0x1C);
                if (len > 0) G<float>(clip + 0x14) = len;
                game::g_grenade_skip_frames = frames - 1;
            } else {
                game::g_grenade_skip_frames = 0;
            }
        }
        // ---- launcher fire (RPG, GRL, BNS M203) ----
        if (game::g_launcher_skip_request.exchange(false)) {
            std::string name = ReadCString(ac + kAnimName, 13);
            bool idle_name = name.size() >= 5 && name.compare(name.size() - 5, 5, "S_001") == 0;
            if (!idle_name) {
                game::g_launcher_skip_clip = clip;
                game::g_launcher_skip_frames = 10;
            }
        }
        int lframes = game::g_launcher_skip_frames.load();
        if (lframes > 0) {
            if (clip == game::g_launcher_skip_clip.load()) {
                float len = G<float>(clip + 0x1C);
                if (len > 0) G<float>(clip + 0x14) = len;
                game::g_launcher_skip_frames = lframes - 1;
            } else {
                game::g_launcher_skip_frames = 0;
            }
        }
    }
    __imp__sub_0017B130(ctx, base);
    if (game::Opt().rapid_fire_extra > 1) {
        if (game::g_grenade_skip_frames.load() > 0) {
            uint32_t now_clip = R32(ac + kAnimClip);
            if (now_clip != game::g_grenade_skip_clip.load()) game::g_grenade_skip_frames = 0;
        }
        if (game::g_launcher_skip_frames.load() > 0) {
            uint32_t now_clip = R32(ac + kAnimClip);
            if (now_clip != game::g_launcher_skip_clip.load()) game::g_launcher_skip_frames = 0;
        }
    }
    {
        uint32_t now_clip = R32(ac + kAnimClip);
        std::string clip_name = ReadCString(ac + kAnimName, 13);
        {
            bool idle_name = clip_name.size() >= 5 && clip_name.compare(clip_name.size() - 5, 5, "S_001") == 0;
            bool aim_name = clip_name.size() >= 5 && clip_name.compare(clip_name.size() - 5, 5, "S_190") == 0;
            if (now_clip && (idle_name || aim_name) && IdleHoldNow()) {
                float hold_at = aim_name ? kAimPin : 0.0f;
                G<float>(now_clip + 0x14) = hold_at;
                G<float>(now_clip + 0x18) = hold_at;
            }
        }
    }
    if (G<int16_t>(clip + kAnimClipFadeSlot) == slot || FrameShare() >= 1) return;
    uint32_t weapon = R32(R32(ac + kAnimOwner) + kPlayerWeapon);
    if (weapon && R32(weapon + kWeaponState) == 1) {
        if (!IsFireClip(ReadCString(ac + kAnimName, 13))) g_idle_early = true;
    }
    bool fire = from && R32(from + 0x10) == 3 && R32(from + kAnimClipHash) == kFireLoopHash;
    if (fire && G<uint16_t>(fade + 0x24)) W32(fade + 0x18, 2);
}

X86_EXTERN_FUNC(__imp__sub_0013FC20);
X86_FUNC(sub_0013FC20) {
    float& hold = G<float>(ctx.esp + 8);
    hold = WholeFrames(hold, Dt());
    __imp__sub_0013FC20(ctx, base);
}

constexpr uint32_t kPhysics = 0x2D45F4;
constexpr uint32_t kSolverWorld = 0x18;
constexpr uint32_t kPhysicsBodyEnd = 0x270;
constexpr uint32_t kPhysicsBodies = 0x29C;
constexpr uint32_t kPhysicsGravity = 0x360;
constexpr uint32_t kPhysicsGravityStep = 0x370;
constexpr uint32_t kPhysicsStep = 0x380;
constexpr uint32_t kPhysicsRate = 0x384;
constexpr uint32_t kPhysicsRestLimit = 0x390;
constexpr uint32_t kBodyNext = 0x2C;
constexpr uint32_t kBodyImpulse = 0x90;

static void ScaleBodies(uint32_t world, float by) {
    static const uint32_t kMove[] = {0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0xA0, 0xA4, 0xA8};
    uint32_t body = R32(world + kPhysicsBodies);
    for (int left = 4096; body && body != world + kPhysicsBodyEnd && left; left--, body = R32(body + kBodyNext)) {
        for (uint32_t at : kMove) G<float>(body + at) *= by;
        for (int i = 0; i < 12; i += 4) {
            float g = G<float>(world + kPhysicsGravityStep + i);
            float& impulse = G<float>(body + kBodyImpulse + i);
            impulse = (impulse - g) * by + g * by * by;
        }
        G<float>(body + 0x9C) *= by * by;
    }
}

static bool g_rest_tick;
void PhysicsTime(X86Context&, uint8_t*) {
    uint32_t physics = R32(kPhysics);
    uint32_t solver = physics ? R32(physics) : 0;
    uint32_t world = solver ? R32(solver + kSolverWorld) : 0;
    float dt = FrameTime();
    float share = FrameShare();
    if (!world || !(dt > 0)) return;
    float& step = G<float>(world + kPhysicsStep);
    if (step > 0 && step != dt) ScaleBodies(world, dt / step);
    step = dt;
    G<float>(world + kPhysicsRate) = 1 / dt;
    for (int i = 0; i < 16; i += 4) {
        G<float>(world + kPhysicsGravityStep + i) = G<float>(world + kPhysicsGravity + i) * dt * dt;
    }
    static float limit;
    static float written;
    static float ticks;
    float& now = G<float>(world + kPhysicsRestLimit);
    if (now != written) limit = now;
    now = written = limit * share * share;
    ticks += share;
    g_rest_tick = ticks >= 1;
    if (g_rest_tick) ticks -= 1;
}
bool BodyRestTick(X86Context&, uint8_t*) { return g_rest_tick; }

void BodyDamping(X86Context& ctx, uint8_t*) {
    for (int i = 0; i < 2; i++)
        if (float& keep = ctx.xmm[i].f[0]; keep > 0) keep = std::pow(keep, FrameShare());
}
void BodyTopSpeed(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

void JointTurnCap(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= FrameShare(); }
void JointTurnCapNext(X86Context& ctx, uint8_t*) { ctx.xmm[6].f[0] *= FrameShare(); }

void ContactReach(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] = 1 / G<float>(kGameFrameTime); }

void RagdollLookAhead(X86Context& ctx, uint8_t*) {
    if (float share = FrameShare(); share > 0) ctx.xmm[0].f[0] /= share;
}

void JointAnchorGap(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) {
        float motion = ctx.xmm[2].f[i];
        ctx.xmm[1].f[i] = motion + (ctx.xmm[1].f[i] - motion) * share;
    }
}

constexpr uint32_t kJointTurnLow = 0x60;
constexpr uint32_t kJointTurnHigh = 0x80;
void JointTurnBack(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (uint32_t axis = 0; axis < 12; axis += 4) {
        float& low = G<float>(ctx.esp + kJointTurnLow + axis);
        float& high = G<float>(ctx.esp + kJointTurnHigh + axis);
        bool locked = low == high;
        if (locked || high < 0) high *= share;
        if (locked || low > 0) low *= share;
    }
}

constexpr uint32_t kShardAngle = 0xF4;

static bool g_shard_tick;
X86_EXTERN_FUNC(__imp__sub_001690D0);
X86_FUNC(sub_001690D0) {
    static float ticks;
    ticks += FrameShare();
    g_shard_tick = ticks >= 1;
    if (g_shard_tick) ticks -= 1;
    __imp__sub_001690D0(ctx, base);
}
bool ShardTick(X86Context&, uint8_t*) { return g_shard_tick; }

void ShardSpinGain(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

void ShardTurn(X86Context& ctx, uint8_t*) {
    ctx.xmm[0].f[0] = G<float>(ctx.ebx + kShardAngle) + ctx.xmm[1].f[0] * FrameShare();
}

void ShardStep(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) ctx.xmm[0].f[i] *= share;
}

static float Reach() { return float(std::clamp(game::Opt().draw_distance, 1u, 8u)); }
void DistanceShorter(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach(); }
void DistanceShorterSquared(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach() * Reach(); }
void LimitLonger(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= Reach(); }
void CharacterDetail(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] /= Reach(); }
void PropLimit(X86Context& ctx, uint8_t*) { ctx.xmm[2].f[0] *= Reach() * Reach(); }
constexpr uint32_t kParticleFarCut = 0xC4;
constexpr uint32_t kCameraFarCut = 0x88;

void ParticleFade(X86Context& ctx, uint8_t*) {
    float cut = std::min(G<float>(ctx.edi + kParticleFarCut) * ctx.xmm[1].f[0], G<float>(ctx.edx + kCameraFarCut));
    if (Reach() > 1 && cut > 0) ctx.xmm[3].f[0] *= ctx.xmm[2].f[0] / cut;
}

constexpr uint32_t kCoverPatches = 0x28;
constexpr uint32_t kPatchCount = 63;
constexpr uint32_t kPatchSize = 16;
constexpr uint32_t kPatchDistance = 4;

void GroundCoverFull(X86Context& ctx, uint8_t*) {
    if (ctx.eax < kPatchCount || game::Opt().draw_distance <= 1) return;
    uint32_t list = ctx.ebx + kCoverPatches;
    uint32_t last = list;
    for (uint32_t at = list + kPatchSize; at < list + kPatchCount * kPatchSize; at += kPatchSize)
        if (G<float>(at + kPatchDistance) > G<float>(last + kPatchDistance)) last = at;
    if (!(ctx.xmm[2].f[0] < G<float>(last + kPatchDistance))) return;
    W32(last, R32(ctx.ebp + 8));
    G<float>(last + kPatchDistance) = ctx.xmm[2].f[0];
    G<float>(last + 8) = ctx.xmm[1].f[0];
    G<uint8_t>(last + 0xC) = uint8_t(ctx.ecx);
    G<uint8_t>(last + 0xD) = ctx.edx == 2;
}

void DrawListRoom(X86Context&, uint8_t*) {
    static const uint32_t caps[] = {4096, 0, 2048, 512};
    static uint32_t room[4];
    uint32_t list = R32(kDisplay) + kDisplayDrawLists;
    for (int i = 0; i < 4; i++, list += kDrawListSize) {
        if (!caps[i]) continue;
        if (!room[i]) room[i] = mem::AllocSystem(caps[i] * 28);
        W32(list + kDrawListRoom, room[i]);
        if (R32(list + 4)) W32(list + 4, room[i] + caps[i] * 20);
        W32(list + kDrawListCap, caps[i]);
    }
}

bool ReloadBlur(X86Context&, uint8_t*) { return game::Opt().reload_blur; }

constexpr uint32_t kAnimationFrames = 0x1C;
void ReloadBlurHold(X86Context& ctx, uint8_t*) {
    float frames = G<float>(ctx.ecx + kAnimationFrames);
    ctx.xmm[0].f[0] = frames * G<float>(kGameFrameTime);
}

static bool SkipPressed() {
    static bool held = true;
    bool down = input::LeftButton();
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        down |= pad.analog[0] || (pad.buttons & 0x10);
    }
    bool pressed = down && !held;
    held = down;
    return pressed;
}

constexpr uint32_t kFilmName = 0x84;
constexpr uint32_t kFilmFlags = 0x198;
constexpr uint32_t kFilmState = 0x1A8;
constexpr uint32_t kFilmStop = 0x1B9;
constexpr uint32_t kFilmPlaying = 0x1C;
constexpr uint32_t kMovieFrameLength = 0x54;
constexpr uint32_t kMovieClockStep = 0x5C;

void SkipVideo(X86Context&, uint8_t*) {
    bool pressed = SkipPressed();
    uint32_t game_object = R32(kGame);
    uint32_t player = game_object + kFrontEnd + kFilmPlayer;
    uint32_t film = R32(game_object + kCurrentFilm);
    uint32_t state = R32(player + kFilmState);
    bool background = film < kFilms && G<uint8_t>(kFilmTable + film * kFilmEntry + kFilmBehindMenu);
    if (pressed) LOGI("film %u: press in state %X, flags %X", film, state, R32(player + kFilmFlags));
    if (uint32_t movie = R32(player); movie && state >= 4 && state <= kFilmPlaying) {
        G<float>(movie + kMovieFrameLength) = G<float>(kGameFrameTime);
        G<float>(movie + kMovieClockStep) = G<float>(game_object + kGameClockStep);
    }
    if (background || state < 2 || state > kFilmPlaying) return;
    std::string name = ReadCString(player + kFilmName, 8);
    bool intro = name.rfind("LO_", 0) == 0 || name.rfind("02_", 0) == 0;
    if (pressed || (game::Opt().skip_intros && intro)) G<uint8_t>(player + kFilmStop) = 1;
}
