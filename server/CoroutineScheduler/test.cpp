#include "CoroutineScheduler.h"
#include "Task.h"
#include <iostream>

// ————————————————————————————测试协程——————————————————————————————————————
Task hello()
{
    std::cout << "A\n";

    co_return;
}
int main()
{

    auto t = hello();

    std::cout << "Before\n";

    t.resume();

    std::cout << "After\n";
    return 0;
}