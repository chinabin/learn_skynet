#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include "skynet_system.h"
#include <stdint.h>

/*
 添加系统定时器消息( source 等于 -1 )，其实就是自己给自己发消息
 当 message 的 data 不为空的时候， sz 表示 data 大小。
 当 message 的 data 为空的时候， sz 表示两方通信的一个简单约定。
*/
void skynet_timeout(int handle, int time, int session);
// 更新计时器，执行刷新函数
void skynet_updatetime(void);
// 计时器，系统开机到现在的秒数，单位是 10 毫秒
uint32_t skynet_gettime(void);

// 创建全局timer TI并初始化
void skynet_timer_init(void);

#endif
