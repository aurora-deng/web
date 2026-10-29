#include "TreeMediaService.h"
#include <algorithm>
namespace webserver::function3 {
phase11::FileObject TreeMediaService::create(Id actor,std::string name,std::string mime,std::uint64_t bytes,phase11::TimePoint now){
    if(const auto separator=mime.find(';');separator!=std::string::npos)mime.erase(separator);
    const bool image=mime=="image/png"||mime=="image/jpeg"||mime=="image/gif"||mime=="image/webp";
    const bool audio=mime=="audio/wav"||mime=="audio/x-wav"||mime=="audio/mpeg"||mime=="audio/ogg"||mime=="audio/webm";
    if((!image&&!audio)||!bytes||bytes>(image?5ULL:10ULL)*1024*1024)throw BusinessError(Failure::Invalid,"unsupported tree media or size");
    // 先作为私有、不透明文件上传。只有独立处理器确认类型/时长后才能成为公开果实媒体。
    if(name.size()>245)name.resize(245);
    return files_.createUpload(actor,name+".bin","application/octet-stream",bytes,now);
}
ValidatedMedia TreeMediaService::validate(Id actor,Id file){
    const auto owned=files_.download(actor,file);
    if(owned.file.ownerId!=actor)throw BusinessError(Failure::Forbidden,"media owner required");
    if(auto previous=repository_.get(file))return *previous;
    auto value=processor_.validate(file,owned.storagePath,owned.file.expectedSize);
    work_.transact([&]{repository_.save(value);});return value;
}
ValidatedMedia TreeMediaService::download(Id actor,Id post,Id file)const{
    const auto value=tree_.get(actor,post);
    if(std::none_of(value.media.begin(),value.media.end(),[&](auto&m){return m.file==file;}))throw BusinessError(Failure::NotFound,"media does not belong to visible fruit");
    auto media=repository_.get(file);if(!media)throw BusinessError(Failure::NotFound,"validated media missing");return *media;
}
}
