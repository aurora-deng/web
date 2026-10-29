#pragma once
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
#include <functional>
namespace webserver::function3 {
/** 同一 SQL 语句保留旧事件索引并写入可重试队列，不增加一次中间落盘。 */
class ChatOutboxWriter final:public phase11::PostgresOutboxWriter {
public:
    explicit ChatOutboxWriter(std::function<void()>committed={}):committed_(std::move(committed)){}
    phase11::OutboxEvent append(PGconn*connection,phase11::OutboxEvent event)override;
    void committed()noexcept override{if(committed_)try{committed_();}catch(...){}}
private:std::function<void()>committed_;
};
}
