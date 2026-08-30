#include <iostream>
#include "levelgen/WorldgenRandom.h"
#include "random/XoroshiroRandomSource.h"

using namespace minecraft;
using namespace minecraft::levelgen;

int main() {
    std::cout << "=== C++ WorldgenRandom Test ===" << std::endl << std::endl;
    
    XoroshiroRandomSource randomSource(12345L);
    WorldgenRandom random(randomSource);
    
    std::cout << "After construction with seed 12345:" << std::endl;
    
    // Simulate InSquarePlacement
    std::cout << "\nInSquarePlacement random calls:" << std::endl;
    int x1 = random.nextInt(16);
    int z1 = random.nextInt(16);
    std::cout << "  nextInt(16) = " << x1 << std::endl;
    std::cout << "  nextInt(16) = " << z1 << std::endl;
    
    // Simulate trapezoid with range=192, plateau=0
    std::cout << "\nTrapezoidHeight random calls (range=192, plateau=0):" << std::endl;
    int t1 = random.nextInt(97);  // plateauEnd + 1 = 97
    int t2 = random.nextInt(97);  // plateauStart + 1 = 97
    std::cout << "  nextInt(97) = " << t1 << std::endl;
    std::cout << "  nextInt(97) = " << t2 << std::endl;
    std::cout << "  result = -64 + " << t1 << " + " << t2 << " = " << (-64 + t1 + t2) << std::endl;
    
    std::cout << "\n=== Checking WorldgenRandom.nextInt behavior ===" << std::endl;
    random.setSeed(12345L);
    std::cout << "After fresh setSeed(12345):" << std::endl;
    for (int i = 0; i < 10; i++) {
        std::cout << "  nextInt(100) #" << i << " = " << random.nextInt(100) << std::endl;
    }
    
    return 0;
}
