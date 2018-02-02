#ifndef MREAD_MAP_H
#define MREAD_MAP_H

struct map;

//新建map，size等于2的max次幂
struct map * map_new(int max);
//删除map
void map_delete(struct map *);
//从map的node里面找到fd等于指定fd的node
int map_search(struct map * , int fd);
void map_insert(struct map * , int fd, int id);
void map_erase(struct map *, int fd);
//打印map里面的信息
void map_dump(struct map *m);

#endif
