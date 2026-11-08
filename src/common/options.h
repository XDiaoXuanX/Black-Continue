#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};

struct Options {
    bool skip_intros = false;
    bool fps_counter = false;
    uint32_t fmv_scale = 0;
    uint32_t screen_filter = 0;
    bool reload_blur = true;
    bool bloom = true;
    bool fog = true;
    uint32_t anti_aliasing = 0;
    uint32_t mali_fix = 1;
    bool hdr = false;
    uint32_t draw_distance = 1;
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;
    bool invert_mouse = false;
    bool hold_aim = true;
    uint32_t fov = 70;
    uint32_t viewmodel_fov = 70;
    uint32_t walk_speed = 50;
    uint32_t left_deadzone = 0;
    uint32_t right_deadzone = 0;
    float left_sensitivity = 1.0f;
    float right_sensitivity = 1.0f;
    uint32_t audio_output = 0;
    std::string language = "int";
    bool infinite_ammo = false;
    // cheat: 1 OFF, 2-998 = times the game's rate of fire, 999 = INFINITE (no wait at all, every weapon fires each frame)
    uint32_t rapid_fire = 1;
    bool no_recoil = false;
    bool steady_aim = true;
    uint32_t aim_view = 1;
    bool no_kick = false;
    uint32_t ads_custom = 0;
    uint32_t game_speed = 100;
    bool god_mode = false;
    bool one_hit_kill = false;
    bool infinite_grenades = false;
    bool infinite_medkits = false;
    bool unlock_levels = false;
    bool unlock_black_ops = false;
    uint32_t esp_style = 0;
    bool esp_arrows = false;
    bool aimbot = false;
    uint32_t aim_assist = 0;
    uint32_t head_height = 90;
    uint32_t head_offset = 30;
    uint32_t esp_label = 0;
    uint32_t esp_label_show = 0;
    bool esp_health = false;
    uint32_t esp_width = 8;
    uint32_t esp_height = 180;
    uint32_t esp_on_color = 0xFFFFFF;
    uint32_t esp_off_color = 0xFF0000;

    // rapid fire extra: the launchers fire at this multiplier (1 OFF, 2-998 = times the game's rate of fire,
    // 999 = INFINITE, no wait at all). Applies to RPG and GRL (all their fire) and to the M203 under the barrel of
    // the BNS (only the launcher, the M16 rifle above it uses RAPID FIRE). Like the grenade throw, the fire
    // animation is fast-forwarded so it completes in one frame and the next shot can start at once. Its own level,
    // whatever RAPID FIRE is set to
    uint32_t rapid_fire_extra = 1;

    // crosshair: a marker at the middle of the view, drawn by esp.cpp in the same 2D pass as the ESP
    bool crosshair = false;
    uint32_t crosshair_mode = 0;
    uint32_t crosshair_size = 100;
    uint32_t crosshair_color = 0x00FF00;

    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
const std::string& BootLanguage();
bool HasLanguages();
bool LanguageAvailable(const std::string& code);
inline std::atomic<float> g_fps{0};
inline std::atomic<bool> g_hide_hud{false};
inline std::atomic<float> g_zoom{1.0f};
inline std::atomic<bool> g_focus_paused{false};
inline std::atomic<uint32_t> g_focus_pauses{0};

inline std::atomic<bool> g_grenade_skip_request{false};
inline std::atomic<uint32_t> g_grenade_skip_clip{0};
inline std::atomic<int> g_grenade_skip_frames{0};

inline std::atomic<bool> g_launcher_skip_request{false};
inline std::atomic<uint32_t> g_launcher_skip_clip{0};
inline std::atomic<int> g_launcher_skip_frames{0};

void WaitWhileUnfocused();
bool HudShown();

}  // namespace game
