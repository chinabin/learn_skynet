#include "map.h"

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

struct node {
	int fd;				//用来计算哈希索引
	int id;
	struct node * next;	//指向fd相同或者fd与map size做与运算的结果相同的node的位置
};

struct map {
	int size;		//hash node的个数
	struct node * hash;
};

//新建map，size等于第一个大于max的2的幂。
//例如max等于10，那么第一个大于10的2的次幂是16。如果max等于16，那么第一个大于16的2的次幂是32.
struct map * 
map_new(int max) {
	int sz = 1;
	while (sz <= max) {
		sz *= 2;
	}
	struct map * m = malloc(sizeof(*m));
	m->size = sz;
	m->hash = malloc(sizeof(struct node) * sz);
	int i;
	for (i=0;i<sz;i++) {
		m->hash[i].fd = -1;
		m->hash[i].id = 0;
		m->hash[i].next = NULL;
	}
	return m;
}

//删除map
void 
map_delete(struct map * m) {
	free(m->hash);
	free(m);
}

//从map的node里面找到fd等于指定fd的node
int 
map_search(struct map * m, int fd) {
	int hash = fd & (m->size-1);
	struct node * n = &m->hash[hash];
	do {
		if (n->fd == fd)
			return n->id;
		n = n->next;
	} while(n);
	return -1;
}

/*
每个node的索引都是通过fd计算出来的，不同的fd可能得到同样的索引。
如果两个不同的fd得到了同一个索引，那么后面将要插入的那个node会重新计算出另外一个索引，
因为占用了一个不属于它的位置，所以当属于那个位置的node插入的时候，错误插入的那个node会挪开
*/
void 
map_insert(struct map * m, int fd, int id) {
	int hash = fd & (m->size-1);
	struct node * n = &m->hash[hash];
	if (n->fd < 0) {	//表示未占用，这个fd还未存在于map
		n->fd = fd;
		n->id = id;
		return;
	}
	
	int ohash = n->fd & (m->size-1);
	if (hash != ohash) {
		//last是错误的node所属的队列的第一个元素
		struct node * last = &m->hash[ohash];
		//m->hash[hash]是错误的node，因为现在要把它的位置挪开，所以需要删除它
		while (last->next != &m->hash[hash]) {
			last = last->next;
		}
		//找到错误node的上一个元素然后将next指针指向错误node的下一个元素，&m->hash[hash]就是n
		last->next = n->next;

		//为错误的node去重新找位置，并且将错误node的数据更新为正确的数据
		int ofd = n->fd;
		int oid = n->id;
		n->fd = fd;
		n->id = id;
		n->next = NULL;
		map_insert(m,ofd, oid);
		return;
	}

	//两个不同的fd对应了一个相同的索引，现在需要给后面那个node找到一个新的索引
	int last = (n - m->hash) * 2;
	int i;
	for (i=0;i<m->size;i++) {
		int idx = (i + last + 1) & (m->size - 1);
		struct node * temp = &m->hash[idx];
		if (temp->fd < 0) {
			temp->fd = fd;
			temp->id = id;
			temp->next = n->next;
			n->next = temp;
			return;
		}
	}
	assert(0);
}

void
map_erase(struct map *m , int fd) {
	int hash = fd & (m->size-1);
	struct node * n = &m->hash[hash];
	if (n->fd == fd) {
		if (n->next == NULL) {		//这个fd对应的node只有一个
			n->fd = -1;
			return;
		}
		//这个fd对应的node不止一个，那么就把当前这个node删了，把这个node后面的node前移
		struct node * next = n->next;
		n->fd = next->fd;
		n->id = next->id;
		n->next = next->next;
		next->fd = -1;
		next->next = NULL;
		return;
	}
	if (n->next == NULL) {
		return;
	}
	struct node * last = n;
	n = n->next;
	for(;;) {
		if (n->fd == fd) {
			n->fd = -1;
			last->next = n->next;
			n->next = NULL;
			return;
		}
		if (n->next == NULL)
			return;
		last = n;
		n = n->next;
	}
}

//打印map里面的信息
void
map_dump(struct map *m) {
	int i;
	for (i=0;i<m->size;i++) {
		struct node * n = &(m->hash[i]);
		printf("[%d] fd = %d , id = %d , next = %d\n",i,n->fd,n->id,(int)(n->next - m->hash));
	}
}