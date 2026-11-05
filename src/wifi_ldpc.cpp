#include "wifi_ldpc.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
namespace rfmon::wifi {
namespace {
// Base matrices (Annex F / 802.11n Annex R): 24 column blocks of Z x Z
// cyclic shifts (-1 = zero block), Z = L/24. Cross-checked between two
// independent transcriptions (docs/WIFI_HT_VHT_LDPC_2026-11-04.md).
struct BaseMatrix { int length,num,den,rows; int8_t h[12][24]; };
const BaseMatrix matrices[]={
    {648,1,2,12,{
        { 0,-1,-1,-1, 0, 0,-1,-1, 0,-1,-1, 0, 1, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {22, 0,-1,-1,17,-1, 0, 0,12,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        { 6,-1, 0,-1,10,-1,-1,-1,24,-1, 0,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1},
        { 2,-1,-1, 0,20,-1,-1,-1,25, 0,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1},
        {23,-1,-1,-1, 3,-1,-1,-1, 0,-1, 9,11,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1},
        {24,-1,23, 1,17,-1, 3,-1,10,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {25,-1,-1,-1, 8,-1,-1,-1, 7,18,-1,-1, 0,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        {13,24,-1,-1, 0,-1, 8,-1, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1},
        { 7,20,-1,16,22,10,-1,-1,23,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1},
        {11,-1,-1,-1,19,-1,-1,-1,13,-1, 3,17,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        {25,-1, 8,-1,23,18,-1,14, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0},
        { 3,-1,-1,-1,16,-1,-1, 2,25, 5,-1,-1, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0}}},
    {648,2,3,8,{
        {25,26,14,-1,20,-1, 2,-1, 4,-1,-1, 8,-1,16,-1,18, 1, 0,-1,-1,-1,-1,-1,-1},
        {10, 9,15,11,-1, 0,-1, 1,-1,-1,18,-1, 8,-1,10,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {16, 2,20,26,21,-1, 6,-1, 1,26,-1, 7,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        {10,13, 5, 0,-1, 3,-1, 7,-1,-1,26,-1,-1,13,-1,16,-1,-1,-1, 0, 0,-1,-1,-1},
        {23,14,24,-1,12,-1,19,-1,17,-1,-1,-1,20,-1,21,-1, 0,-1,-1,-1, 0, 0,-1,-1},
        { 6,22, 9,20,-1,25,-1,17,-1, 8,-1,14,-1,18,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        {14,23,21,11,20,-1,24,-1,18,-1,19,-1,-1,-1,-1,22,-1,-1,-1,-1,-1,-1, 0, 0},
        {17,11,11,20,-1,21,-1,26,-1, 3,-1,-1,18,-1,26,-1, 1,-1,-1,-1,-1,-1,-1, 0}}},
    {648,3,4,6,{
        {16,17,22,24, 9, 3,14,-1, 4, 2, 7,-1,26,-1, 2,-1,21,-1, 1, 0,-1,-1,-1,-1},
        {25,12,12, 3, 3,26, 6,21,-1,15,22,-1,15,-1, 4,-1,-1,16,-1, 0, 0,-1,-1,-1},
        {25,18,26,16,22,23, 9,-1, 0,-1, 4,-1, 4,-1, 8,23,11,-1,-1,-1, 0, 0,-1,-1},
        { 9, 7, 0, 1,17,-1,-1, 7, 3,-1, 3,23,-1,16,-1,-1,21,-1, 0,-1,-1, 0, 0,-1},
        {24, 5,26, 7, 1,-1,-1,15,24,15,-1, 8,-1,13,-1,13,-1,11,-1,-1,-1,-1, 0, 0},
        { 2, 2,19,14,24, 1,15,19,-1,21,-1, 2,-1,24,-1, 3,-1, 2, 1,-1,-1,-1,-1, 0}}},
    {648,5,6,4,{
        {17,13, 8,21, 9, 3,18,12,10, 0, 4,15,19, 2, 5,10,26,19,13,13, 1, 0,-1,-1},
        { 3,12,11,14,11,25, 5,18, 0, 9, 2,26,26,10,24, 7,14,20, 4, 2,-1, 0, 0,-1},
        {22,16, 4, 3,10,21,12, 5,21,14,19, 5,-1, 8, 5,18,11, 5, 5,15, 0,-1, 0, 0},
        { 7, 7,14,14, 4,16,16,24,24,10, 1, 7,15, 6,10,26, 8,18,21,14, 1,-1,-1, 0}}},
    {1296,1,2,12,{
        {40,-1,-1,-1,22,-1,49,23,43,-1,-1,-1, 1, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {50, 1,-1,-1,48,35,-1,-1,13,-1,30,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {39,50,-1,-1, 4,-1, 2,-1,-1,-1,-1,49,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1},
        {33,-1,-1,38,37,-1,-1, 4, 1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1},
        {45,-1,-1,-1, 0,22,-1,-1,20,42,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1},
        {51,-1,-1,48,35,-1,-1,-1,44,-1,18,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {47,11,-1,-1,-1,17,-1,-1,51,-1,-1,-1, 0,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        { 5,-1,25,-1, 6,-1,45,-1,13,40,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1},
        {33,-1,-1,34,24,-1,-1,-1,23,-1,-1,46,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1},
        { 1,-1,27,-1, 1,-1,-1,-1,38,-1,44,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        {-1,18,-1,-1,23,-1,-1, 8, 0,35,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0},
        {49,-1,17,-1,30,-1,-1,-1,34,-1,-1,19, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0}}},
    {1296,2,3,8,{
        {39,31,22,43,-1,40, 4,-1,11,-1,-1,50,-1,-1,-1, 6, 1, 0,-1,-1,-1,-1,-1,-1},
        {25,52,41, 2, 6,-1,14,-1,34,-1,-1,-1,24,-1,37,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {43,31,29, 0,21,-1,28,-1,-1, 2,-1,-1, 7,-1,17,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        {20,33,48,-1, 4,13,-1,26,-1,-1,22,-1,-1,46,42,-1,-1,-1,-1, 0, 0,-1,-1,-1},
        {45, 7,18,51,12,25,-1,-1,-1,50,-1,-1, 5,-1,-1,-1, 0,-1,-1,-1, 0, 0,-1,-1},
        {35,40,32,16, 5,-1,-1,18,-1,-1,43,51,-1,32,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        { 9,24,13,22,28,-1,-1,37,-1,-1,25,-1,-1,52,-1,13,-1,-1,-1,-1,-1,-1, 0, 0},
        {32,22, 4,21,16,-1,-1,-1,27,28,-1,38,-1,-1,-1, 8, 1,-1,-1,-1,-1,-1,-1, 0}}},
    {1296,3,4,6,{
        {39,40,51,41, 3,29, 8,36,-1,14,-1, 6,-1,33,-1,11,-1, 4, 1, 0,-1,-1,-1,-1},
        {48,21,47, 9,48,35,51,-1,38,-1,28,-1,34,-1,50,-1,50,-1,-1, 0, 0,-1,-1,-1},
        {30,39,28,42,50,39, 5,17,-1, 6,-1,18,-1,20,-1,15,-1,40,-1,-1, 0, 0,-1,-1},
        {29, 0, 1,43,36,30,47,-1,49,-1,47,-1, 3,-1,35,-1,34,-1, 0,-1,-1, 0, 0,-1},
        { 1,32,11,23,10,44,12, 7,-1,48,-1, 4,-1, 9,-1,17,-1,16,-1,-1,-1,-1, 0, 0},
        {13, 7,15,47,23,16,47,-1,43,-1,29,-1,52,-1, 2,-1,53,-1, 1,-1,-1,-1,-1, 0}}},
    {1296,5,6,4,{
        {48,29,37,52, 2,16, 6,14,53,31,34, 5,18,42,53,31,45,-1,46,52, 1, 0,-1,-1},
        {17, 4,30, 7,43,11,24, 6,14,21, 6,39,17,40,47, 7,15,41,19,-1,-1, 0, 0,-1},
        { 7, 2,51,31,46,23,16,11,53,40,10, 7,46,53,33,35,-1,25,35,38, 0,-1, 0, 0},
        {19,48,41, 1,10, 7,36,47, 5,29,52,52,31,10,26, 6, 3, 2,-1,51, 1,-1,-1, 0}}},
    {1944,1,2,12,{
        {57,-1,-1,-1,50,-1,11,-1,50,-1,79,-1, 1, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        { 3,-1,28,-1, 0,-1,-1,-1,55, 7,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {30,-1,-1,-1,24,37,-1,-1,56,14,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1,-1},
        {62,53,-1,-1,53,-1,-1, 3,35,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1,-1},
        {40,-1,-1,20,66,-1,-1,22,28,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1,-1},
        { 0,-1,-1,-1, 8,-1,42,-1,50,-1,-1, 8,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {69,79,79,-1,-1,-1,56,-1,52,-1,-1,-1, 0,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        {65,-1,-1,-1,38,57,-1,-1,72,-1,27,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1},
        {64,-1,-1,-1,14,52,-1,-1,30,-1,-1,32,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1,-1},
        {-1,45,-1,70, 0,-1,-1,-1,77, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        { 2,56,-1,57,35,-1,-1,-1,-1,-1,12,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0, 0},
        {24,-1,61,-1,60,-1,-1,27,51,-1,-1,16, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0}}},
    {1944,2,3,8,{
        {61,75, 4,63,56,-1,-1,-1,-1,-1,-1, 8,-1, 2,17,25, 1, 0,-1,-1,-1,-1,-1,-1},
        {56,74,77,20,-1,-1,-1,64,24, 4,67,-1, 7,-1,-1,-1,-1, 0, 0,-1,-1,-1,-1,-1},
        {28,21,68,10, 7,14,65,-1,-1,-1,23,-1,-1,-1,75,-1,-1,-1, 0, 0,-1,-1,-1,-1},
        {48,38,43,78,76,-1,-1,-1,-1, 5,36,-1,15,72,-1,-1,-1,-1,-1, 0, 0,-1,-1,-1},
        {40, 2,53,25,-1,52,62,-1,20,-1,-1,44,-1,-1,-1,-1, 0,-1,-1,-1, 0, 0,-1,-1},
        {69,23,64,10,22,-1,21,-1,-1,-1,-1,-1,68,23,29,-1,-1,-1,-1,-1,-1, 0, 0,-1},
        {12, 0,68,20,55,61,-1,40,-1,-1,-1,52,-1,-1,-1,44,-1,-1,-1,-1,-1,-1, 0, 0},
        {58, 8,34,64,78,-1,-1,11,78,24,-1,-1,-1,-1,-1,58, 1,-1,-1,-1,-1,-1,-1, 0}}},
    {1944,3,4,6,{
        {48,29,28,39, 9,61,-1,-1,-1,63,45,80,-1,-1,-1,37,32,22, 1, 0,-1,-1,-1,-1},
        { 4,49,42,48,11,30,-1,-1,-1,49,17,41,37,15,-1,54,-1,-1,-1, 0, 0,-1,-1,-1},
        {35,76,78,51,37,35,21,-1,17,64,-1,-1,-1,59, 7,-1,-1,32,-1,-1, 0, 0,-1,-1},
        { 9,65,44, 9,54,56,73,34,42,-1,-1,-1,35,-1,-1,-1,46,39, 0,-1,-1, 0, 0,-1},
        { 3,62, 7,80,68,26,-1,80,55,-1,36,-1,26,-1, 9,-1,72,-1,-1,-1,-1,-1, 0, 0},
        {26,75,33,21,69,59, 3,38,-1,-1,-1,35,-1,62,36,26,-1,-1, 1,-1,-1,-1,-1, 0}}},
    {1944,5,6,4,{
        {13,48,80,66, 4,74, 7,30,76,52,37,60,-1,49,73,31,74,73,23,-1, 1, 0,-1,-1},
        {69,63,74,56,64,77,57,65, 6,16,51,-1,64,-1,68, 9,48,62,54,27,-1, 0, 0,-1},
        {51,15, 0,80,24,25,42,54,44,71,71, 9,67,35,-1,58,-1,29,-1,53, 0,-1, 0, 0},
        {16,29,36,41,44,56,59,37,50,24,-1,65, 4,65,52,-1, 4,-1,73,52, 1,-1,-1, 0}}}
};
const BaseMatrix* base(int length,int num,int den){
    for(const auto& m:matrices)if(m.length==length&&m.num==num&&m.den==den)return &m;
    return nullptr;
}
// Edges of one base matrix row: (column block, shift).
struct Row { std::vector<std::pair<int,int>> edges; };
std::vector<Row> rows_of(const BaseMatrix& m){
    std::vector<Row> rows(size_t(m.rows));
    for(int r=0;r<m.rows;++r)for(int c=0;c<24;++c)if(m.h[r][c]>=0)rows[size_t(r)].edges.push_back({c,m.h[r][c]});
    return rows;
}
// Share of each total over the codewords: the first (total mod n) get one more.
long share(long total,int n,int i){return total/n+(i<total%n?1:0);}
// Expanded parity checks: block shift P^s connects row i of the block to
// column (i + s) mod Z. Check c (block row r, row i) owns the bit indices
// var[start[c] .. start[c+1]), in edge order; built once per code.
struct Graph { int z=0; std::vector<int> start,var; };
const Graph& graph(const BaseMatrix& m){
    static const auto graphs=[]{
        std::array<Graph,std::size(matrices)> g;
        for(size_t n=0;n<std::size(matrices);++n){
            const auto& b=matrices[n];const int z=b.length/24;g[n].z=z;g[n].start.push_back(0);
            for(const auto& row:rows_of(b))for(int i=0;i<z;++i){
                for(auto [col,s]:row.edges)g[n].var.push_back(col*z+(i+s)%z);
                g[n].start.push_back(int(g[n].var.size()));
            }
        }
        return g;
    }();
    return graphs[size_t(&m-matrices)];
}
// Number of unmet parity checks (stops counting at `limit`).
template<class Bit> int unmet(const Graph& g,const Bit& bit,int limit){
    int n=0;
    for(size_t c=0;c+1<g.start.size()&&n<limit;++c){unsigned x=0;for(int e=g.start[c];e<g.start[c+1];++e)x^=unsigned(bit(g.var[size_t(e)]));n+=int(x);}
    return n;
}
template<class Bit> bool parity_ok(const Graph& g,const Bit& bit){return unmet(g,bit,1)==0;}
// Layered normalised min-sum (0.75) on one codeword; llr positive = 0 here.
// Gives up once the count of unmet checks has not improved for 5 iterations:
// on live 802.11ac traffic that cut decode time by a quarter and lost 4 of
// 1591 FCS-valid PPDUs.
bool decode_codeword(const Graph& g,std::vector<float>& post,int iterations){
    auto hard=[&](int v){return post[size_t(v)]<0;};
    int best=unmet(g,hard,1<<30),since=0;
    if(!best)return true;
    constexpr int stall=5;
    std::vector<float> msg(g.var.size(),0.f),q(24);
    for(int it=0;it<iterations;++it){
        for(size_t c=0;c+1<g.start.size();++c){
            const int e0=g.start[c],deg=g.start[c+1]-e0;
            float min1=std::numeric_limits<float>::max(),min2=min1;int at=0;bool negative=false;
            for(int j=0;j<deg;++j){
                q[size_t(j)]=post[size_t(g.var[size_t(e0+j)])]-msg[size_t(e0+j)];
                const float a=std::abs(q[size_t(j)]);
                if(a<min1){min2=min1;min1=a;at=j;}else if(a<min2)min2=a;
                negative^=q[size_t(j)]<0;
            }
            for(int j=0;j<deg;++j){
                const float mag=0.75f*(j==at?min2:min1),m=(negative^(q[size_t(j)]<0))?-mag:mag;
                msg[size_t(e0+j)]=m;post[size_t(g.var[size_t(e0+j)])]=q[size_t(j)]+m;
            }
        }
        const int now=unmet(g,hard,1<<30);
        if(!now)return true;
        if(now<best){best=now;since=0;}else if(++since>=stall)return false;
    }
    return false;
}
}
LdpcPlan ldpc_plan(long pld,long avbits,int num,int den,int cbps,int m_stbc,std::optional<bool> extra){
    LdpcPlan p;p.rate_num=num;p.rate_den=den;p.payload_bits=pld;
    const double r=double(num)/den;
    auto fits=[&](double x){return double(avbits)>=double(pld)+x*(1-r);};
    if(avbits<=648){p.codewords=1;p.length=fits(912)?1296:648;}
    else if(avbits<=1296){p.codewords=1;p.length=fits(1464)?1944:1296;}
    else if(avbits<=1944){p.codewords=1;p.length=1944;}
    else if(avbits<=2592){p.codewords=2;p.length=fits(2916)?1944:1296;}
    else{const long k=1944L*num/den;p.codewords=int((pld+k-1)/k);p.length=1944;}
    const long k=long(p.length)*num/den,parity=long(p.length)-k,cw=p.codewords;
    p.shortened=std::max(0L,cw*k-pld);
    long punc=std::max(0L,cw*p.length-avbits-p.shortened);
    const double par=double(cw*parity);
    const bool add=extra?*extra:((double(punc)>0.1*par&&double(p.shortened)<1.2*double(punc)*r/(1-r))||double(punc)>0.3*par);
    if(add){avbits+=long(cbps)*m_stbc;punc=std::max(0L,cw*p.length-avbits-p.shortened);}
    p.extra_symbol=add;p.coded_bits=avbits;p.punctured=punc;
    p.repeated=std::max(0L,avbits-cw*parity-pld);
    return p;
}
LdpcPlan ldpc_plan_he(long pld,long avbits_init,long avbits,int num,int den){
    LdpcPlan p=ldpc_plan(pld,avbits_init,num,den,0,1,false);
    const long k=long(p.length)*num/den,parity=long(p.length)-k,cw=p.codewords;
    p.coded_bits=avbits;p.extra_symbol=avbits!=avbits_init;
    p.punctured=std::max(0L,cw*p.length-avbits-p.shortened);
    p.repeated=std::max(0L,avbits-cw*parity-pld);
    return p;
}
int ldpc_decode(const float* llr,const LdpcPlan& p,std::vector<uint8_t>& bits,int iterations){
    bits.clear();
    const BaseMatrix* m=base(p.length,p.rate_num,p.rate_den);
    if(!m||p.codewords<=0)return p.codewords>0?p.codewords:1;
    const Graph& g=graph(*m);
    const long k=long(p.length)*p.rate_num/p.rate_den,parity=long(p.length)-k;
    int failed=0;size_t at=0;
    for(int i=0;i<p.codewords;++i){
        const long ki=k-share(p.shortened,p.codewords,i),sent_parity=parity-share(p.punctured,p.codewords,i),sent=ki+sent_parity;
        std::vector<float> post(size_t(p.length),0.f);
        // Our soft bits are positive for 1; the decoder's are positive for 0.
        for(long j=0;j<ki;++j)post[size_t(j)]=-llr[at++];
        for(long j=ki;j<k;++j)post[size_t(j)]=1e20f;  // shortened: known zeros
        for(long j=0;j<sent_parity;++j)post[size_t(k+j)]=-llr[at++];  // punctured parity stays 0
        // Repeated bits copy the transmitted codeword from its start.
        for(long j=0,rep=share(p.repeated,p.codewords,i);j<rep;++j){const long t=j%sent;post[size_t(t<ki?t:k+t-ki)]-=llr[at++];}
        if(!decode_codeword(g,post,iterations))++failed;
        for(long j=0;j<ki;++j)bits.push_back(post[size_t(j)]<0);
    }
    return failed;
}
std::vector<uint8_t> ldpc_encode(const std::vector<uint8_t>& payload,const LdpcPlan& p){
    std::vector<uint8_t> out;
    const BaseMatrix* m=base(p.length,p.rate_num,p.rate_den);
    if(!m)return out;
    const auto rows=rows_of(*m);const int z=p.length/24,kb=24-m->rows;
    const long k=long(p.length)*p.rate_num/p.rate_den,parity=long(p.length)-k;
    size_t at=0;
    for(int i=0;i<p.codewords;++i){
        const long ki=k-share(p.shortened,p.codewords,i),sent_parity=parity-share(p.punctured,p.codewords,i);
        std::vector<uint8_t> c(size_t(p.length),0);
        for(long j=0;j<ki;++j)c[size_t(j)]=at<payload.size()?payload[at++]:0;
        // Dual-diagonal parity: lambda_r from the information blocks; the sum
        // of all rows gives p0, then the staircase gives p1, p2, ...
        std::vector<std::vector<uint8_t>> lambda(size_t(m->rows),std::vector<uint8_t>(size_t(z),0));
        for(int r=0;r<m->rows;++r)for(auto [col,s]:rows[size_t(r)].edges)if(col<kb)for(int t=0;t<z;++t)lambda[size_t(r)][size_t(t)]^=c[size_t(col*z+(t+s)%z)];
        std::vector<uint8_t> p0(size_t(z),0);for(const auto& l:lambda)for(int t=0;t<z;++t)p0[size_t(t)]^=l[size_t(t)];
        int middle=-1;for(int r=1;r+1<m->rows;++r)if(m->h[r][kb]>=0)middle=r;
        auto block=[&](int j){return &c[size_t((kb+j)*z)];};
        for(int t=0;t<z;++t)block(0)[t]=p0[size_t(t)];
        for(int t=0;t<z;++t)block(1)[t]=lambda[0][size_t(t)]^p0[size_t((t+m->h[0][kb])%z)];
        for(int r=1;r+1<m->rows;++r)for(int t=0;t<z;++t)block(r+1)[t]=lambda[size_t(r)][size_t(t)]^block(r)[t]^(r==middle?p0[size_t(t)]:0);
        const size_t start=out.size();
        for(long j=0;j<ki;++j)out.push_back(c[size_t(j)]);
        for(long j=0;j<sent_parity;++j)out.push_back(c[size_t(k+j)]);
        const long sent=long(out.size()-start);
        for(long j=0,rep=share(p.repeated,p.codewords,i);j<rep;++j)out.push_back(out[start+size_t(j%sent)]);
        if(!parity_ok(graph(*m),[&](int v){return c[size_t(v)];}))return {};
    }
    return out;
}
}
