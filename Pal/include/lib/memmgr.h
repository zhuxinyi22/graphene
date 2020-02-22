/* Copyright (C) 2014 Stony Brook University
   This file is part of Graphene Library OS.

   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*
 * memmgr.h
 *
 * This file contains implementation of fixed-size memory allocator.
 */

#ifndef MEMMGR_H
#define MEMMGR_H

#include <sys/mman.h>

#include "api.h"
#include "assert.h"
#include "list.h"

#ifndef OBJ_TYPE
#error "OBJ_TYPE not defined"
#endif

#ifndef system_malloc
#error "macro \"void * system_malloc (size_t size)\" not declared"
#endif
#ifndef system_free
#error "macro \"void * system_free (void * ptr, size_t size)\" not declared"
#endif
#ifndef SYSTEM_LOCK
#define SYSTEM_LOCK() ({})
#endif
#ifndef SYSTEM_UNLOCK
#define SYSTEM_UNLOCK() ({})
#endif
#ifndef SYSTEM_LOCKED
#define SYSTEM_LOCKED() true
#endif

DEFINE_LIST(mem_obj);
typedef struct mem_obj {
    union {
        LIST_TYPE(mem_obj) __list;
        OBJ_TYPE obj;
    };
} MEM_OBJ_TYPE, *MEM_OBJ;

DEFINE_LIST(mem_area);
typedef struct mem_area {
    LIST_TYPE(mem_area) __list;
    size_t size;
    MEM_OBJ_TYPE objs[];
} MEM_AREA_TYPE, *MEM_AREA;

DEFINE_LISTP(mem_area);
DEFINE_LISTP(mem_obj);
typedef struct mem_mgr {
    LISTP_TYPE(mem_area) area_list;
    LISTP_TYPE(mem_obj) free_list;
    size_t size;
    MEM_OBJ_TYPE* obj;
    MEM_OBJ_TYPE* obj_limit;
    MEM_AREA active_area;
} MEM_MGR_TYPE, *MEM_MGR;

#define __SUM_OBJ_SIZE(size) (sizeof(MEM_OBJ_TYPE) * (size))
#define __MIN_MEM_SIZE()     (sizeof(MEM_MGR_TYPE) + sizeof(MEM_AREA_TYPE))
#define __MAX_MEM_SIZE(size) (__MIN_MEM_SIZE() + __SUM_OBJ_SIZE(size))

#ifdef ALLOC_ALIGNMENT
static inline size_t size_align_down(size_t size) {
    assert(IS_POWER_OF_2(ALLOC_ALIGNMENT));
    size_t s = __MAX_MEM_SIZE(size) - sizeof(MEM_MGR_TYPE);
    size_t p = s - ALIGN_DOWN_POW2(s, ALLOC_ALIGNMENT);
    size_t o = __SUM_OBJ_SIZE(1);
    return size - p / o - (p % o ? 1 : 0);
}

static inline size_t size_align_up(size_t size) {
    assert(IS_POWER_OF_2(ALLOC_ALIGNMENT));
    size_t s = __MAX_MEM_SIZE(size) - sizeof(MEM_MGR_TYPE);
    size_t p = ALIGN_UP_POW2(s, ALLOC_ALIGNMENT) - s;
    size_t o = __SUM_OBJ_SIZE(1);
    return size + p / o;
}

static inline size_t init_align_down(size_t size) {
    assert(IS_POWER_OF_2(ALLOC_ALIGNMENT));
    size_t s = __MAX_MEM_SIZE(size);
    size_t p = s - ALIGN_DOWN_POW2(s, ALLOC_ALIGNMENT);
    size_t o = __SUM_OBJ_SIZE(1);
    return size - p / o - (p % o ? 1 : 0);
}

static inline size_t init_align_up(size_t size) {
    assert(IS_POWER_OF_2(ALLOC_ALIGNMENT));
    size_t s = __MAX_MEM_SIZE(size);
    size_t p = ALIGN_UP_POW2(s, ALLOC_ALIGNMENT) - s;
    size_t o = __SUM_OBJ_SIZE(1);
    return size + p / o;
}
#endif

static inline void __set_free_mem_area(MEM_AREA area, MEM_MGR mgr) {
    assert(SYSTEM_LOCKED());

    mgr->size += area->size;
    mgr->obj         = area->objs;
    mgr->obj_limit   = area->objs + area->size;
    mgr->active_area = area;
}

static inline MEM_MGR create_mem_mgr(size_t size) {
    assert(size);

    void* mem = system_malloc(__MAX_MEM_SIZE(size));
    if (!mem)
        return NULL;

    MEM_MGR mgr = (MEM_MGR)mem;
    mgr->size = 0;
    INIT_LISTP(&mgr->area_list);
    INIT_LISTP(&mgr->free_list);

    MEM_AREA area = (MEM_AREA)(mem + sizeof(*mgr));
    area->size = size;
    INIT_LIST_HEAD(area, __list);
    LISTP_ADD(area, &mgr->area_list, __list);

    __set_free_mem_area(area, mgr);
    return mgr;
}

static inline MEM_MGR enlarge_mem_mgr(MEM_MGR mgr, size_t size) {
    MEM_AREA area;

    assert(size);
    area = (MEM_AREA)system_malloc(sizeof(*area) + __SUM_OBJ_SIZE(size));
    if (!area)
        return NULL;

    SYSTEM_LOCK();
    area->size = size;
    INIT_LIST_HEAD(area, __list);
    LISTP_ADD(area, &mgr->area_list, __list);
    SYSTEM_UNLOCK();
    return mgr;
}

static inline void destroy_mem_mgr(MEM_MGR mgr) {
    MEM_AREA tmp, n;

    MEM_AREA last = LISTP_LAST_ENTRY(&mgr->area_list, MEM_AREA_TYPE, __list);
    LISTP_FOR_EACH_ENTRY_SAFE(tmp, n, &mgr->area_list, __list) {
        if (tmp != last) {
            LISTP_DEL(tmp, &mgr->area_list, __list);
            system_free(tmp, sizeof(MEM_AREA_TYPE) + __SUM_OBJ_SIZE(tmp->size));
        }
    }
    system_free(mgr, __MAX_MEM_SIZE(last->size));
}

static inline OBJ_TYPE* get_mem_obj_from_mgr_enlarge(MEM_MGR mgr, size_t size) {
    MEM_OBJ mobj;

    SYSTEM_LOCK();
    while (mgr->obj == mgr->obj_limit && LISTP_EMPTY(&mgr->free_list)) {
        MEM_AREA area;

        /* If there is a previously allocated area, just activate it. */
        area = LISTP_PREV_ENTRY(mgr->active_area, &mgr->area_list, __list);
        if (area) {
            __set_free_mem_area(area, mgr);
            break;
        }

        SYSTEM_UNLOCK();
        if (!size)
            return NULL;

        area = (MEM_AREA)system_malloc(sizeof(MEM_AREA_TYPE) + __SUM_OBJ_SIZE(size));
        if (!area)
            return NULL;
        area->size = size;
        INIT_LIST_HEAD(area, __list);

        /* There can be concurrent operations to extend the manager. In case
         * someone has already enlarged the space, we just add the new area to
         * the list for later use. */
        SYSTEM_LOCK();
        LISTP_ADD(area, &mgr->area_list, __list);
    }

    if (!LISTP_EMPTY(&mgr->free_list)) {
        mobj = LISTP_FIRST_ENTRY(&mgr->free_list, MEM_OBJ_TYPE, __list);
        LISTP_DEL_INIT(mobj, &mgr->free_list, __list);
        CHECK_LIST_HEAD(MEM_OBJ, &mgr->free_list, __list);
    } else {
        mobj = mgr->obj++;
    }
    assert(mgr->obj <= mgr->obj_limit);
    SYSTEM_UNLOCK();
    return &mobj->obj;
}

static inline OBJ_TYPE* get_mem_obj_from_mgr(MEM_MGR mgr) {
    return get_mem_obj_from_mgr_enlarge(mgr, 0);
}

static inline void free_mem_obj_to_mgr(MEM_MGR mgr, OBJ_TYPE* obj) {
    MEM_OBJ mobj = container_of(obj, MEM_OBJ_TYPE, obj);

    SYSTEM_LOCK();
    INIT_LIST_HEAD(mobj, __list);
    LISTP_ADD(mobj, &mgr->free_list, __list);
    CHECK_LIST_HEAD(MEM_OBJ, &mgr->free_list, __list);
    SYSTEM_UNLOCK();
}

#endif /* MEMMGR_H */
