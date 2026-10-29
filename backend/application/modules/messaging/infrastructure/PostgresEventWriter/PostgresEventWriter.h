#pragma once
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class PostgresEventWriter final:public IEventWriter {
public:
    explicit PostgresEventWriter(SqlContext &sql,std::function<void()> committed={}):sql_(sql),committed_(std::move(committed)){}
    void appendEvent(DomainEvent event) override;
private:SqlContext &sql_;std::function<void()>committed_;
};
}
