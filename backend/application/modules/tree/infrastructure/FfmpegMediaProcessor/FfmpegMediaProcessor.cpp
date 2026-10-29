#include "backend/application/modules/tree/domain/TreeContentPolicy/TreeContentPolicy.h"
#include "FfmpegMediaProcessor.h"
#include "backend/application/modules/shared/infrastructure/ProcessRunner/ProcessRunner.h"
#include <map>
#include <sstream>
#include <unistd.h>
#include <cmath>
namespace webserver::function3 {
ValidatedMedia FfmpegMediaProcessor::validate(Id file,std::string input,std::uint64_t bytes){
    if(!std::filesystem::exists(prefix_/"bin/ffprobe")||!std::filesystem::exists(prefix_/"bin/ffmpeg"))throw BusinessError(Failure::Busy,"media validator is not installed");
    auto probe=runProcess({(prefix_/"bin/ffprobe").string(),"-v","error","-protocol_whitelist","file,pipe","-show_entries","stream=codec_type,codec_name,width,height:format=duration","-of","default=noprint_wrappers=1",input},std::chrono::seconds(10));
    if(probe.status||probe.timedOut)throw BusinessError(Failure::Invalid,"media could not be decoded");
    std::map<std::string,std::string> fields;std::istringstream lines(probe.output);int streams=0;
    for(std::string line;std::getline(lines,line);){auto equal=line.find('=');if(equal!=std::string::npos){auto key=line.substr(0,equal);if(key=="codec_type")++streams;fields[key]=line.substr(equal+1);}}
    if(streams!=1)throw BusinessError(Failure::Invalid,"exactly one media stream required");
    const bool image=fields["codec_type"]=="video";
    const auto codec=fields["codec_name"];
    if(image && codec!="png"&&codec!="mjpeg"&&codec!="gif"&&codec!="webp")throw BusinessError(Failure::Invalid,"unsupported image codec");
    if(!image && fields["codec_type"]!="audio")throw BusinessError(Failure::Invalid,"image or audio required");
    if(bytes>(image?TreeContentPolicy::imageBytes:TreeContentPolicy::voiceBytes))throw BusinessError(Failure::Invalid,"media size exceeded");
    Milliseconds duration=0;
    if(!image){if(auto i=fields.find("duration");i!=fields.end()&&i->second!="N/A"){const auto seconds=std::stod(i->second);if(!std::isfinite(seconds)||seconds<=0||seconds>TreeContentPolicy::voiceMilliseconds/1000.0)throw BusinessError(Failure::Invalid,"audio must be at most 60 seconds");}}
    else{auto w=std::stoull(fields.at("width")),h=std::stoull(fields.at("height"));if(!w||!h||w>4096||h>4096)throw BusinessError(Failure::Invalid,"image dimensions exceed 4096 pixels");}
    std::filesystem::create_directories(output_);if(std::filesystem::space(output_).available<256*1024*1024)throw BusinessError(Failure::Busy,"media storage reserve reached");const auto destination=output_/(std::to_string(file)+(image?".png":".wav"));
    const auto temporary=destination.string()+"."+std::to_string(getpid())+".partial";
    std::vector<std::string> args{(prefix_/"bin/ffmpeg").string(),"-v","error","-nostdin","-y","-threads","1","-protocol_whitelist","file,pipe","-i",input};
    if(image)args.insert(args.end(),{"-frames:v","1","-map_metadata","-1","-c:v","png","-f","image2"});
    else args.insert(args.end(),{"-map_metadata","-1","-vn","-t","60.1","-ac","1","-ar","24000","-c:a","pcm_s16le","-f","wav"});
    args.push_back(temporary);
    auto decoded=runProcess(args,std::chrono::seconds(20));
    if(decoded.status||decoded.timedOut){std::error_code error;std::filesystem::remove(temporary,error);throw BusinessError(Failure::Invalid,"media validation or conversion failed");}
    if(!image){
        auto measured=runProcess({(prefix_/"bin/ffprobe").string(),"-v","error","-show_entries","format=duration","-of","default=noprint_wrappers=1:nokey=1",temporary},std::chrono::seconds(5));
        double seconds=0;try{seconds=std::stod(measured.output);}catch(...){}
        if(measured.status||!std::isfinite(seconds)||seconds<=0||seconds>TreeContentPolicy::voiceMilliseconds/1000.0){std::filesystem::remove(temporary);throw BusinessError(Failure::Invalid,"decoded audio must be at most 60 seconds");}
        duration=static_cast<Milliseconds>(std::llround(seconds*1000));
    }
    std::filesystem::rename(temporary,destination);
    return {{file,image?MediaKind::Image:MediaKind::Audio,bytes,duration},destination.string(),image?"image/png":"audio/wav"};
}
}
