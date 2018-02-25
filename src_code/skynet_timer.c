#include "skynet_timer.h"
#include "skynet_mq.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)	// 256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)	// 64
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_node {
	struct timer_node *next;
	int expire;		// 到期秒数
};

struct link_list {
	struct timer_node head;			// 头部， head->next 指向第一个节点
	struct timer_node *tail;		// 尾部，插入操作发生在尾部
};

struct timer {
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL-1];
	int lock;				// 自旋锁，防止 timer_add 和 timer_execute 冲突
	int time;				// skynet 的滴答数，最开始是基于 current ，之后由 skynet 自己控制增加
	uint32_t current;		// 系统开机到现在的秒数，单位是10毫秒
};

static struct timer * TI = NULL;

// 清除链表，返回原链表的第一个节点。
static inline struct timer_node *
link_clear(struct link_list *list)
{
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//将 node 插入 list 尾端
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
	
	/*
	(0000 0011 | 0000 1111) == 0000 1111
	(0001 0011 | 0000 1111) == 0001 1111 != 0000 1111
	*/
	//表示 time 和 current_time 两个值只在低 TIME_NEAR_MASK 位中
	//也就是满足条件设立的精度，也就是 2.56 秒内
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {	// (time - current_time) <= TIME_NEAR_MASK
		link(&T->near[time&TIME_NEAR_MASK],node);
	}
	else {
		// 这里也是比较差距，只是精度越来越大
		// mask 是从 TIME_NEAR (1 0000 0000)左移 TIME_LEVEL_SHIFT (6)位变成 1 0000 0000 0000 00
		// 之后每次都是左移 TIME_LEVEL_SHIFT (6)位，比较的值变化如下
		// 1111 1111 1111 11							-- 2 ^ 14 ，163.84秒，大概 2.8 分钟
		// 1111 1111 1111 1111 1111						-- 2 ^ 20 ，10485.76秒，大概 2.9 小时
		// 1111 1111 1111 1111 1111 1111 11				-- 2 ^ 26 ，67108864秒，大概 7.8 天
		// 1111 1111 1111 1111 1111 1111 1111 1111		-- 2 ^ 32 ，4294967296秒，大概 497 天
		int i;
		int mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {	// (time - current_time) <= (mask-1)
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}

		/*
		 二维数组 t 的第一个下标表示的是等级，例如 0 表示容纳的时间是[0, 255]，而 1 表示容纳的时间是[256, 16383]
		 越往后靠，权重越大。上面的循环就是为了找到 time 所属的等级，也就是 i 。
		 第二个下标表示的是位置，也就是说找到组织之后，需要找到自己的位置。
		 time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT) 得到了一个 1 <= X <= TIME_LEVEL_MASK 的值
		 也就是 time 应该进入的链表。
		 之所以要与上 TIME_LEVEL_MASK 是因为如果传入的 time 非常大，例如是一万年，那么右移之后的值还是大于我们预设的最大值，
		 所以做了一个裁剪。
		*/
		int tmp = (time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK;
		link(&T->t[i][tmp-1],node);	
	}
}

// sz 表示数据大小， arg 是数据， time 是过期秒数
// arg 是 skynet_message
static void
timer_add(struct timer *T,void *arg,size_t sz,int time)
{
	struct timer_node *node = (struct timer_node *)malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);

	// __sync_lock_test_and_set(&T->lock,1) 是把地址 &T->lock 指向的值设置为 1 ，并返回地址之前指向的值
	// 假设之前是 0 ，也就是没有加锁，那么首先锁住( T->lock 变为 1 )，然后返回 0 ，使得跳出循环
	// 假设之前是 1 ，也就是锁住了，那么就会一直循环，直到锁打开。
	// 从而达到了自旋锁的效果。
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
			// 定时到期，将消息推到消息队列
			skynet_mq_push((struct skynet_message *)(temp+1));
			current=current->next;
			free(temp);	
		} while (current);
	}
	
	++T->time;
	
	mask = TIME_NEAR;
	time = T->time >> TIME_NEAR_SHIFT;
	i=0;
	
	/*
	 1. 首先，记住一件事：对于 near 数组，单位是 1 个嘀嗒，对于 t[0] 数组，单位是 25.6 个嘀嗒，越往后单位越大。
	 	就想象有几个表，有的是每一秒动一下，有的是每十秒动一下...
	 2. 现在抽象化，就假设有一个数组，长度为len，有一个变量 index 作为索引，不停的从第一个位置走到最后一个位置，
	 	当走到最后一个位置又折回走第一个位置。
	 	T->time & TIME_LEVEL_MASK 表示的就是这个 index 。
	 3. 在 add_node 中， node->expire 是 T->time + expire 得到的，所以当求 near 位置的时候 time&TIME_NEAR_MASK
	 	可以拆开，(T->time + expire) & TIME_NEAR_MASK ，T->time & TIME_LEVEL_MASK 就是当前的 index ，expire & TIME_LEVEL_MASK 其实就是 expire，
	 	所以其实找的是，当前 index 后面的 expire 个位置。
	 4. 在下面的循环中 idx 也是一个数组的索引， idx 会不停的往后走，所以 idx 后面的索引都不用动，轮到谁了就是谁。
	 	但是 idx 前面的索引必须动，因为 idx 前面的索引肯定是新加的，如果不是新加的那么早就执行了。既然是新加的，就
	 	必须不停的更新。
	*/
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

/*
当 message 的 data 不为空的时候， sz 表示 data 大小。
当 message 的 data 为空的时候， sz 表示两方通信的一个简单约定。
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

// 计算系统开机到现在的秒数，单位是10豪秒
static uint32_t
_gettime(void) {
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);	// 返回的是系统启动到现在的秒数和纳秒数
	uint32_t t = (uint32_t)(ti.tv_sec & 0xffffff) * 100;	// 秒数乘以 1000 等于毫秒数，那么乘以 100 就是10毫秒数
	t += ti.tv_nsec / 10000000;	// 本来应该是 (纳秒 / 1000 000 000 * 100)，也就是先转成秒，再乘以100

	return t;	// 返回的时间单位是 10毫秒
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

// 计时器，单位是10豪秒
uint32_t 
skynet_gettime(void) {
	return TI->current;
}

// 创建全局timer TI并初始化
void 
skynet_timer_init(void) {
	TI = timer_create_timer();
	TI->current = _gettime();
}
