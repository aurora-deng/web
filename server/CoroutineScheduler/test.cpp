#include"CoroutineScheduler.h"
#include"Task.h"
#include<iostream>


        // ————————————————————————————测试协程——————————————————————————————————————
Task test()
{
    std::cout << "A\n";

    co_await std::suspend_always{};

    std::cout << "B\n";
}
int main()
 {

        Task t = test();
        CoroutineScheduler scheduler;
        scheduler.add(t.release());

        scheduler.runReady();

        // std::cout<<"Hello!!!!\n";
        scheduler.runReady();
        return 0;
    }