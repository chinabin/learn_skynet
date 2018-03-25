#ifndef MREAD_MAP_H
#define MREAD_MAP_H

struct map;

/*
 新建 map ， size 等于第一个大于 max 的 2 的幂。
 例如 max 等于 10 ，那么第一个大于 10 的 2 的次幂是 16 。
 如果 max 等于 16 ，那么第一个大于 16 的 2 的次幂是 32 。
*/
struct map * map_new(int max);
// 删除 map
void map_delete(struct map *);
/*
 根据 fd ，从 map 里找到对应的 node ，并返回 id
*/
int map_search(struct map * , int fd);
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
void map_insert(struct map * , int fd, int id);
// 去除 node->fd 等于 fd 的 node
void map_erase(struct map *, int fd);
// 打印 map 里面的信息
void map_dump(struct map *m);

#endif
