#include "wifi_enrolment.hpp"
#include <filesystem>
#include <iostream>
using namespace rfmon::wifi_master;
int main(int argc,char**argv){
 if(argc!=3 || std::filesystem::exists(argv[2]))return 2;
 std::filesystem::copy(argv[1],argv[2],std::filesystem::copy_options::recursive);
 std::string key; int failures=0;
 auto check=[&](bool b,const char* s){std::cout<<(b?"PASS ":"FAIL ")<<s<<'\n';failures+=!b;};
 {
 WifiEnrolment e(argv[2]);
 for(auto&r:e.snapshot())if(r.enrolment)for(auto&p:r.enrolment->profiles)if(p.captures>=8&&p.last_ts-p.first_ts>=10)key=r.device_key;
 if(key.empty()){std::cout<<"No eligible live profile\n";return 3;}
 auto row=[&](){for(auto&r:e.snapshot())if(r.device_key==key)return r;throw std::runtime_error("missing");};
 auto cmd=[&](const char*a){check(e.command({key,a,"Isolated test only","Not a trusted real-world identity"}),"command queued");e.flush();};
 check(row().enrolment->policy=="unreviewed","live discovery starts unreviewed");
 cmd("reference");check(row().enrolment->reference_version==0,"unapproved live identity cannot acquire reference");
 cmd("acknowledge");check(row().enrolment->policy=="acknowledged","acknowledgement does not approve");
 cmd("approve");cmd("reference");check(row().enrolment->reference_version==1,"eligible live RF profile accepts explicit test reference");
 cmd("note");cmd("disallow");check(row().enrolment->policy=="disallowed","disallow policy saved");
 WifiEnrolment second(argv[2]);check(!second.health().error.empty(),"second writer blocked");
 check(e.health().error.empty(),"primary store healthy");
 }
 {
 WifiEnrolment e(argv[2]);for(auto&r:e.snapshot())if(r.device_key==key){check(r.enrolment->policy=="disallowed","policy survives reopen");check(r.enrolment->reference_version==1,"reference survives reopen");check(r.enrolment->alias=="Isolated test only","alias survives reopen");}
 }
 return failures?1:0;
}
