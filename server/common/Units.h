// =============================================================================
// 文件名：Units.h
// 所属模块：server/common —— 容量单位换算工具
//
// 【职责比喻：厨房量杯换算表】
// 本文件提供两个 constexpr 内联函数 KiB / MiB，把"人类可读的容量数字"换算成
// "字节数"。用法如 KiB(64) 表示 64 KiB = 65536 字节、MiB(1) 表示 1 MiB。
// 用来消除代码里裸写的 1024、1024*1024 这种"魔法数字"——让缓冲区大小、内存水位
// 等阈值一眼能看出物理含义，减少看代码时心算的负担。
//
// 关键技术点（初学者重点理解）：
// 1. 【inline constexpr】两个函数都是 inline constexpr——编译期就能求值，调用处
//    会被直接替换成常量（如 KiB(64) 编译成 65536），运行时零开销，等价于写裸数字。
// 2. 【用 UL 后缀防溢出】1024UL 是 unsigned long 字面量，确保与 std::size_t 相乘时
//    走无符号算术。在 32 位平台 size_t 是 32 位，MiB(8)=8388608 仍在范围内；
//    但 MiB(8192) 等大数需注意平台位宽，避免溢出截断。
// 3. 【为什么不用宏】用 #define KIB(x) ((x)*1024) 是 C 风格做法，但宏没有类型、
//    没有作用域、调试时看不到展开后的值。constexpr 函数有类型安全、可调试、可重载，
//    是 C++ 推荐写法。
// =============================================================================
#pragma once
#ifndef SERVER_COMMON_UNITS_H
#define SERVER_COMMON_UNITS_H

#include <cstddef>

/**
 * @brief 把"KiB 数"换算成字节数
 * @param value 多少 KiB
 * @return 对应的字节数（value * 1024）
 *
 * 【通俗解释】
 * KiB(64) = 65536 字节。用来给缓冲区大小、内存水位等阈值换算，消除裸 1024 魔法数字。
 * 编译期求值，运行时零开销。
 */
inline constexpr std::size_t KiB(std::size_t value)
{
    return value * 1024UL;
}

/**
 * @brief 把"MiB 数"换算成字节数
 * @param value 多少 MiB
 * @return 对应的字节数（value * 1024 * 1024）
 *
 * 【通俗解释】
 * MiB(1) = 1048576 字节，MiB(8) = 8388608 字节。用来给大块内存（如内存池、缓冲池）
 * 容量换算。注意 32 位平台 size_t 只有 32 位，MiB(4096) 就接近上限，更大的数
 * 需在 64 位平台使用。
 */
inline constexpr std::size_t MiB(std::size_t value)
{
    return value * 1024UL * 1024UL;
}

#endif
