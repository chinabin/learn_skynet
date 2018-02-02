#ifndef MREAD_RINGBUFFER_H
#define MREAD_RINGBUFFER_H

struct ringbuffer;

struct ringbuffer_block {
	int length;		//sizeof(ringbuffer_block) + size
	int offset;
	int id;			//标号，-1表示无用
	int next;		//同一标号的下一块位置，小于0表示没有
};

//设定一个指定大小的ringbuffer，以及第一个ringbuffer_block
struct ringbuffer * ringbuffer_new(int size);
//释放ring_buffer
void ringbuffer_delete(struct ringbuffer * rb);
//将一个blk链接到同一id的blk中
void ringbuffer_link(struct ringbuffer *rb , struct ringbuffer_block * prev, struct ringbuffer_block * next);
struct ringbuffer_block * ringbuffer_alloc(struct ringbuffer * rb, int size);
int ringbuffer_collect(struct ringbuffer * rb);
void ringbuffer_resize(struct ringbuffer * rb, struct ringbuffer_block * blk, int size);
//将与指定blk的id号相同的blk都标记为废弃
void ringbuffer_free(struct ringbuffer * rb, struct ringbuffer_block * blk);
int ringbuffer_data(struct ringbuffer * rb, struct ringbuffer_block * blk, int size, int skip, void **ptr);
void * ringbuffer_copy(struct ringbuffer * rb, struct ringbuffer_block * from, int skip, struct ringbuffer_block * to);
struct ringbuffer_block * ringbuffer_yield(struct ringbuffer * rb, struct ringbuffer_block *blk, int skip);

//显示当前rb中前十个blk信息
void ringbuffer_dump(struct ringbuffer * rb);

#endif

