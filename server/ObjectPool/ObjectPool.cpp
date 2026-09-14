// ============================================================
// 文件名：ObjectPool.cpp
// ------------------------------------------------------------
// 【生活比喻：酒店布草间的总台登记本】
// 这个文件本身只做一件事：实例化两个全局对象池——
//   requestPool  ：HttpRequest  的布草柜（请求对象复用）
//   responsePool ：HttpResponse 的布草柜（响应对象复用）
//
// 【布草间 通俗解释】
// 酒店每天有大量客人入住，床单（HttpRequest/HttpResponse）用一次就洗净
// 换给下一位客人，而不是用完就扔、下次再买新的。ObjectPool 就是这个布草间：
// acquire() 领一床干净床单，release() 用完送回洗涤复用。
//
// 为什么要池化 HttpRequest/HttpResponse？
//   这俩对象内部有 vector、string、map 等动态结构，频繁 new/delete
//   会让堆碎片化严重。复用后，它们的内部容器容量也跟着复用，
//   连 vector 的扩容 malloc 都省了，性能提升显著。
// ============================================================

#include"ObjectPool.h"
#include "server/http/http.h"


// 全局布草柜：所有线程共享。模板实例化为 HttpRequest/HttpResponse 两个具体池。
// requestPool  复用 HttpRequest  —— HTTP 请求对象
// responsePool 复用 HttpResponse —— HTTP 响应对象
// 在 HttpSession.cpp 等处通过 acquire/release 循环借用，避免反复 new/delete。
ObjectPoll<HttpRequest> requestPool;
ObjectPoll<HttpResponse> responsePool;
