#pragma once
#include "backend/application/modules/tree/ports/ITreeMediaProcessor/ITreeMediaProcessor.h"
#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/business/BusinessInterfaces/BusinessInterfaces.h"
namespace webserver::function3 {
class TreeMediaService final {
public:
    TreeMediaService(TreeService&tree,phase11::IFileBusiness&files,IUnitOfWork&work,ITreeMediaProcessor&processor,ITreeMediaRepository&repository):tree_(tree),files_(files),work_(work),processor_(processor),repository_(repository){}
    phase11::FileObject create(Id actor,std::string name,std::string mime,std::uint64_t bytes,phase11::TimePoint now);
    ValidatedMedia validate(Id actor,Id file);
    ValidatedMedia download(Id actor,Id post,Id file)const;
private:TreeService&tree_;phase11::IFileBusiness&files_;IUnitOfWork&work_;ITreeMediaProcessor&processor_;ITreeMediaRepository&repository_;
};
}
