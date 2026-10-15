#include "drones/remote_id.hpp"
#include <iostream>
int main(){std::string line;while(std::getline(std::cin,line)){try{if(line.size()>16384||line.size()%2)throw std::runtime_error("bad length");std::vector<uint8_t>b;for(size_t i=0;i<line.size();i+=2){auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("invalid hex");};b.push_back((digit(line[i])<<4)|digit(line[i+1]));}std::cout<<drone::parse_wifi_remote_id(b).dump()<<'\n';}catch(...){std::cout<<"[]\n";}}}
