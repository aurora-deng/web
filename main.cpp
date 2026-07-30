#include "server/Runtime/ServerRuntime.h"
#include "server/http/RequestContext/RequestContext.h"
#include "log/logger/logger.h"
#include <unistd.h>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        ServerRuntime server;

        // 请求日志中间件
        server.router().use([](RequestContext &ctx, auto next)
                            {
        LOG_HTTP(ctx.request.method + " " + ctx.request.path);
        next(); });

        // 鉴权中间件
        server.router().use([](RequestContext &ctx, auto next)
                            {
        if (ctx.request.path == "/admin")
        {
            auto it = ctx.request.headers.find("token");
            if (it == ctx.request.headers.end())
            {
                ctx.response->status = 401;
                ctx.response->text("Unauthorized");
                return;
            }
        }
        next(); });

        server.router().GET("/", [](RequestContext &ctx) -> bool
                            {
        ctx.response->html("<h1>hello</h1>");
        return true; });

        server.router().GET("/user/:id", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text(ctx.params.at("id"));
        return true; });

        server.router().GET("/stream1", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        ctx.response->writeChunk("hello");
        ctx.response->writeChunk(" world");
        ctx.response->endChunked();
        return true; });

        // /stream2 的 sleep(1) 在 Executor Worker 线程执行，不会阻塞 Reactor
        server.router().GET("/stream2", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        for (int i = 0; i < 10; i++)
        {
            ctx.response->writeChunk("hello\n");
            sleep(1);
        }
        ctx.response->endChunked();
        return true; });

        server.router().GET("/logo", [](RequestContext &ctx) -> bool
                            {
        const std::string path = "./static/logo.svg";
        ctx.response->setHeader("Content-Type", "image/svg+xml");
        if (!ctx.response->sendfile(path, ctx.request, ctx.request.range))
        {
            ctx.response->status = 404;
            ctx.response->statusText = "Not Found";
            ctx.response->text("Not Found");
        }
        return true; });

        // /slow: 验证 Executor 异步执行，sleep(10) 不阻塞其他连接
        server.router().GET("/slow", [](RequestContext &ctx) -> bool
                            {
        sleep(10);
        ctx.response->text("slow done");
        return true; });

        // /fast: 应在 /slow 期间立即返回
        server.router().GET("/fast", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text("fast");
        return true; });

        server.start();
        return 0;
    }
    catch (const std::exception &ex)
    {
        // bind Address already in use 等：正常退出码，避免 uncaught → terminate → 核心转储。
        std::cerr << "webserver failed to start: " << ex.what() << "\n"
                  << "Hint: Ctrl+Z only suspends the process and keeps port 8080; "
                     "use Ctrl+C / kill -TERM, \nor: fg then Ctrl+C, \n"
                     "or kill $(pgrep -n webserver)\n";
        return 1;
    }
}
