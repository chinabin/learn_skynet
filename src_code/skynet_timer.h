#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

void skynet_timeout(int handle, int time, int session);
void skynet_updatetime(void);
//获取系统开机到现在的秒数，单位是100豪秒
uint32_t skynet_gettime(void);

//创建全局timer TI并初始化
void skynet_timer_init(void);

#endif
