#pragma once
#include "chirp_structure.hpp"
#include <array>
namespace rfmon::cyclo {
struct MarkerStructure {
    bool evaluated=false,prefix_compatible=false,joint_compatible=false;
    std::size_t start=0,lag=0,gate=0,trials=0;
    double coherence=0,first_half=0,second_half=0,balance=0,guard_max=0;
    double span_min=0,span_energy=0;
    bool boundary_contrast=false;
};
std::array<MarkerStructure,3> measure_marker_structures(
    const std::vector<std::complex<float>>& iq,double rate,const std::vector<ChirpMeasurement>& chirps);
// Shared by strict wire validation and tests; no acceptance or identity inference.
bool valid_marker_structure(const MarkerStructure&,double rate,std::size_t samples,const ChirpMeasurement&);
}
