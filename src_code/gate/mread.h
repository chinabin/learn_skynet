#ifndef MREAD_H
#define MREAD_H

struct mread_pool;

//云风设计思路：https://blog.codingnow.com/2012/04/mread.html
//仓库地址：https://github.com/cloudwu/mread

 //创建和删除mread
struct mread_pool * mread_create(int port , int max , int buffer);
void mread_close(struct mread_pool *m);

//查询现在的状态，看到底发生了什么事，并做出相应的操作
int mread_poll(struct mread_pool *m , int timeout);
void * mread_pull(struct mread_pool *m , int size);
void mread_yield(struct mread_pool *m);
int mread_closed(struct mread_pool *m);
//关闭客户端
void mread_close_client(struct mread_pool *m, int id);
//返回index索引的socket句柄
int mread_socket(struct mread_pool *m , int index);

#endif
