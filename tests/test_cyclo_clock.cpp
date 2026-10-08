#include "cyclostationary/clock_refinement.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace rfmon::cyclo;
using IQ=std::vector<std::complex<float>>;
namespace {
constexpr double pi=3.14159265358979323846,rate=20e6;
void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
IQ noise(unsigned seed,std::size_t n=32768){std::mt19937 rng(seed);std::normal_distribution<float>d;IQ x(n);for(auto& z:x)z={d(rng),d(rng)};return x;}
IQ cp(unsigned seed,std::size_t lag=96,std::size_t prefix=24){const auto source=noise(seed);IQ x;std::size_t j=0;
 while(x.size()<source.size()){IQ symbol(source.begin()+j,source.begin()+j+lag);x.insert(x.end(),symbol.end()-prefix,symbol.end());x.insert(x.end(),symbol.begin(),symbol.end());j+=lag;}x.resize(source.size());return x;}
IQ barker(unsigned seed){std::mt19937 rng(seed);constexpr int code[]{1,1,1,-1,-1,-1,1,-1,-1,1,-1};IQ x(32768);int sign=1;
 for(std::size_t j=0;j<x.size();++j){if(j%88==0)sign=rng()%2?1:-1;x[j]=float(sign*code[(j/8)%11])*std::polar(1.f,float(2*pi*.0175*j));}return x;}
IQ warp(const IQ& x,double factor){IQ y(x.size());for(std::size_t j=0;j<y.size();++j){double at=j*factor;auto k=std::size_t(at);if(k+1<x.size())y[j]=float(1-(at-k))*x[k]+float(at-k)*x[k+1];}return y;}
using CD=std::complex<double>;
std::vector<CD> oracle_partition(const IQ& x,const ClockRefinement& r,std::size_t origin,std::size_t grid) {
 std::vector<CD> y(r.partition_samples);CD mean{};const auto factor=timing_grid_scale(grid);
 for(std::size_t j=0;j<y.size();++j){const double t=16+factor*(origin+j);const auto k=std::size_t(t);const double f=t-k;
  check(k+1<x.size(),"oracle interpolation escapes source");y[j]=CD(x[k])*(1-f)+CD(x[k+1])*f;mean+=y[j];}
 mean/=y.size();for(auto& z:y)z-=mean;return y;
}
double coh(CD sum,double a,double b){return a>0 && b>0?std::norm(sum)/(a*b):0;}
void oracle_cp(const IQ& x,const ClockRefinement& r,const RefinedCp& p) {
 const auto& m=p.measurement;const auto period=m.useful_samples+m.prefix_samples;
 for(bool held:{false,true}) {
  const auto origin=held?r.holdout_offset:0;const auto y=oracle_partition(x,r,origin,p.grid_index);
  const auto count=(y.size()-m.useful_samples)/period*period;
  CD inside{},outside{},cyclic{};double ia=0,ib=0,oa=0,ob=0,ta=0,tb=0;
  for(std::size_t j=0;j<count;++j){const auto a=y[j+m.useful_samples],b=y[j],cross=a*std::conj(b);const auto phase=(origin+j)%period;
   if((phase+period-m.phase_samples)%period<m.prefix_samples){inside+=cross;ia+=std::norm(a);ib+=std::norm(b);}
   else{outside+=cross;oa+=std::norm(a);ob+=std::norm(b);}
   cyclic+=cross*std::polar(1.,-2*pi*phase/period);ta+=std::norm(a);tb+=std::norm(b);}
  check(std::abs(coh(inside,ia,ib)-(held?m.holdout_prefix:m.train_prefix))<1e-6 &&
   std::abs(coh(outside,oa,ob)-(held?m.holdout_outside:m.train_outside))<1e-6,"retimed CP differs from direct source-pair oracle");
  if(held)check(std::abs(coh(cyclic,ta,tb)-m.holdout_cyclic)<1e-6,"retimed cyclic support differs from direct oracle");
 }
}
void oracle_code(const IQ& x,const ClockRefinement& r,const RefinedCode& p) {
 constexpr int code[]{1,1,1,-1,-1,-1,1,-1,-1,1,-1};const auto& m=p.measurement;
 for(bool held:{false,true}) {
  const auto origin=held?r.holdout_offset:0;const auto y=oracle_partition(x,r,origin,p.grid_index);
  double sums[11]{};std::size_t counts[11]{};const auto phase=m.code_phase_samples%m.chip_samples;
  const auto first=(phase+m.chip_samples-origin%m.chip_samples)%m.chip_samples;
  for(std::size_t j=first;j+11*m.chip_samples<=y.size();j+=m.chip_samples){CD word{};double power=0;
   for(std::size_t c=0;c<11;++c){CD chip{};
    for(std::size_t k=0;k<m.chip_samples;++k){const auto at=j+c*m.chip_samples+k;chip+=y[at]*std::polar(1.,-2*pi*m.carrier_hz/rate*(origin+at));}
    chip/=m.chip_samples;word+=double(code[c])*chip;power+=std::norm(chip);}
   if(power<=1e-18)continue;const auto bucket=((origin+j)/m.chip_samples)%11;sums[bucket]+=std::norm(word)/(11*power);++counts[bucket];}
  const auto chosen=m.code_phase_samples/m.chip_samples;double competitor=0;
  for(std::size_t k=0;k<11;++k){if(counts[k])sums[k]/=counts[k];if(k!=chosen)competitor=std::max(competitor,sums[k]);}
  check(counts[chosen]==(held?m.holdout_words:m.train_words) &&
   std::abs(sums[chosen]-(held?m.holdout_code_coherence_squared:m.train_code_coherence_squared))<1e-6 &&
   std::abs(competitor-(held?m.holdout_other_phase:m.train_other_phase))<1e-6,"retimed code differs from direct chip/word oracle");
 }
}
template<class F>void reject(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid timing input accepted");}
bool cp_match(const ClockRefinement& r){return !r.cp.empty() && r.cp[0].measurement.pattern_consistent;}
bool code_match(const ClockRefinement& r){return !r.code.empty() && r.code[0].measurement.pattern_consistent;}
}
int main(){try {
 for(const auto kind:{0,1})for(int ppm:{-2000,0,2000}) {
  auto x=warp(kind?barker(101008):cp(101008),1+ppm*1e-6);const auto before=x;
  const auto raw=discover_waveform_structure(x,rate);const auto r=refine_structure_clock(x,rate,raw);
  std::cout<<(kind?"code":"cp")<<" ppm "<<ppm<<" candidates "<<r.cp.size()<<' '<<r.code.size();
  if(!r.cp.empty())std::cout<<" CP "<<timing_grid_ppm[r.cp[0].grid_index]<<" "<<r.cp[0].measurement.useful_samples<<'+'<<r.cp[0].measurement.prefix_samples<<" "<<r.cp[0].measurement.train_prefix<<" "<<r.cp[0].measurement.holdout_prefix;
  if(!r.code.empty())std::cout<<" code "<<timing_grid_ppm[r.code[0].grid_index]<<" "<<r.code[0].measurement.chip_samples<<" "<<r.code[0].measurement.train_code_coherence_squared<<" "<<r.code[0].measurement.holdout_code_coherence_squared;
  std::cout<<'\n';
  check(kind?code_match(r):cp_match(r),"clock-distorted structure not recovered");
  for(const auto& p:r.cp)oracle_cp(x,r,p);for(const auto& p:r.code)oracle_code(x,r,p);
  auto changed=x;const auto replacement=noise(102008);
  std::copy(replacement.begin()+x.size()/2,replacement.end(),changed.begin()+x.size()/2);
  const auto frozen=refine_structure_clock(changed,rate,raw);
  check(r.cp.size()==frozen.cp.size() && r.code.size()==frozen.code.size(),"held data changes discovery selection count");
  if(!r.cp.empty())check(r.cp[0].grid_index==frozen.cp[0].grid_index && r.cp[0].seed_index==frozen.cp[0].seed_index &&
   r.cp[0].measurement.phase_samples==frozen.cp[0].measurement.phase_samples && !cp_match(frozen),"held data refits CP grid/phase or passes noise");
  if(!r.code.empty())check(r.code[0].grid_index==frozen.code[0].grid_index && r.code[0].seed_index==frozen.code[0].seed_index &&
   r.code[0].measurement.code_phase_samples==frozen.code[0].measurement.code_phase_samples && !code_match(frozen),"held data refits code grid/phase or passes noise");
  check(x==before && r.grids_tested==9 && r.cp_trials<=18 && r.code_trials<=18 && r.interpolated_samples<=11*r.partition_samples,"timing resource budget or source preservation failed");
 }
 for(unsigned seed=0;seed<16;++seed)for(float rho:{0.f,.975f}) {
  auto x=noise(103008+seed);std::complex<float> prev{};for(auto& z:x){prev=rho*prev+z;z=prev;}
  const auto r=refine_structure_clock(x,rate,discover_waveform_structure(x,rate));
  check(!cp_match(r) && !code_match(r),"Gaussian timing search passes development controls");
 }
 auto x=warp(barker(104008),1.002);const auto raw=discover_waveform_structure(x,rate);const auto reference=refine_structure_clock(x,rate,raw);
 for(float gain:{1e-25f,1e25f}) {auto y=x;for(auto& z:y)z*=gain;const auto a=refine_structure_clock(y,rate,raw);
  check(a.code[0].grid_index==reference.code[0].grid_index && std::abs(a.code[0].measurement.holdout_code_coherence_squared-reference.code[0].measurement.holdout_code_coherence_squared)<1e-6,"timing gain invariance fails");}
 auto short_iq=noise(105008,2064);const auto short_raw=discover_waveform_structure(short_iq,rate);
 check(refine_structure_clock(short_iq,rate,short_raw).status=="insufficient_resampling_support","short timing input padded or missing margin abstention");
 IQ zero(32768);check(refine_structure_clock(zero,rate,discover_waveform_structure(zero,rate)).status=="no_variation","zero timing input does not abstain");
 reject([&]{refine_structure_clock(x,0,raw);});reject([&]{timing_grid_scale(9);});
 auto bad=raw;bad.partition_samples/=2;reject([&]{refine_structure_clock(x,rate,bad);});
 bad=raw;bad.spread[0].chip_samples=17;reject([&]{refine_structure_clock(x,rate,bad);});
 x.back()={NAN,0};reject([&]{refine_structure_clock(x,rate,raw);});
 std::cout<<"timing drift recovery controls passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
