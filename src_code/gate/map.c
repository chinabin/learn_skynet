#include "map.h"

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

// 通过 fd 来映射 id
struct node {
	int fd;				// 通过 fd & (m->size-1) 来计算哈希索引，也就是 map 中的位置
	int id;				// 用户自定义的用来存储的值
	struct node * next;	// 指向fd相同或者fd与map size做与运算的结果相同的node的位置
};

struct map {
	int size;		//hash node的个数
	struct node * hash;
};

/*
 新建 map ， size 等于第一个大于 max 的 2 的幂。
 例如 max 等于 10 ，那么第一个大于 10 的 2 的次幂是 16 。
 如果 max 等于 16 ，那么第一个大于 16 的 2 的次幂是 32 。
*/
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

// 删除 map
void 
map_delete(struct map * m) {
	free(m->hash);
	free(m);
}

/*
 根据 fd ，从 map 里找到对应的 node ，并返回 id
*/
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
 通过 fd 计算一个索引，然后往 map 中插入一个新的 node。
 当出现碰撞(计算出来的索引已经被占用)的时候，假设占用的 fd 为 f_A ，传入的 fd 为 f_B ：
 1. (f_A & (m->size-1)) == (f_B & (m->size-1))，有亲戚关系
 	那么需要把 f_B 对应的 node_B 插入到一个空闲位置，
 	然后让 f_A 对应的 node_A 的 next 指针指向 node_B。
 2. (f_A & (m->size-1)) != (f_B & (m->size-1))，没任何关系
 	那么需要找到 f_A 对应的 node_A 的父 node ，也就是 node_AA，
 	使得 node_AA->next = node_A->next ，然后重新为 node_A 找到一个新的位置。
 	然后把 node_B 的数据填充到 node_A。
*/
void 
map_insert(struct map * m, int fd, int id) {
	int hash = fd & (m->size-1);
	struct node * n = &m->hash[hash];
	if (n->fd < 0) {	// 位置可用
		n->fd = fd;
		n->id = id;
		return;
	}
	
	// 发生碰撞，第二种情况
	int ohash = n->fd & (m->size-1);
	if (hash != ohash) {
		/*
		 因为是一个萝卜一个坑，如果存在一条链(fd & (m->size-1)的值全部相等)，
		 那么除了链头，其余元素其实都是占着属于别的萝卜的坑。
		 所以当我取这条链上任意一个 node 的 fd 所计算出来的索引都是一样，并且，
		 这个索引的位置就是链头。
		*/
		struct node * last = &m->hash[ohash];
		/*
		 last 就是链头，而 n(也就是 m->hash[hash]) 是需要腾出来的位置。
		 下面的循环就是找到 n 的父节点，做交接
		*/
		while (last->next != &m->hash[hash]) {
			last = last->next;
		}
		// 交接
		last->next = n->next;

		// 为错误的node去重新找位置，并且将错误 node 的数据更新为正确的数据
		int ofd = n->fd;
		int oid = n->id;
		n->fd = fd;
		n->id = id;
		n->next = NULL;
		map_insert(m,ofd, oid);
		return;
	}

	// 发生碰撞，第一种情况
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

// 去除 node->fd 等于 fd 的 node
void
map_erase(struct map *m , int fd) {
	int hash = fd & (m->size-1);
	struct node * n = &m->hash[hash];
	if (n->fd == fd) {
		if (n->next == NULL) {		// 这个 fd 对应的 node 只有一个
			n->fd = -1;
			return;
		}
		//这个 fd 对应的 node 不止一个，那么就把当前这个 node 删了，把这个 node 后面的node前移
		struct node * next = n->next;
		n->fd = next->fd;
		n->id = next->id;
		n->next = next->next;
		next->fd = -1;
		next->next = NULL;
		return;
	}
	/*
	 后面的情况是这样: 
	 一条链，因为除了链头其余元素都是占用空闲位置，但是每个元素的 fd 所计算出来的索引都是一样，
	 现在既然传入的 fd 找到了这条链，那么 fd 就可能存在在这条链上。
	*/
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