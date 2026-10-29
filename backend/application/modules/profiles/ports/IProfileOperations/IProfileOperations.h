#pragma once
#include "backend/application/domain/Domain/Domain.h"
namespace webserver::phase11 {
enum class ProfileWriteResult{Applied,Replay,Conflict,NotFound};
/** 数据库驱动将操作收据和资料更新放在一个事务，不向业务暴露SQL/连接。 */
class IProfileOperations{
public:virtual~IProfileOperations()=default;
    virtual ProfileWriteResult writeProfileOnce(UserId user,std::string operation,std::string displayName,std::string biography)=0;
};
}
