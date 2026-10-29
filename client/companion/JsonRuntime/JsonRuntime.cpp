// Boost.JSON 的单一实现单元；不重复链接第三方 .cpp，也不在构建时下载库。
#include <boost/json/src.hpp>
#include <boost/version.hpp>
static_assert(BOOST_VERSION == 109200, "Use the approved Boost 1.92.0 distribution");
