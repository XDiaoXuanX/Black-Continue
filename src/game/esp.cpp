// The cheats page's ESP: ESP STYLE, box and/or line for each living enemy in view; ESP HEALTH BARS; ESP ARROWS, an
// arrowhead on a ring for enemies out of view; ESP LABEL, a plate with health and distance drawn by ImGui (EspOverlay);
// CROSSHAIR, a marker at the middle of the view (dot, cross or star) with its own colour and size. All drawn in the same
// 2D pass, over the finished frame. AUTO HEADSHOT and AIM ASSIST turn the view while aiming/firing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <imgui.h>

#include "common/aspect.h"
#include "common/options.h"
#include "common/filelog.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace touch { bool Firing(); }

namespace {

constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharHealth = 0x2F0;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kCharAi = 0x39C;
constexpr uint32_t kEnemyKind = 1;

constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

constexpr float kBoxWidth = 0.45f;
constexpr float kNear = 0.08f;
constexpr float kThickness = 0.8f;
constexpr float kBarThickness = 2.2f;
constexpr float kBarGap = 2.5f;
constexpr float kArrow = 9.0f;
constexpr float kRing = 0.30f;
constexpr float kBaseHeight = 1.8f;
constexpr float kChestShare = 0.66f;
constexpr auto kHitShow = std::chrono::seconds(4);
constexpr float kCone = 30.0f;
constexpr float kPull = 1.0f;
constexpr float kAssistCone[] = {0.0f, 6.0f, 10.0f, 15.0f};
constexpr float kAssistPull[] = {0.0f, 0.03f, 0.06f, 0.10f};
constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr auto kFireHold = std::chrono::milliseconds(150);

// crosshair: base arm length and line thickness, in lines of a 480 frame
constexpr float kCrossBaseSize = 6.0f;
constexpr float kCrossThickness = 1.5f;

constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 64;

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
    float peak;
    float last;
    std::chrono::steady_clock::time_point hit_at;
};
Seen g_seen[kMaxEnemies];
size_t g_count;

float g_view[16], g_projection[16];
bool g_camera;
uint32_t g_camera_frame;

constexpr uint32_t kTriangles = 0xD5A00;
struct Vertex { float x, y; };
constexpr size_t kSegments = 8;
// ESP + room for crosshair (dot 2 tris, cross 4 quads, star 6 quads + 2-tri dot ≈ 50 vertices)
constexpr size_t kMaxVertices = kMaxEnemies * kSegments * 6 + 128;

float Height() { return float(std::clamp(game::Opt().esp_height, 100u, 250u)) / 100.0f; }

bool Drawing() {
    const game::Options& o = game::Opt();
    return o.esp_style || o.esp_arrows || o.esp_health || o.esp_label || o.crosshair;
}
bool Wanted() { return Drawing() || game::Opt().aimbot || game::Opt().aim_assist; }

bool Target(uint32_t who) {
    if (R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) return false;
    return G<float>(who + kCharHealth) > 0 && R32(who + kCharAi) != 0;
}

void Remember(uint32_t who) {
    auto now = std::chrono::steady_clock::now();
    float health = G<float>(who + kCharHealth);
    for (size_t i = 0; i < g_count; i++)
        if (g_seen[i].who == who) {
            g_seen[i].at = now;
            g_seen[i].peak = std::max(g_seen[i].peak, health);
            if (health < g_seen[i].last - 0.001f) g_seen[i].hit_at = now;
            g_seen[i].last = health;
            return;
        }
    filelog::Write("esp", "character %08X kind %u ai %08X health %.0f", who, R32(who + kCharKind), R32(who + kCharAi), health);
    const auto long_ago = now - std::chrono::hours(1);
    if (g_count < kMaxEnemies) {
        g_seen[g_count++] = {who, now, health, health, long_ago};
        return;
    }
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now, health, health, long_ago};
}

bool Clip(const float p[3], float clip[4]) {
    float eye[4];
    for (int c = 0; c < 4; c++)
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    for (int c = 0; c < 4; c++)
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] + eye[2] * g_projection[8 + c] +
                  eye[3] * g_projection[12 + c];
    return clip[3] > kNear;
}

void Eye(const float p[3], float eye[3]) {
    for (int c = 0; c < 3; c++) eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
}

bool Project(const float p[3], float vx, float vy, float vw, float vh, float out[2]) {
    float clip[4];
    if (!Clip(p, clip)) return false;
    out[0] = vx + (clip[0] / clip[3] + 1) * 0.5f * vw;
    out[1] = vy + (1 - clip[1] / clip[3]) * 0.5f * vh;
    return true;
}

Vertex* Segment(Vertex* v, float x0, float y0, float x1, float y1, float width) {
    float dx = x1 - x0, dy = y1 - y0;
    float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0)) return v;
    float nx = -dy / length * width * 0.5f, ny = dx / length * width * 0.5f;
    const float corner[4][2] = {{x0 + nx, y0 + ny}, {x0 - nx, y0 - ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}};
    for (int i : {0, 1, 2, 2, 1, 3}) *v++ = {corner[i][0], corner[i][1]};
    return v;
}

void Unpack(uint32_t c, float out[4]) {
    out[0] = float((c >> 16) & 255) / 255.0f;
    out[1] = float((c >> 8) & 255) / 255.0f;
    out[2] = float(c & 255) / 255.0f;
    out[3] = 1.0f;
}

struct Label { float u, v, health, share, distance; };
struct Labels {
    Label item[kMaxEnemies];
    size_t count = 0;
    float on[4] = {0, 1, 0, 1};
    std::chrono::steady_clock::time_point at;
};
std::mutex g_labels_lock;
Labels g_labels;

void Publish(const Label* items, size_t count, const float on[4]) {
    std::lock_guard lock(g_labels_lock);
    g_labels.count = count;
    for (size_t i = 0; i < count; i++) g_labels.item[i] = items[i];
    std::memcpy(g_labels.on, on, 16);
    g_labels.at = std::chrono::steady_clock::now();
}

struct Turn { float yaw = 0, pitch = 0; bool found = false; };

Turn Nearest(float cone, float share, float side) {
    Turn turn;
    auto now = std::chrono::steady_clock::now();
    float best = cone;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || !Target(who)) continue;
        const float* at = &G<float>(who + kCharPosition);
        float point[3] = {at[0] + g_view[0] * side, at[1] + kBaseHeight * share + g_view[4] * side, at[2] + g_view[8] * side};
        float clip[4];
        if (!Clip(point, clip)) continue;
        float y = -std::atan(clip[0] / clip[3] / g_projection[0]) * kDegrees;
        float p = -std::atan(clip[1] / clip[3] / g_projection[5]) * kDegrees;
        float off = std::sqrt(y * y + p * p);
        if (off < best) best = off, turn = {y, p, true};
    }
    return turn;
}

}  // namespace

namespace game {

void EspCamera() {
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    std::memcpy(g_view, HostPtr(transforms), sizeof(g_view));
    std::memcpy(g_projection, HostPtr(transforms + 64), sizeof(g_projection));
    g_camera = true;
    g_camera_frame++;
}

void Aimbot(X86Context& ctx) {
    static uint32_t turned;
    static std::chrono::steady_clock::time_point fired;
    uint32_t c = ctx.esi;
    uint32_t me = R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0;
    uint32_t level = std::min(Opt().aim_assist, 3u);
    if ((!Opt().aimbot && !level) || !g_camera || !me || turned == g_camera_frame) return;
    turned = g_camera_frame;
    auto now = std::chrono::steady_clock::now();
    if (R32(me + kPlayerFireRequest)) fired = now;
    bool aiming = G<float>(me + kPlayerZoom) > 1.01f;
    bool firing = touch::Firing() || now - fired < kFireHold;
    Turn turn;
    float pull = 0;
    if (Opt().aimbot && aiming) {
        turn = Nearest(kCone, float(std::clamp(Opt().head_height, 40u, 100u)) / 100.0f,
                       float(int(std::clamp(Opt().head_offset, 0u, 60u)) - 30) / 100.0f);
        pull = kPull;
    } else if (level && firing) {
        turn = Nearest(kAssistCone[level], kChestShare, 0.0f);
        pull = kAssistPull[level];
    }
    if (!turn.found) return;
    G<float>(c + kControllerYaw) += turn.yaw * pull;
    G<float>(c + kControllerPitch) += turn.pitch * pull;
}

void EspOverlay() {
    uint32_t mode = std::min(Opt().esp_label, 3u);
    if (!mode) return;
    Labels l;
    {
        std::lock_guard lock(g_labels_lock);
        l = g_labels;
    }
    if (std::chrono::steady_clock::now() - l.at > std::chrono::milliseconds(150)) return;
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y, aspect = Aspect();
    float fw = std::min(w, h * aspect), fh = fw / aspect;
    float fx = (w - fw) * 0.5f, fy = (h - fh) * 0.5f;
    float line = fh / 480.0f;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 0.5f;
    float scale = size / ImGui::GetFontSize();
    float pad = size * 0.45f, gap = size * 0.7f;
    float lift = ((Opt().esp_health ? kBarGap + kBarThickness : 0.0f) + 2.5f) * line;
    ImU32 edge = ImGui::ColorConvertFloat4ToU32(ImVec4(l.on[0], l.on[1], l.on[2], 0.85f));
    ImU32 ground = IM_COL32(10, 12, 14, 175);
    ImU32 dim = IM_COL32(215, 222, 228, 255);
    char hp[16], dist[16];
    for (size_t i = 0; i < l.count; i++) {
        const Label& e = l.item[i];
        bool show_hp = mode & 1, show_dist = (mode & 2) && e.distance >= 0;
        if (!show_hp && !show_dist) continue;
        std::snprintf(hp, sizeof(hp), "%d", int(std::lround(e.health)));
        std::snprintf(dist, sizeof(dist), "%d m", int(std::lround(e.distance)));
        ImVec2 hp_size = ImGui::CalcTextSize(hp), dist_size = ImGui::CalcTextSize(dist);
        hp_size.x *= scale, hp_size.y *= scale, dist_size.x *= scale, dist_size.y *= scale;
        float inner = (show_hp ? hp_size.x : 0) + (show_dist ? dist_size.x : 0) + (show_hp && show_dist ? gap : 0);
        float text_h = std::max(hp_size.y, dist_size.y);
        float pw = inner + pad * 2, ph = text_h + pad;
        float cx = fx + e.u * fw, bottom = fy + e.v * fh - lift;
        float left = std::clamp(cx - pw * 0.5f, fx, std::max(fx, fx + fw - pw));
        float top = std::max(fy, bottom - ph);
        ImVec2 a(left, top), b(left + pw, top + ph);
        draw->AddRectFilled(a, b, ground, size * 0.35f);
        draw->AddRect(a, b, edge, size * 0.35f, 0, std::max(1.0f, fh / 540.0f));
        float x = left + pad, y = top + (ph - text_h) * 0.5f;
        if (show_hp) {
            ImU32 colour = ImGui::ColorConvertFloat4ToU32(
                ImVec4(std::min(1.0f, 2 * (1 - e.share)), std::min(1.0f, 2 * e.share), 0.2f, 1));
            draw->AddText(font, size, ImVec2(x, y), colour, hp);
            x += hp_size.x;
        }
        if (show_hp && show_dist) {
            float mid = x + gap * 0.5f;
            draw->AddLine(ImVec2(mid, top + ph * 0.25f), ImVec2(mid, top + ph * 0.75f), IM_COL32(255, 255, 255, 70));
            x += gap;
        }
        if (show_dist) draw->AddText(font, size, ImVec2(x, y), dim, dist);
    }
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end.
// Draws the ESP (if on) and/or the crosshair (if on) as untextured triangles in the game's own 2D layer.
// The ESP is drawn first, the crosshair on top of it.
void DrawEsp(X86Context& ctx, float unit) {
    float on[4], off[4];
    Unpack(Opt().esp_on_color, on);
    Unpack(Opt().esp_off_color, off);

    const bool draw_esp_only = Opt().esp_style || Opt().esp_arrows || Opt().esp_health || Opt().esp_label;
    const bool draw_cross = Opt().crosshair;

    if (!Drawing() || !g_camera || !R32(kWorld)) {
        Publish(nullptr, 0, on);
        return;
    }

    const uint32_t me = R32(kWorld) + kWorldPlayer;
    const float* mine = &G<float>(me + kCharPosition);
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    static const uint32_t buffer  = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset  = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * 16, 16);
    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    float(*colour)[4] = static_cast<float(*)[4]>(HostPtr(colours));
    Vertex* v = first;

    auto add = [&](float x0, float y0, float x1, float y1, float width, const float rgba[4]) {
        size_t before = size_t(v - first);
        v = Segment(v, x0, y0, x1, y1, width);
        for (size_t i = before; i < size_t(v - first); i++) std::memcpy(colour[i], rgba, 16);
    };
    auto add_square = [&](float cx, float cy, float s, const float rgba[4]) {
        float h = s * 0.5f;
        const float q[4][2] = {{cx - h, cy - h}, {cx + h, cy - h}, {cx + h, cy + h}, {cx - h, cy + h}};
        for (int i : {0, 1, 2, 2, 1, 3}) {
            *v = {q[i][0], q[i][1]};
            std::memcpy(colour[v - first], rgba, 16);
            v++;
        }
    };

    // ---- ESP (drawn first, so the crosshair sits on top) ----
    if (draw_esp_only) {
        Label labels[kMaxEnemies];
        size_t labelled = 0;
        const float back[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        const uint32_t style = std::min(Opt().esp_style, 3u), label_mode = std::min(Opt().esp_label, 3u);
        const float width = float(std::min(Opt().esp_width, 30u)) / 10.0f * unit;
        const float tail = kThickness * unit * 1.6f, bar = kBarThickness * unit;
        float height = Height();
        auto now = std::chrono::steady_clock::now();
        size_t kept = 0;
        for (size_t i = 0; i < g_count; i++) {
            uint32_t who = g_seen[i].who;
            if (now - g_seen[i].at > kSeen || !Target(who)) continue;
            Seen entry = g_seen[kept++] = g_seen[i];
            const float* at = &G<float>(who + kCharPosition);
            float chest[3] = {at[0], at[1] + height * kChestShare, at[2]}, clip[4];
            bool front = Clip(chest, clip);
            bool inside = front && std::fabs(clip[0]) <= clip[3] && std::fabs(clip[1]) <= clip[3];
            if (!inside) {
                if (!Opt().esp_arrows) continue;
                float dir[2];
                if (front) {
                    dir[0] = clip[0] / clip[3];
                    dir[1] = -clip[1] / clip[3];
                } else {
                    float eye[3];
                    Eye(chest, eye);
                    dir[0] = eye[0];
                    dir[1] = -eye[2];
                }
                float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
                if (!std::isfinite(length)) continue;
                if (length < 1e-4f) dir[0] = 0, dir[1] = front ? -1.0f : 1.0f, length = 1;
                float ux = dir[0] / length, uy = dir[1] / length;
                float ring = kRing * std::min(vw, vh), h = kArrow * unit;
                float mx = vx + vw * 0.5f + ux * ring, my = vy + vh * 0.5f + uy * ring;
                const float pts[3][2] = {{mx + ux * h, my + uy * h},
                                         {mx - ux * h * 0.5f - uy * h * 0.8f, my - uy * h * 0.5f + ux * h * 0.8f},
                                         {mx - ux * h * 0.5f + uy * h * 0.8f, my - uy * h * 0.5f - ux * h * 0.8f}};
                size_t before = size_t(v - first);
                for (const auto& q : pts) *v++ = {q[0], q[1]};
                for (size_t i = before; i < size_t(v - first); i++) std::memcpy(colour[i], off, 16);
                if (!front) add(mx - ux * h * 0.5f, my - uy * h * 0.5f, mx - ux * h * 2.2f, my - uy * h * 2.2f, tail, off);
                continue;
            }
            float feet[3] = {at[0], at[1], at[2]};
            float top[3] = {at[0], at[1] + height, at[2]};
            float f[2], h[2];
            if (!Project(feet, vx, vy, vw, vh, f) || !Project(top, vx, vy, vw, vh, h)) continue;
            float hp = G<float>(who + kCharHealth);
            float dx = at[0] - mine[0], dy = at[1] - mine[1], dz = at[2] - mine[2];
            bool hit = now - entry.hit_at < kHitShow;
            uint32_t show = std::min(Opt().esp_label_show, 2u);
            if (label_mode && (show == 1 || (show == 0 && style) || hit) && labelled < kMaxEnemies)
                labels[labelled++] = {(f[0] - vx) / vw, (h[1] - vy) / vh, hp,
                                      entry.peak > 0 ? std::clamp(hp / entry.peak, 0.0f, 1.0f) : 1.0f,
                                      std::sqrt(dx * dx + dy * dy + dz * dz)};
            float half = (f[1] - h[1]) * kBoxWidth * 0.5f;
            float left = f[0] - half, right = f[0] + half;
            if (width > 0) {
                if (style & 2) add(vx + vw * 0.5f, vy + vh, f[0], f[1], width, on);
                if (style & 1) {
                    add(left, h[1], right, h[1], width, on);
                    add(left, f[1], right, f[1], width, on);
                    add(left, h[1], left, f[1], width, on);
                    add(right, h[1], right, f[1], width, on);
                }
            }
            if (Opt().esp_health) {
                float frac = entry.peak > 0 ? std::clamp(G<float>(who + kCharHealth) / entry.peak, 0.0f, 1.0f) : 1.0f;
                float y = h[1] - kBarGap * unit - bar * 0.5f;
                const float fill[4] = {std::min(1.0f, 2 * (1 - frac)), std::min(1.0f, 2 * frac), 0.0f, 1.0f};
                add(left, y, right, y, bar, back);
                if (frac > 0) add(left, y, left + (right - left) * frac, y, bar, fill);
            }
        }
        g_count = kept;
        Publish(labels, labelled, on);
    } else {
        Publish(nullptr, 0, on);
    }

    // ---- CROSSHAIR (drawn on top of the ESP, independent of it) ----
    if (draw_cross) {
        float rgba[4];
        Unpack(Opt().crosshair_color, rgba);
        rgba[3] = 1.0f;
        float cx = vx + vw * 0.5f, cy = vy + vh * 0.5f;
        float size = kCrossBaseSize * float(std::clamp(Opt().crosshair_size, 10u, 500u)) / 100.0f * unit;
        float th = kCrossThickness * unit;
        uint32_t mode = Opt().crosshair_mode > 2 ? 0 : Opt().crosshair_mode;
        if (mode == 0) {
            add_square(cx, cy, size * 0.6f, rgba);
        } else if (mode == 1) {
            float gap = size * 0.25f;
            add(cx - gap - size, cy, cx - gap, cy, th, rgba);
            add(cx + gap, cy, cx + gap + size, cy, th, rgba);
            add(cx, cy - gap - size, cx, cy - gap, th, rgba);
            add(cx, cy + gap, cx, cy + gap + size, th, rgba);
        } else {
            float gap = size * 0.3f;
            for (int i = 0; i < 3; i++) {
                float ang = float(i) * 3.14159265f / 3.0f;
                float co = std::cos(ang), si = std::sin(ang);
                add(cx + gap * co, cy + gap * si, cx + (gap + size) * co, cy + (gap + size) * si, th, rgba);
                add(cx - gap * co, cy - gap * si, cx - (gap + size) * co, cy - (gap + size) * si, th, rgba);
            }
            add_square(cx, cy, size * 0.25f, rgba);
        }
    }

    if (v == first) return;
    W32(offset, 0);
    W32(offset + 4, 0);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = uint32_t(v - first) / 3;
    ctx.ecx = buffer;
    CallGuest(ctx, kTriangles, {offset, colours});
    ctx.eax = eax;
    ctx.ecx = ecx;
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if (Wanted() && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
