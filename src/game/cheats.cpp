// The cheats page's god mode, one-hit kill and infinite grenades. The rest live where the game
// already has a hook: infinite ammo, rapid fire and no recoil with the weapon's, game speed with the frame time
#include <algorithm>
#include <cstring>

#include "common/options.h"
#include "common/filelog.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharGrenades = 0x286;
constexpr uint16_t kGrenadeMax = 9;
constexpr uint32_t kCharHealth = 0x2F0;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;
constexpr float kFullHealth[] = {1200.0f, 750.0f};

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

constexpr float kSliver = 0.001f;

struct Doomed {
    uint32_t who = 0;
    float health = 0;
};

Doomed Doom(uint32_t victim, uint32_t attacker) {
    uint32_t me = Player();
    if (!game::Opt().one_hit_kill || !me || attacker != me || victim == me || R32(victim + kCharDying)) return {};
    float& health = G<float>(victim + kCharHealth);
    if (!(health > kSliver)) return {};
    Doomed kept{victim, health};
    health = kSliver;
    return kept;
}

void Spare(const Doomed& kept) {
    if (!kept.who || R32(kept.who + kCharDying)) return;
    float& health = G<float>(kept.who + kCharHealth);
    if (health > 0) health = kept.health - (kSliver - health);
}

bool GuestPointer(uint32_t v) {
    return !(v & 3) && ((v >= 0x10000 && v < 0x04000000) || (v >= 0x80000000 && v < 0x84000000));
}

void LogHit(uint32_t victim, uint32_t attacker, uint32_t esp) {
    static int logged = 0;
    uint32_t me = Player();
    if (logged >= 80 || !me || attacker != me || victim == me) return;
    logged++;
    const float* feet = &G<float>(victim + 0xA0);
    filelog::Write("hit_test", "hit %d: victim %08X feet %.2f %.2f %.2f health %.1f", logged, victim, feet[0], feet[1],
                   feet[2], G<float>(victim + kCharHealth));
    for (int i = 0; i < 7; i++) {
        uint32_t v = R32(esp + 4 + 4 * i);
        float f;
        std::memcpy(&f, &v, 4);
        if (GuestPointer(v)) {
            const float* p = &G<float>(v);
            filelog::Write("hit_test", "hit %d arg %d: %08X (guest) -> %.3f %.3f %.3f", logged, i, v, p[0], p[1], p[2]);
        } else {
            filelog::Write("hit_test", "hit %d arg %d: %08X (%d, %.3f)", logged, i, v, int32_t(v), f);
        }
    }
}

bool g_invulnerable;

}  // namespace

X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    if (me && me == Player() && R32(me + kCharKind) == kPlayerKind) {
        if (game::Opt().infinite_grenades && G<uint16_t>(me + kCharGrenades) < kGrenadeMax)
            G<uint16_t>(me + kCharGrenades) = kGrenadeMax;
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
    }
    __imp__sub_001E5FA0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    LogHit(ctx.ecx, R32(ctx.esp + 0x18), ctx.esp);
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event: the throw takes a grenade, the cheat puts the player's back. RAPID FIRE EXTRA
// (rapid_fire_extra > 1): a throw keeps the count and asks the animation controller (hooks.cpp's sub_0017B130) to
// fast-forward the throw clip, so the next throw can start at once. rapid_fire_extra is a uint32_t now (options.h):
// 1 = OFF, 2-999 = active (999 = INFINITE). Grenade throws are instant for every rapid_fire_extra > 1
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool is_player = me && me == Player();
    uint16_t before = is_player ? G<uint16_t>(me + kCharGrenades) : 0;
    // keep the count when either cheat wants it: INFINITE GRENADES, or RAPID FIRE EXTRA at any level above OFF
    bool keep = is_player && (game::Opt().infinite_grenades || game::Opt().rapid_fire_extra > 1);
    __imp__sub_001E8830(ctx, base);
    uint16_t after = is_player ? G<uint16_t>(me + kCharGrenades) : 0;
    bool thrown = is_player && after < before;
    if (keep && thrown) G<uint16_t>(me + kCharGrenades) = before;
    // RAPID FIRE EXTRA: fast-forward the throw animation so the next grenade can be thrown without waiting
    if (thrown && game::Opt().rapid_fire_extra > 1) game::g_grenade_skip_request = true;
}
