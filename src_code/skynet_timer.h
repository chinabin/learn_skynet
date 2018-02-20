#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

//添加一个定时器， session 可以作为一个约定的数传递
void skynet_timeout(int handle, int time, int session);
void skynet_updatetime(void);
//上一次执行 skynet_updatetime 的时间
//时间计算是从系统开机开始计算的秒数，单位是10豪秒
uint32_t skynet_gettime(void);

//创建全局timer TI并初始化
void skynet_timer_init(void);

#endif
