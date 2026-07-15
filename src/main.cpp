#include "app/ApplicationBootstrap.h"
#include "app/CiSmokeTest.h"

#include <cstring>
#include <iostream>

int main(int argc, char* argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--version") == 0) {
            std::cout << "BrockDJ " << BROCKDJ_VERSION
                      << " (" << BROCKDJ_BUILD_ARCH << ")\n";
            return 0;
        }
        if (std::strcmp(argv[index], "--ci-smoke-test") == 0)
            return runCiSmokeTest(argc, argv);
    }

    return runApplication(argc, argv);
}
