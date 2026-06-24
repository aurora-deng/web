#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H
#include<mutex>
#include<memory>
#include<vector>

#include"server/http/http.h"
// 使用模版池化函数
template<class T>
class ObjectPoll
{
private:
    std::mutex mtx;
    std::vector<std::unique_ptr<T>> freeList;
public:
    T* acquire(){
        std::lock_guard lock(mtx);
        if(freeList.empty())
        {
            return new T();
        }
        auto p=
        std::move(
            freeList.back()
        );

        freeList.pop_back();
        p->reset();
        return p.release();
    }
    void release(T* obj){
        obj->reset();
        std::lock_guard lock(mtx);
        freeList.emplace_back(obj);
    }
};



#endif
