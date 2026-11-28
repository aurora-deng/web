#pragma once
#include "backend/application/modules/shared/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/modules/shared/ports/ITransaction/ITransaction.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IDatabase
{
public:
    virtual ~IDatabase() = default;
    [[nodiscard]] virtual std::unique_ptr<ITransaction> beginTransaction() = 0;
    [[nodiscard]] virtual bool healthy() const noexcept = 0;
};
}
