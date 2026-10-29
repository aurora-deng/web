// =============================================================================
// 文件名：test.cpp
// 职责：最小可运行的 C++20 协程测试程序——验证 Task<void> 的创建与 resume 流程
//
// 【生活比喻】
// 这是一个"模拟入职"的小测试：经理（main）叫一个服务员（hello 协程）入职，
// 但因为是 lazy 协程，服务员领了工牌后站在原地不说话；经理喊"上岗"（t.resume()），
// 服务员才说一句 "A"，然后干完活（co_return）。整个流程用来验证 Task 包装是否正确。
//
// 关键技术点（初学者重点理解）：
// 1. 协程函数 hello() 的返回类型是 Task<void>——这是让它成为协程的关键。
//    函数体里出现 co_return; 编译器就会把它编译成协程。
// 2. 调用 hello() 不会立即执行函数体，而是：①分配协程帧；②构造 promise；
//    ③调 promise.get_return_object() 返回 Task；④调 initial_suspend()（返回 suspend_always）。
//    所以函数体里的 std::cout<<"A" 此时尚未执行——这是 lazy 协程的核心特性。
// 3. t.resume() 才真正让协程体开始跑，跑到 co_return; 后停在 final_suspend 处。
// 4. 预期输出顺序：Before → A → After。如果 hello() 是普通函数，A 会先于 Before 打印。
// =============================================================================
#include "CoroutineScheduler.h"
#include "Task.h"
#include <iostream>

// ————————————————————————————测试协程——————————————————————————————————————
/**
 * @brief 测试用协程函数：打印一个 "A" 后返回
 * @return Task<void> 协程任务包装
 *
 * 【通俗解释】
 * 这就是一个最简单的协程：函数体内有 co_return;，返回类型是 Task<void>。
 * 编译器看到这两个特征，就会把 hello 编译成"可挂起/恢复"的协程。
 * 调用 hello() 时函数体不会立刻执行，必须等返回的 Task 被 resume 才会跑。
 */
Task hello()
{
    std::cout << "A\n";

    co_return;
}

/**
 * @brief 主函数：验证 lazy 协程的执行时序
 * @return 程序退出码
 *
 * 【通俗解释】
 * 经理（main）的工作流程：
 *   1. auto t = hello();  // 给服务员入职，但服务员没开始干活（lazy）
 *   2. cout << "Before";  // 经理先说一句"开始前"
 *   3. t.resume();        // 经理叫服务员上岗，服务员才打印 "A"
 *   4. cout << "After";   // 服务员干完了，经理说一句"结束后"
 * 输出顺序：Before → A → After。这能证明协程是 lazy 的——
 * 如果是普通函数，hello() 一调用 "A" 就会先打印，Before 反而在后面。
 */
int main()
{

    auto t = hello();

    std::cout << "Before\n";

    t.resume();

    std::cout << "After\n";
    return 0;
}
