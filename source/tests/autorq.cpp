#include "../AutoRq.h"
#include <cassert>
using namespace MyClientAutoRq;
int main() {
    assert(kModeCount == 10 && Command(0).empty() && Command(11).empty());
    assert(Command(1) == "/q SKY" && Command(4) == "/q SKY-MEGA" && Command(5) == "/q BED" && Command(9) == "/q BED-DUELS" && Command(10) == "/q BED-DUELS-DUOS");
    Gate g;
    assert(!g.Update(true, true, 1000));              // first sample already dead (joined as spectator): no edge
    assert(!g.Update(true, false, 1100));
    assert(g.Update(true, true, 1200));               // alive -> dead fires
    assert(!g.Update(true, true, 1300));              // still dead: once only
    assert(!g.Update(true, false, 1400) && !g.Update(true, true, 1500));  // alive again, died inside cooldown
    assert(!g.Update(true, false, 20000));
    assert(g.Update(true, true, 21000));              // next game, next death
    Gate off; assert(!off.Update(true, false, 1)); assert(!off.Update(false, true, 2));  // disabled: never
    return 0;
}
