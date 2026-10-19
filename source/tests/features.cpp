#include "FeatureData.h"
#include <cstddef>
#include <cassert>
#include <iostream>
#include <limits>
using namespace MyClientFeatures;
int main() {
    Command request{1000, 1, 'C', 0, 0};
    assert(WantsZoom(request, 1100, true, true, true));
    assert(!WantsZoom(request, 1251, true, true, true));
    assert(!WantsZoom(request, 999, true, true, true));
    assert(!WantsZoom(request, 1100, false, true, true));
    assert(!WantsZoom(request, 1100, true, false, true));
    assert(!WantsZoom(request, 1100, true, true, false));
    request.zoom = 0; assert(!WantsZoom(request, 1100, true, true, true));
    request.zoom = 2; assert(!WantsZoom(request, 1100, true, true, true));
    request.zoom = 1;
    for (uint32_t key : {0u, 255u, 0xffffffffu, 27u, unsigned('G')}) {
        request.key = key; assert(!WantsZoom(request, 1100, true, true, true));
    }
    assert(ZoomFov(90, false) == 90);
    assert(std::abs(ZoomFov(90, true) - 36.8699f) < .001f);
    assert(ZoomFov(70, true) < 70);
    assert(ZoomFov(0, true) == 0 && ZoomFov(200, true) == 200);
    assert(std::isnan(ZoomFov(std::numeric_limits<float>::quiet_NaN(), true)));
    Command bright{1000, 0, 'C', 1, 0};
    assert(WantsFullbright(bright, 1100));
    assert(!WantsFullbright(bright, 1251) && !WantsFullbright(bright, 999));  // lease expiry / future stamp
    bright.fullbright = 0; assert(!WantsFullbright(bright, 1100));
    bright.fullbright = 2; assert(!WantsFullbright(bright, 1100));
    assert(FullbrightScale(0.f, false) == 0.f && FullbrightScale(0.f, true) == 1.f);
    assert(FullbrightScale(0.4f, true) == 1.f && FullbrightScale(1.5f, true) == 1.5f);  // never lowers
    assert(FullbrightScale(0.4f, false) == 0.4f);
    assert(FullbrightScale(std::numeric_limits<float>::quiet_NaN(), true) == 1.f);
    Command tag{1000, 0, 0, 0, 1};
    assert(WantsNametag(tag, 1100) && !WantsNametag(tag, 1251) && !WantsNametag(tag, 999));  // lease expiry / future stamp
    tag.nametag = 0; assert(!WantsNametag(tag, 1100));
    tag.nametag = 2; assert(!WantsNametag(tag, 1100));
    assert(sizeof(Command) == 48 && offsetof(Command, autoRq) == 40 && offsetof(Command, nametag) == 20 && offsetof(Command, outline) == 24 &&
           offsetof(Command, outlineRgb) == 28 && offsetof(Command, texReload) == 32 && offsetof(Command, texturesOn) == 36);
    {   // texture reload requests: edge-triggered, primed by the first live command, ignore stale / switched-off commands
        Command c{5000, 0, 0, 0, 0, 0, 0, 7, 1};
        uint32_t seen = 0; bool primed = false;
        assert(!TextureReloadRequested(c, 5100, seen, primed) && primed && seen == 7);   // first sight: no reload
        assert(!TextureReloadRequested(c, 5100, seen, primed));                           // unchanged
        c.texReload = 8;
        assert(TextureReloadRequested(c, 5100, seen, primed) && seen == 8);               // one change = one request
        assert(!TextureReloadRequested(c, 5100, seen, primed));                           // not repeated
        c.texReload = 9; assert(!TextureReloadRequested(c, 5400, seen, primed) && seen == 8);   // stale lease: ignored, not consumed
        assert(TextureReloadRequested(c, 5100, seen, primed) && seen == 9);
        c.texReload = 10; c.texturesOn = 0; assert(!TextureReloadRequested(c, 5100, seen, primed));   // feature off: ignored
        c.texturesOn = 1; assert(TextureReloadRequested(c, 5100, seen, primed));
        c.texReload = 0; assert(TextureReloadRequested(c, 5100, seen, primed));           // HUD restarted its counter: still a change
    }
    Command outline{2000, 0, 0, 0, 0, 1, 0x50B4FF};
    assert(WantsOutline(outline, 2100) && !WantsOutline(outline, 2251) && !WantsOutline(outline, 1999));
    outline.outline = 0; assert(!WantsOutline(outline, 2100));
    assert(FlagOutline == 4 && FlagOutlineColored == 8);
    uint32_t outlineBits = WithFlag(WithFlag(0u, FlagOutline, true), FlagOutlineColored, true);
    assert(outlineBits == 12 && HasFlag(outlineBits, FlagOutline) && !HasFlag(outlineBits, FlagFullbright));
    uint32_t bits = 0;
    bits = WithFlag(bits, FlagNametag, true);
    assert(HasFlag(bits, FlagNametag) && !HasFlag(bits, FlagFullbright));
    bits = WithFlag(bits, FlagFullbright, true);
    assert(bits == 3 && HasFlag(bits, FlagFullbright) && HasFlag(bits, FlagNametag));
    bits = WithFlag(bits, FlagNametag, false);
    assert(bits == FlagFullbright);                       // an older DLL only ever sets bit 0
    assert(HasFlag(1u, FlagFullbright) && !HasFlag(1u, FlagNametag));
    assert(MakeArmor(false, 0, 0).state == Empty);
    assert(MakeArmor(true, 0, 0).state == NoDurability);
    auto armor = MakeArmor(true, 363, 120);
    assert(armor.state == Durable && armor.remaining == 243 && ValidArmor(armor));
    assert(MakeArmor(true, 363, 363).remaining == 0);
    assert(MakeArmor(true, 363, 364).state == Unknown);
    assert(MakeArmor(true, 363, -1).state == Unknown);
    assert(!ValidArmor({Durable, 500, 100}) && !ValidArmor({Empty, 1, 0}));
    assert(!Fresh(0, 100) && !Fresh(101, 100) && !Fresh(100, 2101) && Fresh(100, 2100));
    FrameRate fps; float value = 0; uint64_t at = 0;
    assert(!fps.Read(1000, value, at));
    for (uint64_t i = 0; i <= 60; ++i) fps.Frame(1000 + i * 1000 / 60);
    assert(fps.Read(2000, value, at) && std::abs(value - 60) < .01 && at == 2000);
    assert(!fps.Read(4001, value, at));
    fps.Frame(5000); assert(!fps.Read(5000, value, at));
    for (uint64_t i = 1; i <= 30; ++i) fps.Frame(5000 + i * 1000 / 30);
    assert(fps.Read(6000, value, at) && std::abs(value - 30) < .01);
    fps.Frame(100); assert(!fps.Read(100, value, at));
    fps.Reset(); assert(!fps.Read(100, value, at));
    Shared shared{}; shared.magic=Magic; shared.version=Version; shared.size=sizeof shared; shared.pid=123;
    assert(Valid(shared, 123) && !Valid(shared, 124));
    shared.version++; assert(!Valid(shared, 123));
    std::cout << "PASS: zoom lease/key/focus/menu release, fullbright lease/scale, FOV scaling, armor durability states, render FPS timing and staleness, extension ABI identity.\n";
}
