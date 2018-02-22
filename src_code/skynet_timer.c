 #include "skynet_timer.h"
#include "skynet_mq.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)	//256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)	//64
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_node {
	struct timer_node *next;
	int expire;		//到期秒数
};

struct link_list {
	struct timer_node head;			//头部， head->next 指向第一个节点
	struct timer_node *tail;		//尾部，插入操作发生在尾部
};

struct timer {
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL-1];
	int lock;
	int time;				// skynet 的滴答数，最开始是基于 current ，之后由 skynet 自己控制增加
	uint32_t current;		//系统开机到现在的秒数，单位是100毫秒
};

static struct timer * TI = NULL;

//清楚链表。使得 tail 指向 head ， head 的 next 指向空
//与 link 函数紧密结合。确保 head->next 指向的是第一个节点， tail->next 指向空
static inline struct timer_node *
link_clear(struct link_list *list)
{
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

static inline void
link(struct link_list *list,struct timer_node *node)
{
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

//将 timer_node 添加到指定的 timer 中
//需要根据将要添加的 timer_node 的 expire 来挑选位置
static void
add_node(struct timer *T,struct timer_node *node)
{
	int time=node->expire;
	int current_time=T->time;
	
	//表示 time 和 current_time 两个值只在低 TIME_NEAR_MASK 位中
	//也就是满足条件设立的精度
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	}
	else {
		//这里也是比较差距，只是精度越来越大
		//最开始是	1111 1111 1111 11
		//然后变成	1111 1111 1111 1111 1111
		//			1111 1111 1111 1111 1111 1111 11
		//			1111 1111 1111 1111 1111 1111 1111 1111
		int i;
		int mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}
		//(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)获取了右移的位数
		//(time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT))将要检查的字节送到嘴巴边和TIME_LEVEL_MASK做与操作，其结果就是二维数组的第二个索引
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)-1],node);	
	}
}

// sz 表示数据大小， arg 是数据， time 是过期秒数
// arg 是 skynet_message
static void
timer_add(struct timer *T,void *arg,size_t sz,int time)
{
	struct timer_node *node = (struct timer_node *)malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);

	while (__sync_lock_test_and_set(&T->lock,1)) {};

		node->expire=time+T->time;
		add_node(T,node);

	__sync_lock_release(&T->lock);
}

static void 
timer_execute(struct timer *T)
{
	while (__sync_lock_test_and_set(&T->lock,1)) {};
	int idx=T->time & TIME_NEAR_MASK;
	struct timer_node *current;
	int mask,i,time;
	
	while (T->near[idx].head.next) {
		current=link_clear(&T->near[idx]);
		
		do {
			struct timer_node *temp=current;
			skynet_mq_push((struct skynet_message *)(temp+1));
			current=current->next;
			free(temp);	
		} while (current);
	}
	
	++T->time;
	
	mask = TIME_NEAR;
	time = T->time >> TIME_NEAR_SHIFT;
	i=0;
	
	while ((T->time & (mask-1))==0) {
		idx=time & TIME_LEVEL_MASK;
		if (idx!=0) {
			--idx;
			current=link_clear(&T->t[i][idx]);
			while (current) {
				struct timer_node *temp=current->next;
				add_node(T,current);
				current=temp;
			}
			break;				
		}
		mask <<= TIME_LEVEL_SHIFT;
		time >>= TIME_LEVEL_SHIFT;
		++i;
	}	
	__sync_lock_release(&T->lock);
}

static struct timer *
timer_create_timer()
{
	struct timer *r=(struct timer *)malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL-1;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	r->lock = 0;
	r->current = 0;

	return r;
}

// session 作为一个整数传递，暂时并无太大意义。
/*
当 message 的 data 不为空的时候， sz 表示 data 大小
*/
void 
skynet_timeout(int handle, int time, int session) {
	struct skynet_message message;
	message.source = -1;
	message.destination = handle;
	message.data = NULL;
	message.sz = (size_t) session;
	// time 为0属于特例，不进入 timer 队列，而是直接进入消息队列
	if (time == 0) {
		skynet_mq_push(&message);
	} else {
		timer_add(TI, &message, sizeof(message), time);
	}
}

//计算系统开机到现在的秒数，单位是10豪秒
static uint32_t
_gettime(void) {
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);	//返回的是系统启动到现在的秒数和纳秒数
	uint32_t t = (uint32_t)(ti.tv_sec & 0xffffff) * 100;	//秒数乘以 1000 等于毫秒数，那么乘以 100 就是10毫秒数
	t += ti.tv_nsec / 10000000;	//本来应该是 纳秒 / 1000 000 000 * 100，也就是先转成秒，再乘以100

	return t;	//返回的时间单位是 10毫秒
}

void
skynet_updatetime(void) {
	uint32_t ct = _gettime();
	if (ct > TI->current) {
		int diff = ct-TI->current;
		TI->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			timer_execute(TI);
		}
	}
}

//上一次执行 skynet_updatetime 的时间
//时间计算是从系统开机开始计算的秒数，单位是10豪秒
uint32_t 
skynet_gettime(void) {
	return TI->current;
}

//创建全局timer TI并初始化
void 
skynet_timer_init(void) {
	TI = timer_create_timer();
	TI->current = _gettime();
}
