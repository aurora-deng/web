#include "ProcessRunner.h"
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
extern char **environ;
namespace webserver::function3 {
ProcessResult runProcess(const std::vector<std::string>&args,std::chrono::seconds timeout,std::size_t memoryMiB,const std::vector<std::string>&env,const std::atomic<bool>*cancel){
    if(args.empty()||args[0].empty())throw std::invalid_argument("executable required");
    std::vector<char*>argv,environment;
    for(auto&s:args)argv.push_back(const_cast<char*>(s.c_str()));argv.push_back(nullptr);
    for(auto&s:env)environment.push_back(const_cast<char*>(s.c_str()));environment.push_back(nullptr);
    int pipes[2];if(pipe2(pipes,O_CLOEXEC)!=0)throw std::runtime_error("worker pipe failed");
    // 在 fork 前读上限；子进程只调用异步信号安全函数，不能遍历目录或分配对象。
    rlimit descriptors{};getrlimit(RLIMIT_NOFILE,&descriptors);
    const auto parent=getpid();const auto pid=fork();
    if(pid<0){close(pipes[0]);close(pipes[1]);throw std::runtime_error("worker fork failed");}
    if(pid==0){
        // 服务器异常退出也不能留下失去监管的模型/媒体进程；处理 fork 与 prctl 间的竞态。
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)!=0||getppid()!=parent)_exit(127);
        // 子进程不调用 C++ 分配器或日志，只设置资源上限并立即 exec。
        close(pipes[0]);dup2(pipes[1],STDOUT_FILENO);dup2(pipes[1],STDERR_FILENO);close(pipes[1]);
        int null=open("/dev/null",O_RDONLY);if(null>=0){dup2(null,STDIN_FILENO);close(null);}
        // 模型进程不能继承数据库、监听或客户端 socket。新内核一次关闭，旧内核逐个关闭。
#ifdef SYS_close_range
        if(syscall(SYS_close_range,3U,~0U,0)!=0)
#endif
            for(rlim_t fd=3;fd<descriptors.rlim_cur;++fd)close(static_cast<int>(fd));
        rlimit memory{memoryMiB*1024ULL*1024,memoryMiB*1024ULL*1024};setrlimit(RLIMIT_AS,&memory);
        rlimit file{32*1024*1024,32*1024*1024};setrlimit(RLIMIT_FSIZE,&file);
        rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);
        rlimit cpu{static_cast<rlim_t>(timeout.count()+1),static_cast<rlim_t>(timeout.count()+2)};setrlimit(RLIMIT_CPU,&cpu);
        execve(argv[0],argv.data(),env.empty()?environ:environment.data());_exit(127);
    }
    close(pipes[1]);fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    ProcessResult result;int status=0;const auto deadline=std::chrono::steady_clock::now()+timeout;
    auto drain=[&]{char data[4096];for(;;){auto n=read(pipes[0],data,sizeof(data));if(n<=0)break;if(result.output.size()<32768)result.output.append(data,std::min<std::size_t>(n,32768-result.output.size()));}};
    for(;;){drain();const auto done=waitpid(pid,&status,WNOHANG);if(done==pid)break;if(done<0&&errno!=EINTR){close(pipes[0]);throw std::runtime_error("worker wait failed");}
        result.cancelled=cancel&&cancel->load(std::memory_order_relaxed);
        if(result.cancelled||std::chrono::steady_clock::now()>=deadline){result.timedOut=!result.cancelled;kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain();close(pipes[0]);result.status=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);return result;
}
}
