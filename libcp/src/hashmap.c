// 开放寻址哈希表的基础设施: 键是任意字节串 (key, keylen), 值是 void*.
// 删除用 tombstone 标记, 使用率超过 70% 时 rehash, 重建后降到 50% 以下.
// 桶数组来自 context 的 arena, 不逐项释放; 宏表, pragma once 集合与
// include guard 表都建立在它之上.

#include "libcp.h"

// 初始桶数量.
#define INIT_SIZE 16

// 使用率超过 70% 时 rehash.
#define HIGH_WATERMARK 70

// rehash 之后把使用率保持在 50% 以下.
#define LOW_WATERMARK 50

// 表示一个已删除的哈希条目.
#define TOMBSTONE ((void*)-1)

// FNV 64 位哈希: 逐字节先乘质数再异或, 按长度计算, 因此键可以含 NUL.
static uint64_t fnv_hash(char* s, int len) {
  uint64_t hash = 0xcbf29ce484222325;
  for (int i = 0; i < len; i++) {
    hash *= 0x100000001b3;
    hash ^= (unsigned char)s[i];
  }
  return hash;
}

// 为新的插入腾出空间: 清掉 tombstone, 必要时扩大桶数组.
static void rehash(LibcpContext* ctx, LibcpHashMap* map) {
  // 先统计存活条目数, 据此确定新表的桶数.
  int nkeys = 0;
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
      nkeys++;

  int cap = map->capacity;
  while ((nkeys * 100) / cap >= LOW_WATERMARK)
    cap = cap * 2;
  assert(cap > 0);

  // 建一张新表并搬入全部存活键值; 新桶数组来自 arena, 旧数组不做释放.
  LibcpHashMap map2 = {};
  map2.buckets      = libcp_arena_calloc(ctx, cap, sizeof(LibcpHashEntry));
  map2.capacity     = cap;

  for (int i = 0; i < map->capacity; i++) {
    LibcpHashEntry* ent = &map->buckets[i];
    if (ent->key && ent->key != TOMBSTONE)
      libcp_hashmap_put2(ctx, &map2, ent->key, ent->keylen, ent->val);
  }

  assert(map2.used == nkeys);
  *map = map2;
}

// 判断条目是否为与 (key, keylen) 相同的存活条目; tombstone 与空桶都不匹配.
static bool match(LibcpHashEntry* ent, char* key, int keylen) {
  return ent->key && ent->key != TOMBSTONE && ent->keylen == keylen &&
         memcmp(ent->key, key, keylen) == 0;
}

// 线性探测查找条目; 遇到空桶说明键不存在, 返回 NULL.
static LibcpHashEntry* get_entry(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen) {
  if (!map->buckets)
    return NULL;

  uint64_t hash = fnv_hash(key, keylen);

  for (int i = 0; i < map->capacity; i++) {
    LibcpHashEntry* ent = &map->buckets[(hash + i) % map->capacity];
    if (match(ent, key, keylen))
      return ent;
    if (ent->key == NULL)
      return NULL;
  }
  unreachable();
}

// 查找或插入: 优先复用 tombstone, 否则占用第一个空桶.
// 空表先按初始容量分配, 插入前的使用率达到高水位时先 rehash.
static LibcpHashEntry*
    get_or_insert_entry(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen) {
  if (!map->buckets) {
    map->buckets  = libcp_arena_calloc(ctx, INIT_SIZE, sizeof(LibcpHashEntry));
    map->capacity = INIT_SIZE;
  } else if ((map->used * 100) / map->capacity >= HIGH_WATERMARK) {
    rehash(ctx, map);
  }

  uint64_t hash = fnv_hash(key, keylen);

  for (int i = 0; i < map->capacity; i++) {
    LibcpHashEntry* ent = &map->buckets[(hash + i) % map->capacity];

    if (match(ent, key, keylen))
      return ent;

    if (ent->key == TOMBSTONE) {
      // 复用 tombstone 槽位: 存活计数不变, 待删除计数减一.
      ent->key    = key;
      ent->keylen = keylen;
      map->ndeleted--;
      return ent;
    }

    if (ent->key == NULL) {
      ent->key    = key;
      ent->keylen = keylen;
      map->used++;
      return ent;
    }
  }
  unreachable();
}

// 下面三条短形式按 NUL 结尾的键取长度, 再转调带 keylen 的版本;
// 键含 NUL 或需要按前缀匹配时直接用 get2 / put2 / delete2.
void* libcp_hashmap_get(LibcpContext* ctx, LibcpHashMap* map, char* key) {
  return libcp_hashmap_get2(ctx, map, key, strlen(key));
}

void* libcp_hashmap_get2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen) {
  LibcpHashEntry* ent = get_entry(ctx, map, key, keylen);
  return ent ? ent->val : NULL;
}

void libcp_hashmap_put(LibcpContext* ctx, LibcpHashMap* map, char* key, void* val) {
  libcp_hashmap_put2(ctx, map, key, strlen(key), val);
}

void libcp_hashmap_put2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen, void* val) {
  LibcpHashEntry* ent = get_or_insert_entry(ctx, map, key, keylen);
  ent->val            = val;
}

void libcp_hashmap_delete(LibcpContext* ctx, LibcpHashMap* map, char* key) {
  libcp_hashmap_delete2(ctx, map, key, strlen(key));
}

void libcp_hashmap_delete2(LibcpContext* ctx, LibcpHashMap* map, char* key, int keylen) {
  LibcpHashEntry* ent = get_entry(ctx, map, key, keylen);
  if (!ent)
    return;

  ent->key = TOMBSTONE;
  map->ndeleted++;

  // tombstone 会延长之后每一次探测, 直到下一次 rehash; 只看 `used` 的
  // 话, 还要再插入很多次才会碰到插入水位. 一旦 tombstone 多于存活条目,
  // 就地重建, 以免一段以删除为主的时期把表留成半死状态 (C8).
  if (map->ndeleted > map->used - map->ndeleted)
    rehash(ctx, map);
}
