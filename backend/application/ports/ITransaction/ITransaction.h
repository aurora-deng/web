#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class ITransaction
{
public:
    virtual ~ITransaction() = default;
    virtual void commit() = 0;
    virtual void rollback() = 0;
};
}
