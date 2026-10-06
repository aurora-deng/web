#pragma once

#include <stdexcept>
#include <string>

namespace webserver::phase11
{

enum class ErrorCode
{
    InvalidArgument,
    NotFound,
    Conflict,
    Unauthenticated,
    Forbidden,
    RateLimited,
    Unavailable
};

class ApplicationError : public std::runtime_error
{
public:
    ApplicationError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code)
    {
    }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace webserver::phase11
