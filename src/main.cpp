#include <juce_core/juce_core.h>
#include <iostream>

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "RAMSBROCK DJ ENGINE - INITIAL BUILD TEST" << std::endl;
    
    // Wir nutzen die juce::SystemStats Klasse. Das funktioniert immer,
    // solange juce_core verlinkt ist.
    std::cout << "JUCE Version: " << juce::SystemStats::getJUCEVersion() << std::endl;
    
    std::cout << "========================================" << std::endl;

    return 0;
}