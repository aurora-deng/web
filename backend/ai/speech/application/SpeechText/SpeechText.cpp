#include "SpeechText.h"
#include <regex>
#include <stdexcept>
namespace nebula::speech {
namespace{
void replace(std::string&text,std::string_view from,std::string_view to){std::size_t at=0;while((at=text.find(from,at))!=std::string::npos){text.replace(at,from.size(),to);at+=to.size();}}
std::string chinese(unsigned value){const std::string digits[]={"零","一","二","三","四","五","六","七","八","九"};if(value<10)return digits[value];if(value<20)return "十"+(value%10?digits[value%10]:"");return digits[value/10]+"十"+(value%10?digits[value%10]:"");}
void utf8(std::string_view text){
    for(std::size_t i=0;i<text.size();){const auto lead=static_cast<unsigned char>(text[i]);unsigned n=lead<0x80?1:lead>=0xc2&&lead<=0xdf?2:lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:0;
        if(!n||i+n>text.size())throw std::invalid_argument("invalid UTF8 speech text");
        unsigned point=lead&((1u<<(7-n))-1);if(n==1)point=lead;
        for(unsigned j=1;j<n;++j){const auto byte=static_cast<unsigned char>(text[i+j]);if((byte&0xc0)!=0x80)throw std::invalid_argument("invalid UTF8 speech text");point=(point<<6)|(byte&63);}
        if((n==2&&point<0x80)||(n==3&&point<0x800)||(n==4&&point<0x10000)||point>0x10ffff||(point>=0xd800&&point<=0xdfff)||(point<32&&point!=9&&point!=10&&point!=13))throw std::invalid_argument("invalid speech character");i+=n;
    }
}
}
std::string SpeechText::normalize(std::string text){
    if(text.empty()||text.size()>4096||text.find('\0')!=std::string::npos)throw std::invalid_argument("speech text must contain 1..4096 bytes");
    utf8(text);
    replace(text,"\r\n","\n");replace(text,"\r","\n");replace(text,"→","，");replace(text,"｜","，");replace(text,"…","。" );replace(text,"HTTP/2","HTTP 二");replace(text,"HTTP/1.1","HTTP 一点一");replace(text,"\n","。 ");
    std::regex ordinal("第([0-9]{1,2})名");std::string result;std::smatch match;
    while(std::regex_search(text,match,ordinal)){result+=match.prefix();result+="第"+chinese(static_cast<unsigned>(std::stoul(match[1].str())))+"名";text=match.suffix();}result+=text;
    if(result.size()>8192)throw std::invalid_argument("normalized speech text exceeds limit");return result;
}
std::vector<std::string>SpeechText::segments(std::string text){text=normalize(std::move(text));std::vector<std::string>result;std::string current;std::size_t at=0;
    while(at<text.size()){
        const auto c=static_cast<unsigned char>(text[at]);const auto length=c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:0;
        if(!length||at+length>text.size())throw std::invalid_argument("invalid UTF8 speech text");const auto piece=text.substr(at,length);at+=length;current+=piece;
        const bool boundary=piece=="。"||piece=="！"||piece=="？"||piece=="；"||piece=="!"||piece=="?"||(current.size()>72&&(piece=="，"||piece==","));
        if(boundary||current.size()>=240){result.push_back(std::move(current));current.clear();if(result.size()>32)throw std::invalid_argument("speech has too many phrases");}
    }if(!current.empty())result.push_back(std::move(current));if(result.size()>32)throw std::invalid_argument("speech has too many phrases");return result;
}
}
