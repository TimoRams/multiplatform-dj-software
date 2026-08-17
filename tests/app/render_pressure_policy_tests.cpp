#include "app/RenderPressurePolicy.h"

#include <cassert>
#include <iostream>

int main()
{
    using Policy = RenderPressurePolicy;

    assert(Policy::targetTier({}) == Policy::Tier::Normal);
    assert(Policy::targetTier({.callbackLoad = 0.64}) == Policy::Tier::Normal);
    assert(Policy::targetTier({.callbackLoad = 0.65}) == Policy::Tier::Elevated);
    assert(Policy::targetTier({.callbackLoad = 0.85}) == Policy::Tier::Critical);
    assert(Policy::targetTier({.callbackOverrun = true}) == Policy::Tier::Critical);
    assert(Policy::targetTier({.hardwareXrun = true}) == Policy::Tier::Critical);
    assert(Policy::targetTier({.applicationActive = false}) == Policy::Tier::Suspended);
    assert(Policy::targetTier({.windowMinimized = true}) == Policy::Tier::Suspended);

    std::cout << "Render-pressure policy tests passed\n";
    return 0;
}
