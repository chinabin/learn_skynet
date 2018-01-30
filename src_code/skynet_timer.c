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
	int expire;
};

struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};

struct timer {
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL-1];
	int lock;
	int time;
	uint32_t current;		//系统开机到现在的秒数，单位是100毫秒
};

static struct timer * TI = NULL;

//重置尾指针为头指针，并且将头指针的next指针置空。返回头指针的next指针。
static inline struct timer_node *
link_clear(struct link_list *list)
{
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//将timer_node节点添加到链表
static inline void
link(struct link_list *list,struct timer_node *node)
{
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

//将timer_node添加到指定的timer中合适的链表
static void
add_node(struct timer *T,struct timer_node *node)
{
	int time=node->expire;
	int current_time=T->time;
	
	//表示time和current_time的差距只在TIME_NEAR_MASK所标识的位中
	//也就是满足条件设立的精度
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	}
	else {
		//这里也是比较差距，只是精度越来越大
		//最开始是1111 1111 1111 11
		//然后变成1111 1111 1111 1111 1111
		//		  1111 1111 1111 1111 1111 1111 11
		//		  1111 1111 1111 1111 1111 1111 1111 1111
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

//往指定的timer里面添加定时器消息和参数
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

//更新timer里面的node，时间到了就被推送，不然就会更新位置到近一点的地方
static void 
timer_execute(struct timer *T)
{
	while (__sync_lock_test_and_set(&T->lock,1)) {};
	int idx=T->time & TIME_NEAR_MASK;
	struct timer_node *current;
	int mask,i,time;
	
	//将near数组中的某条链表处理了
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
	
	//将另外四个等级的某一等级的某条链表中的节点重新插入到合适的位置
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

//创建一个timer
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

void 
skynet_timeout(int handle, int time, int session) {
	struct skynet_message message;
	message.source = -1;
	message.destination = handle;
	message.data = NULL;
	message.sz = (size_t) session;
	if (time == 0) {
		skynet_mq_push(&message);
	} else {
		timer_add(TI, &message, sizeof(message), time);
	}
}

//获取系统开机到现在的秒数，单位是100豪秒
static uint32_t
_gettime(void) {
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);	//返回的是系统启动到现在的秒数和纳秒数
	uint32_t t = (uint32_t)(ti.tv_sec & 0xffffff) * 100;
	t += ti.tv_nsec / 10000000;	//本来应该是 纳秒 / 1000 000 000 * 100，也就是先转成秒，再乘以100

	return t;	//返回的时间单位是 100毫秒
}

//取两次获取_gettime的差值，然后执行timer_execute
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

//获取系统开机到现在的秒数，单位是100豪秒
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
