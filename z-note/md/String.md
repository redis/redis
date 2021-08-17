# 字符串

> 数据结构实现 sds.h sds.c

# 为什么 redis 不用 C 的 char*
主要原因 char* 的结束是由 **"/0"** 结束

## char* 的结构设计
char*字符数组的结构很简单，就是一块连续的内存空间，依次存放了字符串中的每一个字。字符数组的结尾位置就用“\0”表示，意思是指字符串的结束。 
不符合 Redis 希望能保存任意二进制数据的需求*

## 操作函数复杂度
而除了 char* 字符数组结构的设计问题以外，使用“\0”作为字符串的结束字符，虽然可以让字符串操作函数判断字符串的结束位置，但它也会带来另一方面的负
面影响，也就是会导致操作函数的复杂度增加。
*不符合 Redis 对字符串高效操作的需求*




# SDS 结构

```c
#define SDS_TYPE_5  0 // 32
#define SDS_TYPE_8  1 // 256
#define SDS_TYPE_16 2 // 2^16
#define SDS_TYPE_32 3 // 2^32
#define SDS_TYPE_64 4// 2^64

```

SDS 结构体
```c
struct __attribute__ ((__packed__)) sdshdr8 {
uint8_t len; // 字符数组现有长度
uint8_t alloc;  // 字符数组的分配空间长度
unsigned char flags;  // SDS 类型 SDS_TYPE_5, SDS_TYPE_8, SDS_TYPE_16, SDS_TYPE_32, SDS_TYPE_64
char buf[]; // 字符数组
};
```
别名
```c
 typedef char *sds;
```

sds的基本函数
* sdslen(const sds s) 获取sds字符串的长度                       
* sdssetlen(sds s, size_t newlen) 设置sds字符串长度            
* sdsinclen(sds s, size_t inc) 增加sds字符串长度               
* sdsalloc(const sds s) 获取sds字符串容量                      
* sdssetalloc(sds s, size_t newlen) 设置sds字符串容量          
* sdsavail(const sds s) 获取sds字符串空余空间                    
* sdsHdrSize(char type) 根据header类型得到header大小            
* sdsReqType(size_t string_size) 根据字符串数据长度计算所需要的header类型
                                                      

char* 的不足：
- 操作效率低：获取长度需遍历，O(N)复杂度
- 二进制不安全：无法存储包含 \0 的数据

SDS 的优势：
- 操作效率高：获取长度无需遍历，O(1)复杂度
- 二进制安全：因单独记录长度字段，所以可存储包含 \0 的数据
- 兼容 C 字符串函数，可直接使用字符串 API

另外 Redis 在操作 SDS 时，为了避免频繁操作字符串时，每次「申请、释放」内存的开销，还做了这些优化：
- 内存预分配：SDS 扩容，会多申请一些内存（小于 1MB 翻倍扩容，大于 1MB 按 1MB 扩容）
- 多余内存不释放：SDS 缩容，不释放多余的内存，下次使用可直接复用这些内存
这种策略，是以多占一些内存的方式，换取「追加」操作的速度。

这个内存预分配策略，详细逻辑可以看 sds.c 的 sdsMakeRoomFor 函数。

课后题：SDS 字符串在 Redis 内部模块实现中也被广泛使用，你能在 Redis server 和客户端的实现中，找到使用 SDS 字符串的地方么？

1、Redis 中所有 key 的类型就是 SDS（详见 db.c 的 dbAdd 函数）

2、Redis Server 在读取 Client 发来的请求时，会先读到一个缓冲区中，这个缓冲区也是 SDS（详见 server.h 中 struct client 的 querybuf 字段）

3、写操作追加到 AOF 时，也会先写到 AOF 缓冲区，这个缓冲区也是 SDS （详见 server.h 中 struct client 的 aof_buf 字段）
