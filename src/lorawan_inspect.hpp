#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfmon {
// Structural inspection only. No key, MIC authentication, session association,
// counter reconstruction or MAC-command/decryption engine exists here.
// Layout reference: LoRa Alliance TS001-1.0.4 sections 4 and 6.
struct LoRaWanCandidate { std::string type, detail; };
inline std::optional<LoRaWanCandidate> inspect_lorawan(const std::vector<uint8_t>& b) {
    if (b.empty() || b.size()>255 || (b[0]&0x1f)!=0) return std::nullopt;
    const int type=b[0]>>5;
    auto hex=[&](size_t begin,size_t end,bool reverse=false) {
        std::string out; const char* digits="0123456789ABCDEF";
        for(size_t i=begin;i<end;++i) {
            auto v=b[reverse ? end-1-(i-begin) : i];
            out+=digits[v>>4];out+=digits[v&15];
        }
        return out;
    };
    LoRaWanCandidate r;
    if(type==0) {
        if(b.size()!=23)return std::nullopt;
        r.type="Join request candidate";
        r.detail="JoinEUI/AppEUI="+hex(1,9,true)+" DevEUI="+hex(9,17,true)+
            " DevNonce="+std::to_string(unsigned(b[17])+(unsigned(b[18])<<8))+" MIC="+hex(19,23);
    } else if(type==1) {
        if(b.size()!=17&&b.size()!=33)return std::nullopt;
        r.type="Join accept candidate";
        r.detail="Encrypted join-accept body retained in raw PHY payload; fields/MIC not extracted.";
    } else if(type>=2&&type<=5) {
        if(b.size()<12)return std::nullopt;
        const bool uplink=type==2||type==4;
        const size_t options=b[5]&15, body_end=b.size()-4, header_end=8+options;
        if(header_end>body_end || (!uplink&&(b[5]&0x40)))return std::nullopt;
        r.type=std::string(type>=4?"Confirmed ":"Unconfirmed ")+(uplink?"uplink candidate":"downlink candidate");
        r.detail="DevAddr="+hex(1,5,true)+" FCnt16="+std::to_string(unsigned(b[6])+(unsigned(b[7])<<8))+
            " (lower 16 bits only) FCtrl="+hex(5,6)+" ADR="+std::to_string((b[5]>>7)&1)+
            " ACK="+std::to_string((b[5]>>5)&1)+" FOpts="+hex(8,header_end);
        if(header_end<body_end) {
            unsigned port=b[header_end];
            if(options&&port==0)return std::nullopt;
            r.detail+=" FPort="+std::to_string(port)+" FRMPayload="+hex(header_end+1,body_end)+" (opaque)";
        } else r.detail+=" FPort absent; no FRMPayload";
        r.detail+=" MIC="+hex(body_end,b.size());
    } else return std::nullopt; // Rejoin/proprietary layouts require separate support.
    r.detail+=". Structural candidate only; MIC not verified, protocol/session/version not established. No decryption.";
    return r;
}
} // namespace rfmon
