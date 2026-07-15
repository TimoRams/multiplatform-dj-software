#include "app/UiScaleController.h"

#include <iostream>
#include <limits>

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    ok &= require(UiScaleController::validatedScale(0.1) == 0.8, "minimum clamps to 80 percent");
    ok &= require(UiScaleController::validatedScale(2.0) == 1.4, "maximum clamps to 140 percent");
    ok &= require(UiScaleController::validatedScale(1.08) == 1.1, "stored scale snaps to supported step");
    ok &= require(UiScaleController::validatedScale(std::numeric_limits<double>::quiet_NaN()) == 1.0,
                  "invalid stored scale falls back to 100 percent");
    ok &= require(UiScaleController::increasedScale(1.0) == 1.1, "step up");
    ok &= require(UiScaleController::decreasedScale(1.0) == 0.9, "step down");
    ok &= require(UiScaleController::increasedScale(1.4) == 1.4, "upper limit is stable");
    ok &= require(UiScaleController::decreasedScale(0.8) == 0.8, "lower limit is stable");
    return ok ? 0 : 1;
}
