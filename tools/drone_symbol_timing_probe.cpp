// Offline parity adapter: fixed cf32 timing blocks on stdin, one ratio array per block.
#include "drones/symbol_timing.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
int main() {
    using namespace rfmon::symbol_timing;
    Probe probe;
    std::vector<std::complex<float>> samples(block_size);
    while (std::cin.read(reinterpret_cast<char*>(samples.data()), samples.size()*sizeof(samples[0]))) {
        Ratios ratios{};
        if (!probe.measure(samples.data(),20e6,ratios)) return 2;
        std::cout << nlohmann::json(ratios).dump() << '\n';
    }
    return std::cin.gcount()==0 ? 0 : 2;
}
