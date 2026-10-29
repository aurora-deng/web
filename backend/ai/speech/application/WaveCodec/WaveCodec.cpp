#include "WaveCodec.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
namespace nebula::speech {
namespace{
std::uint32_t read(const std::vector<unsigned char>&v,std::size_t at,unsigned bytes){std::uint32_t value=0;for(unsigned i=0;i<bytes;i++)value|=std::uint32_t(v[at+i])<<(8*i);return value;}
void put(std::vector<unsigned char>&v,std::uint32_t n,unsigned bytes){for(unsigned i=0;i<bytes;i++)v.push_back(static_cast<unsigned char>(n>>(8*i)));}
void text(std::vector<unsigned char>&v,const char*s){v.insert(v.end(),s,s+4);}
}
std::vector<unsigned char>WaveCodec::base64(std::string_view value){
    if(value.empty()||value.size()%4||value.size()>4*1024*1024)throw std::invalid_argument("invalid bounded audio base64");
    const auto code=[](char c)->unsigned{if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;throw std::invalid_argument("invalid base64 character");};std::vector<unsigned char>out;out.reserve(value.size()/4*3);
    for(std::size_t i=0;i<value.size();i+=4){const auto a=code(value[i]),b=code(value[i+1]);const bool pad2=value[i+2]=='=',pad3=value[i+3]=='=';if((pad2&&!pad3)||((pad2||pad3)&&i+4!=value.size()))throw std::invalid_argument("invalid base64 padding");const auto c=pad2?0:code(value[i+2]),d=pad3?0:code(value[i+3]);if((pad2&&(b&15))||(pad3&&!pad2&&(c&3)))throw std::invalid_argument("noncanonical base64 tail");const auto n=(a<<18)|(b<<12)|(c<<6)|d;out.push_back(n>>16);if(!pad2)out.push_back(n>>8);if(!pad3)out.push_back(n);}
    return out;
}
std::vector<unsigned char>WaveCodec::pcm24k(std::vector<unsigned char>pcm){if(pcm.empty()||pcm.size()%2||pcm.size()>24000*2*60)throw std::invalid_argument("PCM exceeds one minute mono 16-bit bound");std::vector<unsigned char>v;v.reserve(pcm.size()+44);text(v,"RIFF");put(v,pcm.size()+36,4);text(v,"WAVE");text(v,"fmt ");put(v,16,4);put(v,1,2);put(v,1,2);put(v,24000,4);put(v,48000,4);put(v,2,2);put(v,16,2);text(v,"data");put(v,pcm.size(),4);v.insert(v.end(),pcm.begin(),pcm.end());return v;}
bool WaveCodec::valid(const std::vector<unsigned char>&v){
    if(v.size()<44||v.size()>6*1024*1024||std::memcmp(v.data(),"RIFF",4)||std::memcmp(v.data()+8,"WAVE",4)||read(v,4,4)+8!=v.size())return false;
    std::uint32_t rate=0,align=0;bool format=false,data=false;std::size_t at=12;unsigned chunks=0;
    while(at+8<=v.size()&&++chunks<=16){const auto size=read(v,at+4,4);if(size>v.size()-at-8)return false;const auto payload=at+8;
        if(!std::memcmp(v.data()+at,"fmt ",4)){if(size<16||read(v,payload,2)!=1||read(v,payload+2,2)!=1||read(v,payload+14,2)!=16)return false;rate=read(v,payload+4,4);align=read(v,payload+12,2);if(rate<16000||rate>48000||align!=2||read(v,payload+8,4)!=rate*align)return false;format=true;}
        if(!std::memcmp(v.data()+at,"data",4)){if(!format||!size||size%align||size>rate*align*60)return false;data=true;}
        at=payload+size+(size&1);if(at>v.size())return false;
    }return format&&data&&at==v.size();
}
}
