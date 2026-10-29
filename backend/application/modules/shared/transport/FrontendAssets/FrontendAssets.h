#pragma once
class Router;
namespace webserver::function3 {
/** 固定前端根目录中的 HTML/CSS/JS 自动登记，避免拆分模块后漏配新资源。 */
void registerFrontendAssets(Router &router);
}
