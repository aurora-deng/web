#include "TimeWheel.h"


TimerWheel::TimerWheel(size_t slotNum, int timeout):timeout(timeout),slotNum(slotNum)
{
    wheel.resize(slotNum);          //根据时间槽设置时间轮
}

void TimerWheel::add(Conn &conn)
{
    remove(conn.fd);                //保证先取出fd
    auto* node=new TimerNode;           //之后获得TimerNode变量

    node->conn=&conn;               //添加conn

    node->slot=(current+timeout)%wheel.size();          //更新槽位
    wheel[node->slot].push_back(node);                  //根据槽位添加节点node

    conn.timer=node;                                    //将node放到时间节点
    nodes[conn.fd]=node;                                //根据fd添加到对应的TimerNode
}

void TimerWheel::remove(int fd)
{
    auto it=nodes.find(fd);
    if(it==nodes.end())return;

    auto* node=it->second;
    node->removed=true;
    delete it->second;
    nodes.erase(it);
}

void TimerWheel::refresh(Conn& conn) {
    add(conn);
}
// 超时处理
void TimerWheel::tick() {
    current=(current+1)%wheel.size();

    auto& slot=wheel[current];                      //获得timernode结点
    for(auto* node:slot)                            //遍历删除节时间槽里面的节点
    {
        if(node->removed||!node)
        {
            delete node;
            continue;
        }

        auto* conn=node->conn;
        // 使用closecb泛化报错输出
        //   if(conn)
        // {
        //     LOG_ERROR("[TIMEOUT] fd="+std::to_string(node->conn->fd)+"\n");
        //     node->removed=true;

        // }
        nodes.erase(conn->fd);
        if(closeCb)
        {
            closeCb(conn->fd);
        }
        delete node;
    }
    slot.clear();
}
void TimerWheel::setCloseCallbace(std::function<void(int)> cb) {
    closeCb=std::move(cb);
};
