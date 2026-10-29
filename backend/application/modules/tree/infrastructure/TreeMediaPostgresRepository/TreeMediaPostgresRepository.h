#pragma once
#include "backend/application/modules/tree/ports/ITreeMediaProcessor/ITreeMediaProcessor.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class TreeMediaPostgresRepository final:public ITreeMediaRepository {
public:
    explicit TreeMediaPostgresRepository(SqlContext&sql):sql_(sql){}
    void save(const ValidatedMedia&value)override;
    std::optional<ValidatedMedia>get(Id file)const override;
private:SqlContext&sql_;
};
}
