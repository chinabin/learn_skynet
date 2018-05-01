#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

/*
 添加系统定时器消息，启动一个自主逻辑。
*/
int skynet_timeout(uint32_t handle, int time, int session);
/*
 定时器更新，10毫秒为一个嘀嗒
*/
void skynet_updatetime(void);
/*
 系统开机到现在的嘀嗒数，单位是 10 毫秒
*/
uint32_t skynet_gettime(void);
uint32_t skynet_gettime_fixsec(void);

void skynet_timer_init(void);

#endif
