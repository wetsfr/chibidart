#ifndef CD_COMMON_H
#define CD_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define CD_GROW_CAPACITY(cap) ((cap) < 8 ? 8 : (cap) * 2)

#define CD_GROW_ARRAY(type, ptr, oldCount, newCount)     (type*)cd_reallocate(ptr, sizeof(type) * (oldCount), sizeof(type) * (newCount))

#define CD_FREE_ARRAY(type, ptr, oldCount)     cd_reallocate(ptr, sizeof(type) * (oldCount), 0)

#define CD_ALLOC(type, count)     (type*)cd_reallocate(NULL, 0, sizeof(type) * (count))

#define CD_FREE(type, ptr)     cd_reallocate(ptr, sizeof(type), 0)

void* cd_reallocate(void* ptr, size_t oldSize, size_t newSize);

#endif
