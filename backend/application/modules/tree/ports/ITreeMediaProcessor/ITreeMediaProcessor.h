#pragma once
#include "backend/application/modules/tree/domain/TreePost/TreePost.h"
namespace webserver::function3 {
struct ValidatedMedia {TreeMedia media;std::string path,mime;};
class ITreeMediaProcessor {
public:virtual ~ITreeMediaProcessor()=default;virtual ValidatedMedia validate(Id file,std::string input,std::uint64_t bytes)=0;
};
class ITreeMediaRepository {
public:virtual ~ITreeMediaRepository()=default;virtual void save(const ValidatedMedia &value)=0;virtual std::optional<ValidatedMedia>get(Id file)const=0;
};
}
