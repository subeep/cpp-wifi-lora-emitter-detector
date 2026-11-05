#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace rfmon::wifi {
// IEEE 802.11 HT/VHT LDPC (802.11-2020 19.3.11.7): PPDU encoding parameters,
// a layered min-sum decoder and an encoder (used by the tests).
struct LdpcPlan {
    int codewords = 0, length = 0;   // N_CW, L_LDPC (648 / 1296 / 1944)
    int rate_num = 1, rate_den = 2;
    long payload_bits = 0;           // N_pld (SERVICE + PSDU + padding)
    long coded_bits = 0;             // N_avbits
    long shortened = 0, punctured = 0, repeated = 0;
    bool extra_symbol = false;       // an extra OFDM symbol (x m_STBC) was added
};
// Parameters for n_pld payload bits in n_avbits available coded bits.
// 802.11ac signals the extra symbol (VHT-SIG-A); for 802.11n pass nullopt
// and the standard's rule decides.
LdpcPlan ldpc_plan(long n_pld, long n_avbits, int rate_num, int rate_den, int cbps, int m_stbc,
                   std::optional<bool> extra_symbol);
// 802.11ax (27.3.12.5.2): the codeword choice uses the initial available
// bits; puncturing and repetition use the final ones, which include any LDPC
// extra symbol segment.
LdpcPlan ldpc_plan_he(long n_pld, long n_avbits_init, long n_avbits, int rate_num, int rate_den);
// plan.coded_bits soft bits in (positive = 1), plan.payload_bits bits out.
// Returns the number of codewords whose parity checks are not all met.
int ldpc_decode(const float* llr, const LdpcPlan& plan, std::vector<uint8_t>& bits, int max_iterations = 30);
// plan.payload_bits bits in, plan.coded_bits bits out.
std::vector<uint8_t> ldpc_encode(const std::vector<uint8_t>& payload, const LdpcPlan& plan);
}
