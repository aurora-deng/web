#pragma once
#include "client/storage/SqliteStore/SqliteStore.h"
#include <boost/json.hpp>
#include <functional>
#include <mutex>
namespace nebula::client {
using ConfirmedActionExecutor=std::function<boost::json::value(const std::string&tool,const boost::json::object&arguments,const std::string&operationId)>;
/** Agent只提出结构化草稿；确认只携带提案ID和摘要，无法临时替换载荷。 */
class ActionService final{
public:
    explicit ActionService(SqliteStore&store);
    boost::json::value propose(const std::string&tool,const boost::json::object&arguments);
    boost::json::value list();
    boost::json::value confirm(const std::string&id,const std::string&digest,ConfirmedActionExecutor execute);
    void cancel(const std::string&id);
    static boost::json::value capabilities();
    static std::optional<std::pair<std::string,boost::json::object>>parseModelCall(const std::string&output);
private:SqliteStore&store_;std::mutex execution_;
};
}
