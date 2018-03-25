// win_skynet.cpp : Defines the entry point for the console application.
//

#include "mread-master\ringbuffer.h"
#include <stdio.h>

static void
init(struct ringbuffer_block * blk, int n, int from) {
	char * ptr = (char *)(blk + 1);
	int i;
	for (i = 0; i < n; i++) {
		ptr[i] = i + 1 + from;
	}
}

static void
dump(struct ringbuffer * rb, struct ringbuffer_block *blk, int size) {
	void * buffer;
	int sz = ringbuffer_data(rb, blk, size, 0, &buffer);
	char * data = (char *)buffer;
	if (data) {
		int i;
		for (i = 0; i < sz; i++) {
			printf("%d ", data[i]);
		}
		printf("\n");
	}
	else {
		printf("size = %d\n", sz);
	}
}

//使用ringbuffer_data、ringbuffer_copy、ringbuffer_yield
static void
test1(struct ringbuffer *rb) {
	int alloc_sz = 5;
	struct ringbuffer_block * blk, *blk_head;
	blk = ringbuffer_alloc(rb, alloc_sz);
	init(blk, alloc_sz, 0);
	blk_head = blk;
	blk->id = 1;

	blk = ringbuffer_alloc(rb, alloc_sz);
	init(blk, alloc_sz, 10);
	ringbuffer_link(rb, blk_head, blk);

	blk = ringbuffer_alloc(rb, alloc_sz);
	init(blk, alloc_sz, 100);
	ringbuffer_link(rb, blk_head, blk);

	ringbuffer_dump(rb);

	void *data;
	//获取会失败(data == NULL)，因为blk_head没有这么多数据
	int ret = ringbuffer_data(rb, blk_head, alloc_sz * 3, 0, &data);

	struct ringbuffer_block *blk_des;
	blk_des = ringbuffer_alloc(rb, ret);
	ringbuffer_copy(rb, blk_head, 0, blk_des);
	dump(rb, blk_des, ret);		//blk_des里面有数据了

	ret = ringbuffer_data(rb, blk_des, alloc_sz * 3, 0, &data);		//data里面也有数据了
	for (int i = 0; i < blk_des->length - sizeof(ringbuffer_block); ++i)
	{
		printf("%d ", ((char *)data)[i]);
	}
	//ringbuffer_data取的是一个blk中的数据
	//ringbuffer_copy取的是一串id相同的blk中的数据



	printf("\n---after yield 8---\n");
	blk_des = ringbuffer_yield(rb, blk_des, 8);
	dump(rb, blk_des, blk_des->length - sizeof(ringbuffer_block) - blk_des->offset);
}

//分配空间查找的时候不会跳过已经使用的地方去寻找
static void
test2(struct ringbuffer *rb) {
	struct ringbuffer_block * blk, *blk_bak;

	//20
	blk = ringbuffer_alloc(rb, 1);
	blk->id = 0;

	//56
	blk = ringbuffer_alloc(rb, 40);
	blk_bak = blk;
	blk->id = 1;

	//20
	blk = ringbuffer_alloc(rb, 1);
	blk->id = 2;

	ringbuffer_dump(rb);
	ringbuffer_free(rb, blk_bak);		//尽管中间有足够空间，后面的请求还是会失败
	blk = ringbuffer_alloc(rb, 30);		//blk == NULL
	ringbuffer_dump(rb);
}

static void
test(struct ringbuffer *rb) {
	struct ringbuffer_block * blk;

	blk = ringbuffer_alloc(rb, 48);
	blk->id = 0;
	ringbuffer_free(rb, blk);

	blk = ringbuffer_alloc(rb, 48);
	blk->id = 1;
	ringbuffer_free(rb, blk);

	blk = ringbuffer_alloc(rb, 80);
	blk->id = 0;
	ringbuffer_free(rb, blk);


	blk = ringbuffer_alloc(rb, 50);
	blk->id = 1;
	struct ringbuffer_block * next = ringbuffer_alloc(rb, 40);
	next->id = 1;
	ringbuffer_link(rb, blk, next);
	ringbuffer_dump(rb);
	blk = ringbuffer_alloc(rb, 4);
	printf("%p\n", blk);
	int id = ringbuffer_collect(rb);
	printf("collect %d\n", id);

	blk = ringbuffer_alloc(rb, 4);
	blk->id = 2;
	init(blk, 4, 0);

	next = ringbuffer_alloc(rb, 5);
	init(next, 5, 0);
	ringbuffer_link(rb, blk, next);

	next = ringbuffer_alloc(rb, 6);
	init(next, 6, 0);
	ringbuffer_link(rb, blk, next);


	dump(rb, blk, 3);
	dump(rb, blk, 6);
	dump(rb, blk, 16);

	blk = ringbuffer_yield(rb, blk, 5);

	next = ringbuffer_alloc(rb, 7);
	ringbuffer_copy(rb, blk, 1, next);
	dump(rb, next, 7);

	blk = ringbuffer_yield(rb, blk, 5);

	ringbuffer_dump(rb);
}

int
main() {
	struct ringbuffer * rb = ringbuffer_new(128);
	test1(rb);
	ringbuffer_delete(rb);
	return 0;
}
