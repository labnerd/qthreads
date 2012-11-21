#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* The API */
#include "qthread/qthread.h"

/* System Headers */

/* FEB Internal API */
#include "qt_feb.h"

/* Internal Headers */
#include "qt_subsystems.h"
#include "qt_hash.h"
#include "qthread_innards.h"
#include "qt_profiling.h"
#include "qt_qthread_struct.h"
#include "qt_qthread_mgmt.h"
#include "qt_blocking_structs.h"
#include "qt_addrstat.h"
#include "qt_threadqueues.h"
#include "qt_debug.h"
#include "qt_eurekas.h" // for qthread_internal_assassinate() (used in taskfilter)
#include "qt_output_macros.h"

/********************************************************************
 * Local Variables
 *********************************************************************/
static qt_hash *FEBs;
#ifdef QTHREAD_COUNT_THREADS
aligned_t *febs_stripes;
# ifdef QTHREAD_MUTEX_INCREMENT
QTHREAD_FASTLOCK_TYPE *febs_stripes_locks;
# endif
#endif

/********************************************************************
 * Local Types
 *********************************************************************/
typedef enum bt {
    WRITEEF,
    WRITEEF_NB,
    WRITEF,
    READFF,
    READFF_NB,
    READFE,
    READFE_NB,
    FILL,
    EMPTY
} blocker_type;
typedef struct {
    pthread_mutex_t lock;
    void           *a;
    void           *b;
    blocker_type    type;
    int             retval;
} qthread_feb_blocker_t;

/********************************************************************
 * Local Prototypes
 *********************************************************************/
static QINLINE void qthread_gotlock_fill(qthread_shepherd_t *shep,
                                         qthread_addrstat_t *m,
                                         void               *maddr,
                                         const uint_fast8_t  recursive,
                                         qthread_addrres_t  *precond_tasks);
static QINLINE void qthread_gotlock_empty(qthread_shepherd_t *shep,
                                          qthread_addrstat_t *m,
                                          void               *maddr,
                                          const uint_fast8_t  recursive,
                                          qthread_addrres_t  *precond_tasks);

/********************************************************************
 * Shared Globals
 *********************************************************************/
#if !defined(UNPOOLED_ADDRSTAT) && !defined(UNPOOLED)
qt_mpool generic_addrstat_pool = NULL;
#endif
#if !defined(UNPOOLED_ADDRRES) && !defined(UNPOOLED)
qt_mpool generic_addrres_pool = NULL;
#endif

/********************************************************************
 * Functions
 *********************************************************************/

static void qt_feb_subsystem_shutdown(void)
{
    qthread_debug(FEB_DETAILS, "destroy feb infrastructure arrays\n");
    for (unsigned i = 0; i < QTHREAD_LOCKING_STRIPES; i++) {
        qt_hash_destroy_deallocate(FEBs[i],
                                   (qt_hash_deallocator_fn)
                                   qthread_addrstat_delete);
#ifdef QTHREAD_COUNT_THREADS
        print_status("bin %i used %u times for FEBs\n", i, (unsigned int)febs_stripes[i]);
# ifdef QTHREAD_MUTEX_INCREMENT
        QTHREAD_FASTLOCK_DESTROY(febs_stripes_locks[i]);
# endif
#endif
    }
    FREE(FEBs, sizeof(qt_hash) * QTHREAD_LOCKING_STRIPES);
#ifdef QTHREAD_COUNT_THREADS
    FREE(febs_stripes, sizeof(aligned_t) * QTHREAD_LOCKING_STRIPES);
# ifdef QTHREAD_MUTEX_INCREMENT
    FREE(febs_stripes_locks, sizeof(QTHREAD_FASTLOCK_TYPE) * QTHREAD_LOCKING_STRIPES);
# endif
#endif
#if !defined(UNPOOLED_ADDRSTAT) && !defined(UNPOOLED)
    qt_mpool_destroy(generic_addrstat_pool);
    generic_addrstat_pool = NULL;
#endif
#if !defined(UNPOOLED_ADDRRES) && !defined(UNPOOLED)
    qt_mpool_destroy(generic_addrres_pool);
    generic_addrres_pool = NULL;
#endif
}

void INTERNAL qt_feb_subsystem_init(uint_fast8_t need_sync)
{
#if !defined(UNPOOLED_ADDRSTAT) && !defined(UNPOOLED)
    generic_addrstat_pool = qt_mpool_create(sizeof(qthread_addrstat_t));
#endif
#if !defined(UNPOOLED_ADDRRES) && !defined(UNPOOLED)
    generic_addrres_pool = qt_mpool_create(sizeof(qthread_addrres_t));
#endif
    FEBs = MALLOC(sizeof(qt_hash) * QTHREAD_LOCKING_STRIPES);
    assert(FEBs);
#ifdef QTHREAD_COUNT_THREADS
    febs_stripes = MALLOC(sizeof(aligned_t) * QTHREAD_LOCKING_STRIPES);
    assert(febs_stripes);
# ifdef QTHREAD_MUTEX_INCREMENT
    febs_stripes_locks = MALLOC(sizeof(QTHREAD_FASTLOCK_TYPE) * QTHREAD_LOCKING_STRIPES);
    assert(febs_stripes_locks);
# endif
#endif /* ifdef QTHREAD_COUNT_THREADS */
    for (unsigned i = 0; i < QTHREAD_LOCKING_STRIPES; i++) {
#ifdef QTHREAD_COUNT_THREADS
        febs_stripes[i] = 0;
# ifdef QTHREAD_MUTEX_INCREMENT
        QTHREAD_FASTLOCK_INIT(febs_stripes_locks[i]);
# endif
#endif
        FEBs[i] = qt_hash_create(need_sync);
        assert(FEBs[i]);
    }
    qthread_internal_cleanup_late(qt_feb_subsystem_shutdown);
}

static inline void qt_feb_schedule(qthread_t          *waiter,
                                   qthread_shepherd_t *shep)
{
    qthread_debug(FEB_DETAILS, "waiter(%p:%i), shep(%p:%i): setting waiter to 'RUNNING'\n", waiter, (int)waiter->thread_id, shep, (int)shep->shepherd_id);
    waiter->thread_state = QTHREAD_STATE_RUNNING;
    if ((waiter->flags & QTHREAD_UNSTEALABLE) && (waiter->rdata->shepherd_ptr != shep)) {
        qthread_debug(FEB_DETAILS, "waiter(%p:%i), shep(%p:%i): enqueueing waiter in target_shep's ready queue (%p:%i)\n", waiter, (int)waiter->thread_id, shep, (int)shep->shepherd_id, waiter->rdata->shepherd_ptr, waiter->rdata->shepherd_ptr->shepherd_id);
        qt_threadqueue_enqueue(waiter->rdata->shepherd_ptr->ready, waiter);
    } else
#ifdef QTHREAD_USE_SPAWNCACHE
    if (!qt_spawncache_spawn(waiter, shep->ready))
#endif
    {
        qthread_debug(FEB_DETAILS, "waiter(%p:%i), shep(%p:%i): enqueueing waiter in shep's ready queue\n", waiter, (int)waiter->thread_id, shep, (int)shep->shepherd_id);
        qt_threadqueue_enqueue(shep->ready, waiter);
    }
}

/* functions to implement FEB locking/unlocking */

static aligned_t qthread_feb_blocker_thread(void *arg)
{                                      /*{{{ */
    qthread_feb_blocker_t *const restrict a = (qthread_feb_blocker_t *)arg;

    switch (a->type) {
        case READFE:
            a->retval = qthread_readFE(a->a, a->b);
            break;
        case READFE_NB:
            a->retval = qthread_readFE_nb(a->a, a->b);
            break;
        case READFF:
            a->retval = qthread_readFF(a->a, a->b);
            break;
        case READFF_NB:
            a->retval = qthread_readFF_nb(a->a, a->b);
            break;
        case WRITEEF:
            a->retval = qthread_writeEF(a->a, a->b);
            break;
        case WRITEEF_NB:
            a->retval = qthread_writeEF_nb(a->a, a->b);
            break;
        case WRITEF:
            a->retval = qthread_writeF(a->a, a->b);
            break;
        case FILL:
            a->retval = qthread_fill(a->a);
            break;
        case EMPTY:
            a->retval = qthread_empty(a->a);
            break;
    }
    pthread_mutex_unlock(&(a->lock));
    return 0;
}                                      /*}}} */

static int qthread_feb_blocker_func(void        *dest,
                                    void        *src,
                                    blocker_type t)
{   /*{{{*/
    qthread_feb_blocker_t args = { PTHREAD_MUTEX_INITIALIZER, dest, src, t, QTHREAD_SUCCESS };

    pthread_mutex_lock(&args.lock);
    qthread_fork(qthread_feb_blocker_thread, &args, NULL);
    pthread_mutex_lock(&args.lock);
    pthread_mutex_unlock(&args.lock);
    pthread_mutex_destroy(&args.lock);
    return args.retval;
} /*}}}*/

/* this function based on http://burtleburtle.net/bob/hash/evahash.html */
#define rot(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static inline uint64_t qt_hashword(uint64_t key)
{                       /*{{{*/
    const union {
        uint64_t key;
        uint8_t  b[sizeof(uint64_t)];
    } k = {
        key
    };

#if (SIZEOF_VOIDP == 8) /* i.e. a 64-bit machine */
    uint64_t a, b, c;

    a = b = 0x9e3779b97f4a7c13LL;  // the golden ratio
    c = 0xdeadbeefcafebabeULL + sizeof(uint64_t);

    a += ((uint64_t)k.b[7]) << 56;
    a += ((uint64_t)k.b[6]) << 48;
    a += ((uint64_t)k.b[5]) << 40;
    a += ((uint64_t)k.b[4]) << 32;
    a += k.b[3] << 24;
    a += k.b[2] << 16;
    a += k.b[1] << 8;
    a += k.b[0];

    a = a - b;  a = a - c;  a = a ^ (c >> 43);
    b = b - c;  b = b - a;  b = b ^ (a << 9);
    c = c - a;  c = c - b;  c = c ^ (b >> 8);
    a = a - b;  a = a - c;  a = a ^ (c >> 38);
    b = b - c;  b = b - a;  b = b ^ (a << 23);
    c = c - a;  c = c - b;  c = c ^ (b >> 5);
    a = a - b;  a = a - c;  a = a ^ (c >> 35);
    b = b - c;  b = b - a;  b = b ^ (a << 49);
    c = c - a;  c = c - b;  c = c ^ (b >> 11);
    a = a - b;  a = a - c;  a = a ^ (c >> 12);
    b = b - c;  b = b - a;  b = b ^ (a << 18);
    c = c - a;  c = c - b;  c = c ^ (b >> 22);
    return c;

#else /* if (SIZEOF_VOIDP == 8) */
    uint32_t a, b, c;

    a = b = 0x9e3779b9;  // the golden ratio
    c = 0xdeadbeef + sizeof(uint64_t);

    b += k.b[7] << 24;
    b += k.b[6] << 16;
    b += k.b[5] << 8;
    b += k.b[4];
    a += k.b[3] << 24;
    a += k.b[2] << 16;
    a += k.b[1] << 8;
    a += k.b[0];

    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
    return ((uint64_t)c + ((uint64_t)b << 32));
#endif /* if (SIZEOF_VOIDP == 8) */
} /*}}}*/

#define QTHREAD_CHOOSE_STRIPE2(addr) (qt_hashword((uint64_t)(uintptr_t)addr) & (QTHREAD_LOCKING_STRIPES - 1))
// #define QTHREAD_CHOOSE_STRIPE2(addr) QTHREAD_CHOOSE_STRIPE(addr)
/* The lock ordering in these functions is very particular, and is designed to
 * reduce the impact of having only one hashtable. Don't monkey with it unless
 * you REALLY know what you're doing! If one hashtable becomes a problem, we
 * may need to move to a new mechanism.
 */

/* This is just a little function that should help in debugging */
int API_FUNC qthread_feb_status(const aligned_t *addr)
{                      /*{{{ */
    const aligned_t *alignedaddr;

    qthread_debug(FEB_CALLS, "addr=%p (tid=%u)\n", addr, qthread_id());
    if (qlib == NULL) {
        return 1;
    }
#ifndef SST
    qthread_addrstat_t *m;
    int                 status  = 1; /* full */
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(addr);

    QALIGN(addr, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
        if (!m) { break; }
        hazardous_ptr(0, m);
        if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
        if (!m->valid) { continue; }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            continue;
        }
        status = m->full;
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        break;
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]); {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin],
                                                     (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            status = m->full;
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        }
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    qthread_debug(FEB_BEHAVIOR, "addr %p is %i\n", addr, status);
    return status;

#else /* ifndef SST */
    QALIGN(addr, alignedaddr);
    return PIM_feb_is_full((void *)alignedaddr);
#endif /* ifndef SST */
}                      /*}}} */

/* this function removes the FEB data structure for the address maddr from the
 * hash table */
static QINLINE void qthread_FEB_remove(void *maddr)
{                      /*{{{ */
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(maddr);

    // qthread_debug(ALWAYS_OUTPUT, "Attempting removal of addr %p\n", maddr);
    qthread_debug(FEB_BEHAVIOR, "maddr=%p: attempting removal\n", maddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
#ifdef LOCK_FREE_FEBS
    {
        qthread_addrstat_t *m2;
        m = qt_hash_get(FEBs[lockbin], maddr);
got_m:
        if (!m) {
            qthread_debug(FEB_DETAILS, "maddr=%p: addrstat already gone; someone else removed it!\n", maddr);
            return;
        }
        hazardous_ptr(0, m);
        if (m != (m2 = qt_hash_get(FEBs[lockbin], maddr))) {
            m = m2;
            goto got_m;
        }
        if (!m->valid) {
            qthread_debug(FEB_DETAILS, "maddr=%p: addrstat invalid; someone else invalidated it!\n", maddr);
            return;
        }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            qthread_debug(FEB_DETAILS, "maddr=%p: addrstat invalid; someone else invalidated it!\n", maddr);
            return;
        }
        if ((m->FEQ == NULL) && (m->EFQ == NULL) && (m->FFQ == NULL) &&
            (m->full == 1)) {
            qthread_debug(FEB_DETAILS, "maddr=%p: lists are empty, status is full; invalidating and removing (m:%p)\n", maddr, m);
            m->valid = 0;
            qassertnot(qt_hash_remove(FEBs[lockbin], maddr), 0);
        } else {
            QTHREAD_FASTLOCK_UNLOCK(&(m->lock));
            qthread_debug(FEB_DETAILS, "maddr=%p: addrstat cannot be removed; in use\n", maddr);
            return;
        }
    }
#else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]); {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], maddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&(m->lock));
            if ((m->FEQ == NULL) && (m->EFQ == NULL) && (m->FFQ == NULL) &&
                (m->full == 1)) {
                qthread_debug(FEB_DETAILS, "maddr=%p: lists are empty, status is full; invalidating and removing\n", maddr);
                qassertnot(qt_hash_remove_locked(FEBs[lockbin], maddr), 0);
            } else {
                QTHREAD_FASTLOCK_UNLOCK(&(m->lock));
                qthread_debug(FEB_DETAILS, "maddr=%p: addrstat cannot be removed; in use\n", maddr);
                m = NULL;
            }
        }
    }
    qt_hash_unlock(FEBs[lockbin]);
#endif /* ifdef LOCK_FREE_FEBS */
    if (m != NULL) {
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
#ifdef LOCK_FREE_FEBS
        hazardous_release_node((hazardous_free_f)qthread_addrstat_delete, m);
#else
        qthread_addrstat_delete(m);
#endif
    }
}                      /*}}} */

static QINLINE void qthread_precond_init(qthread_addrres_t **precond_tasks_p)
{   /*{{{*/
    /*create empty head to avoid later checks/branches; use the waiter to find the tail*/
    *precond_tasks_p           = ALLOC_ADDRRES();
    (*precond_tasks_p)->waiter = (void *)(*precond_tasks_p);
} /*}}}*/

static QINLINE void qthread_precond_launch(qthread_shepherd_t *shep,
                                           qthread_addrres_t  *precond_tasks)
{   /*{{{*/
    qthread_addrres_t *precond_tail = ((qthread_addrres_t *)(precond_tasks->waiter));

    if (precond_tasks != precond_tail) {
        qthread_addrres_t *precond_free = precond_tasks, *precond_head = precond_tasks;
        do {
            precond_head = precond_head->next;
            FREE_ADDRRES(precond_free);
            if (qthread_check_feb_preconds(precond_head->waiter) != 1) {
                if (precond_head->waiter->target_shepherd == NO_SHEPHERD) {
                    qt_threadqueue_enqueue(shep->ready, precond_head->waiter);
                } else {
                    qt_threadqueue_enqueue(qlib->shepherds[precond_head->waiter->target_shepherd].ready, precond_head->waiter);
                }
            }
            precond_free = precond_head;
        } while(precond_head != precond_tail);
        FREE_ADDRRES(precond_free);
    }
} /*}}}*/

static QINLINE void qthread_gotlock_empty(qthread_shepherd_t *shep,
                                          qthread_addrstat_t *m,
                                          void               *maddr,
                                          const uint_fast8_t  recursive,
                                          qthread_addrres_t  *precond_tasks)
{                      /*{{{ */
    qthread_addrres_t *X = NULL;
    int                removeable;

    assert(m);
    qthread_debug(FEB_FUNCTIONS, "m(%p), maddr(%p), recursive(%u)\n", m, maddr, recursive);
    m->full = 0;
    QTHREAD_EMPTY_TIMER_START(m);
    if ((precond_tasks == NULL) && (recursive == 0)) {
        qthread_precond_init(&precond_tasks);
    }
    if (m->EFQ != NULL) {
        /* dQ */
        X      = m->EFQ;
        m->EFQ = X->next;
        /* op */
        if (maddr && (maddr != X->addr)) {
            *(aligned_t *)maddr = *(X->addr);
            MACHINE_FENCE;
        }
        /* requeue */
        qthread_debug(FEB_DETAILS, "m(%p), maddr(%p), recursive(%u): dQ 1 EFQ (%u releasing tid %u with %u), will fill\n", m, maddr, recursive, qthread_id(), X->waiter->thread_id, *(X->addr));
        qt_feb_schedule(X->waiter, shep);
        FREE_ADDRRES(X);
        qthread_gotlock_fill(shep, m, maddr, 1, precond_tasks);
    }
    if ((m->full == 1) && (m->EFQ == NULL) && (m->FEQ == NULL) && (m->FFQ == NULL)) {
        removeable = 1;
    } else {
        removeable = 0;
    }
    if (recursive == 0) {
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        qthread_precond_launch(shep, precond_tasks);
        if (removeable) {
            qthread_FEB_remove(maddr);
        }
    }
}                      /*}}} */

/*Note: isn't the fact that we're delaying only precond tasks assign implicit priorities?*/
static QINLINE void qthread_gotlock_fill(qthread_shepherd_t *shep,
                                         qthread_addrstat_t *m,
                                         void               *maddr,
                                         const uint_fast8_t  recursive,
                                         qthread_addrres_t  *precond_tasks)
{                      /*{{{ */
    qthread_addrres_t *X = NULL;

    qthread_debug(FEB_FUNCTIONS, "m(%p), addr(%p), recursive(%u)\n", m, maddr, recursive);
    assert(m);
    m->full = 1;
    QTHREAD_EMPTY_TIMER_STOP(m);
    if ((precond_tasks == NULL) && (recursive == 0)) {
        qthread_precond_init(&precond_tasks);
    }
    /* dequeue all FFQ, do their operation, and schedule them */
    while (m->FFQ != NULL) {
        /* dQ */
        X      = m->FFQ;
        m->FFQ = X->next;
        /* op */
        if (X->addr && (X->addr != maddr)) {
            *(aligned_t *)(X->addr) = *(aligned_t *)maddr;
            MACHINE_FENCE;
        }
        /* schedule */
        qthread_t *waiter = X->waiter;
        qthread_debug(FEB_DETAILS, "m(%p), maddr(%p), recursive(%u): dQ one from FFQ (%u releasing tid %u with %u)\n", m, maddr, recursive, qthread_id(), waiter->thread_id, *(aligned_t *)maddr);
        if (QTHREAD_STATE_NASCENT == waiter->thread_state) {
            /* Note: the nascent thread is being tossed into a real live ready
             * queue for one big fat reason: the alternative involves
             * potentially locking multiple parts of the FEB hash table, not to
             * mention needing to avoid re-locking the portion we've already
             * got locked. By allowing the precond thread to have its
             * conditions checked by a shepherd, we can ensure that we don't
             * have this problem. Also, this allows the "work" of checking
             * preconds to be load-balanced by workstealing schedulers, if
             * that's important or useful.
             * Changed behavior to batch all tasks with preconditions,
             * thus avoiding a possible dead-lock but having to launch all
             * sequentially after releasing the lock. On the plus side,
             * all tasks present in the ready queue (or cache) are ready to run.*/
            ((qthread_addrres_t *)(precond_tasks->waiter))->next = X;
            precond_tasks->waiter                                = (void *)X;
        } else {
            qt_feb_schedule(waiter, shep);
            FREE_ADDRRES(X);
        }
    }
    if (m->FEQ != NULL) {
        /* dequeue one FEQ, do their operation, and schedule them */
        X      = m->FEQ;
        m->FEQ = X->next;
        /* op */
        if (X->addr && (X->addr != maddr)) {
            *(aligned_t *)(X->addr) = *(aligned_t *)maddr;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_DETAILS, "m(%p), maddr(%p), recursive(%u): dQ 1 EFQ (%u releasing tid %u with %u), will empty\n", m, maddr, recursive, qthread_id(), X->waiter->thread_id, *(aligned_t *)maddr);
        qt_feb_schedule(X->waiter, shep);
        FREE_ADDRRES(X);
        qthread_gotlock_empty(shep, m, maddr, 1, precond_tasks);
    }
    if (recursive == 0) {
        int removeable;
        if ((m->EFQ == NULL) && (m->FEQ == NULL) && (m->full == 1)) {
            qthread_debug(FEB_DETAILS, "m(%p), addr(%p), recursive(%u): addrstat removeable!\n", m, maddr, recursive);
            removeable = 1;
        } else {
            removeable = 0;
        }
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        qthread_precond_launch(shep, precond_tasks);
        /* now, remove it if it needs to be removed */
        if (removeable) {
            qthread_debug(FEB_DETAILS, "m(%p), addr(%p), recursive(%u): removing addrstat\n", m, maddr, recursive);
            qthread_FEB_remove(maddr);
        }
    }
}                      /*}}} */

int API_FUNC qthread_empty(const aligned_t *dest)
{                      /*{{{ */
    const aligned_t *alignedaddr;

#ifndef SST
    qthread_addrstat_t *m;
    qt_hash             FEBbin;
    qthread_shepherd_t *shep = qthread_internal_getshep();

    assert(qthread_library_initialized);

    if (!shep) {
        return qthread_feb_blocker_func((void *)dest, NULL, EMPTY);
    }
    QALIGN(dest, alignedaddr);
    {
        const int lockbin = QTHREAD_CHOOSE_STRIPE2(alignedaddr);
        FEBbin = FEBs[lockbin];
        qthread_debug(FEB_CALLS, "dest=%p (tid=%i lockbin=%u)\n", dest, qthread_id(), lockbin);

        QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
    }
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBbin, (void *)alignedaddr);
        if (!m) {
            /* currently full, and must be added to the hash to empty */
            m = qthread_addrstat_new();
            if (!m) { return QTHREAD_MALLOC_ERROR; }
            m->full = 0;
            MACHINE_FENCE;
            QTHREAD_EMPTY_TIMER_START(m);
            if (!qt_hash_put(FEBbin, (void *)alignedaddr, m)) {
                qthread_addrstat_delete(m);
                continue;
            }
            m = NULL;
            break;
        } else {
            /* it could be either full or not, don't know */
            hazardous_ptr(0, m);
            if (m != qt_hash_get(FEBbin, (void *)alignedaddr)) { continue; }
            if (!m->valid) { continue; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            if (!m->full) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                m = NULL;
            }
            break;
        }
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBbin);
    {                      /* BEGIN CRITICAL SECTION */
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBbin, (void *)alignedaddr);
        if (!m) {
            /* currently full, and must be added to the hash to empty */
            m = qthread_addrstat_new();
            if (!m) {
                qt_hash_unlock(FEBbin);
                return QTHREAD_MALLOC_ERROR;
            }
            m->full = 0;
            QTHREAD_EMPTY_TIMER_START(m);
            COMPILER_FENCE;
            qassertnot(qt_hash_put_locked(FEBbin, (void *)alignedaddr, m), 0);
            qthread_debug(FEB_DETAILS, "dest=%p (tid=%i): inserted m=%p\n", dest, qthread_id(), m);
            m = NULL;
        } else {
            /* it could be either full or not, don't know */
            qthread_debug(FEB_DETAILS, "dest=%p (tid=%i): found m=%p\n", dest, qthread_id(), m);
            QTHREAD_FASTLOCK_LOCK(&m->lock);
        }
    }                      /* END CRITICAL SECTION */
    qt_hash_unlock(FEBbin);
# endif /* ifdef LOCK_FREE_FEBS */
    if (m) {
        qthread_debug(FEB_BEHAVIOR, "dest=%p (tid=%i): waking waiters\n", dest, qthread_id());
        qthread_gotlock_empty(shep, m, (void *)alignedaddr, 0, NULL);
    }
    qthread_debug(FEB_BEHAVIOR, "dest=%p (tid=%i): success\n", dest, qthread_id());
#else /* ifndef SST */
    QALIGN(dest, alignedaddr);
    PIM_feb_empty((void *)alignedaddr);
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

int API_FUNC qthread_fill(const aligned_t *dest)
{                      /*{{{ */
    const aligned_t *alignedaddr;

    if (qlib == NULL) {
        return QTHREAD_SUCCESS;
    }
#ifndef SST
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(dest);
    qthread_shepherd_t *shep    = qthread_internal_getshep();

    assert(qthread_library_initialized);

    if (!shep) {
        return qthread_feb_blocker_func((void *)dest, NULL, FILL);
    }
    qthread_debug(FEB_CALLS, "dest=%p (tid=%i)\n", dest, qthread_id());
    QALIGN(dest, alignedaddr);
    /* lock hash */
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
        if (!m) {
            /* already full */
            break;
        }
        hazardous_ptr(0, m);
        if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
        if (!m->valid) { continue; }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            continue;
        }
        break;
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {                      /* BEGIN CRITICAL SECTION */
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&m->lock);
        }
    }                              /* END CRITICAL SECTION */
    qt_hash_unlock(FEBs[lockbin]); /* unlock hash */
# endif /* ifdef LOCK_FREE_FEBS */
    if (m) {
        /* if dest wasn't in the hash, it was already full. Since it was,
         * we need to fill it. */
        qthread_debug(FEB_BEHAVIOR, "dest=%p (tid=%i): filling, maybe alerting waiters\n", dest, qthread_id());
        qthread_gotlock_fill(shep, m, (void *)alignedaddr, 0, NULL);
    }
    qthread_debug(FEB_DETAILS, "dest=%p (tid=%i): success\n", dest, qthread_id());
#else /* ifndef SST */
    QALIGN(dest, alignedaddr);
    PIM_feb_fill((unsigned int *)alignedaddr);
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

/* the way this works is that:
 * 1 - data is copies from src to destination
 * 2 - the destination's FEB state gets changed from empty to full
 */

int API_FUNC qthread_writeF(aligned_t *restrict       dest,
                            const aligned_t *restrict src)
{                      /*{{{ */
    aligned_t *alignedaddr;

    qthread_debug(FEB_CALLS, "dest=%p, src=%p\n", dest, src);
#ifndef SST
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(dest);
    qthread_shepherd_t *shep    = qthread_internal_getshep();

    assert(qthread_library_initialized);

    if (!shep) {
        return qthread_feb_blocker_func(dest, (void *)src, WRITEF);
    }
    qthread_debug(FEB_BEHAVIOR, "tid %u dest=%p src=%p...\n", (shep->current) ? (shep->current->thread_id) : UINT_MAX, dest, src);
    QALIGN(dest, alignedaddr);
    QTHREAD_FEB_UNIQUERECORD2(feb, dest, shep);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        qthread_addrstat_t *m2;
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
got_m:
        if (!m) { /* already full */ break; }
        hazardous_ptr(0, m);
        if (m != (m2 = qt_hash_get(FEBs[lockbin], (void *)alignedaddr))) {
            m = m2;
            goto got_m;
        }
        if (!m->valid) { continue; }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            continue;
        }
        break;
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]); {    /* lock hash */
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&m->lock);
        }
    }
    qt_hash_unlock(FEBs[lockbin]);    /* unlock hash */
# endif /* ifdef LOCK_FREE_FEBS */
    /* we have the lock on m, so... */
    if (dest && (dest != src)) {
        memcpy(dest, src, sizeof(aligned_t));
    }
    qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", (shep->current) ? (shep->current->thread_id) : UINT_MAX, dest, src);
    if (m) {
        qthread_gotlock_fill(shep, m, alignedaddr, 0, NULL);
    }
#else /* ifndef SST */
    QALIGN(dest, alignedaddr);
    PIM_feb_empty((void *)alignedaddr);
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

int API_FUNC qthread_writeF_const(aligned_t *dest,
                                  aligned_t  src)
{                      /*{{{ */
    return qthread_writeF(dest, &src);
}                      /*}}} */

/* the way this works is that:
 * 1 - destination's FEB state must be "empty"
 * 2 - data is copied from src to destination
 * 3 - the destination's FEB state gets changed from empty to full
 */

int API_FUNC qthread_writeEF(aligned_t *restrict       dest,
                             const aligned_t *restrict src)
{                      /*{{{ */
    aligned_t *alignedaddr;

#ifndef SST
    qthread_addrstat_t *m;
    qthread_addrres_t  *X       = NULL;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(dest);
    qthread_t          *me      = qthread_internal_self();

    QTHREAD_FEB_TIMER_DECLARATION(febblock);

    assert(qthread_library_initialized);

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, WRITEEF);
    }
    qthread_debug(FEB_CALLS, "dest=%p, src=%p(%u) (tid=%i)\n", dest, src, (unsigned)*src, me->thread_id);
    QTHREAD_FEB_UNIQUERECORD(feb, dest, me);
    QTHREAD_FEB_TIMER_START(febblock);
    QALIGN(dest, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
got_m:
        if (!m) {
            /* currently full, must add to hash to wait */
            m = qthread_addrstat_new();
            if (!m) {
                // qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): MALLOC FAILURE!!!!!!!!!!\n", dest, src, me->thread_id);
                return QTHREAD_MALLOC_ERROR;
            }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!qt_hash_put(FEBs[lockbin], (void *)alignedaddr, m)) {
                // qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): put failure\n", dest, src, me->thread_id);
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                qthread_addrstat_delete(m);
                continue;
            }
            break;
        } else {
            qthread_addrstat_t *m2;

            /* could be either full or not, don't know */
            hazardous_ptr(0, m);
            if ((m2 = qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) != m) {
                // qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): pointer changed! (%p != %p)\n", dest, src, me->thread_id, m, m2);
                m = m2;
                goto got_m;
            }
            if (!m->valid) {
                // qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): m(%p) invalid!\n", dest, src, me->thread_id, m);
                continue;
            }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                // qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): m(%p) invalid!\n", dest, src, me->thread_id, m);
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            break;
        }
    } while(1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (!m) {
            m = qthread_addrstat_new();
            if (!m) {
                qt_hash_unlock(FEBs[lockbin]);
                return QTHREAD_MALLOC_ERROR;
            }
            qassertnot(qt_hash_put_locked(FEBs[lockbin], alignedaddr, m), 0);
        }
        QTHREAD_FASTLOCK_LOCK(&(m->lock));
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    assert(m);
    qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): data structure locked, m(%p)->full = %i\n", dest, src, me->thread_id, m, m->full);
    /* by this point m is locked */
    if (m->full == 1) {            /* full, thus, we must block */
        QTHREAD_WAIT_TIMER_DECLARATION;
        X = ALLOC_ADDRRES();
        if (X == NULL) {
            qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): MALLOC ERROR!!!!!!!!!!!!!!!!!!!!!!\n", dest, src, me->thread_id);
            QTHREAD_FASTLOCK_UNLOCK(&(m->lock));
            return QTHREAD_MALLOC_ERROR;
        }
        X->addr   = (aligned_t *)src;
        X->waiter = me;
        X->next   = m->EFQ;
        m->EFQ    = X;
        qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%i): back to parent (m=%p, X=%p, slice=%u)\n", dest, src, me->thread_id, m, X, lockbin);
        me->thread_state          = QTHREAD_STATE_FEB_BLOCKED;
        me->rdata->blockedon.addr = m;
        QTHREAD_WAIT_TIMER_START();
        qthread_back_to_master(me);
        QTHREAD_WAIT_TIMER_STOP(me, febwait);
        qthread_debug(FEB_BEHAVIOR, "dest=%p, src=%p (tid=%i): succeeded after waiting\n", dest, src, me->thread_id);
    } else {
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "dest=%p, src=%p (tid=%i): succeeded! waking waiters...\n", dest, src, me->thread_id);
        qthread_gotlock_fill(me->rdata->shepherd_ptr, m, alignedaddr, 0, NULL);
    }
    QTHREAD_FEB_TIMER_STOP(febblock, me);
#else /* ifndef SST */
    QALIGN(dest, alignedaddr);
    while (PIM_feb_try_writeef(alignedaddr, src) == 1) {
        qthread_yield(me);
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

int API_FUNC qthread_writeEF_const(aligned_t *dest,
                                   aligned_t  src)
{                      /*{{{ */
    return qthread_writeEF(dest, &src);
}                      /*}}} */

int INTERNAL qthread_writeEF_nb(aligned_t *restrict       dest,
                                const aligned_t *restrict src)
{                      /*{{{ */
    aligned_t *alignedaddr;

    qthread_debug(FEB_CALLS, "dest=%p, src=%p\n", dest, src);
#ifndef SST
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(dest);
    qthread_t          *me      = qthread_internal_self();

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, WRITEEF);
    }
    qthread_debug(FEB_BEHAVIOR, "tid %u dest=%p src=%p...\n", me->thread_id, dest, src);
    QTHREAD_FEB_UNIQUERECORD(feb, dest, me);
    QALIGN(dest, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            /* could be either full or not, don't know */
            hazardous_ptr(0, m);
            if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
            if (!m->valid) { continue; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            break;
        }
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&(m->lock));
        }
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    qthread_debug(FEB_DETAILS, "data structure locked\n");
    /* by this point m is locked */
    qthread_debug(FEB_DETAILS, "m->full == %i\n", m->full);
    if ((m == NULL) || (m->full == 1)) {            /* full, thus, we must block */
        qthread_debug(FEB_BEHAVIOR, "tid %u non-blocking fail\n", me->thread_id);
        if (m) { QTHREAD_FASTLOCK_UNLOCK(&(m->lock)); }
        return QTHREAD_OPFAIL;
    } else {
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", me->thread_id, dest, src);
        qthread_gotlock_fill(me->rdata->shepherd_ptr, m, alignedaddr, 0, NULL);
    }
#else /* ifndef SST */
    QALIGN(dest, alignedaddr);
    while (PIM_feb_try_writeef(alignedaddr, src) == 1) {
        return QTHREAD_OPFAIL;
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

int INTERNAL qthread_writeEF_const_nb(aligned_t *dest,
                                      aligned_t  src)
{                      /*{{{ */
    return qthread_writeEF_nb(dest, &src);
}                      /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 */

int API_FUNC qthread_readFF(aligned_t *restrict       dest,
                            const aligned_t *restrict src)
{                      /*{{{ */
    const aligned_t *alignedaddr;

#ifndef SST
    qthread_addrstat_t *m       = NULL;
    qthread_addrres_t  *X       = NULL;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(src);
    qthread_t          *me      = qthread_internal_self();

    QTHREAD_FEB_TIMER_DECLARATION(febblock);

    assert(qthread_library_initialized);

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, READFF);
    }
    qthread_debug(FEB_CALLS, "dest=%p, src=%p (tid=%u)\n", dest, src, me->thread_id);
    QTHREAD_FEB_UNIQUERECORD(feb, src, me);
    QTHREAD_FEB_TIMER_START(febblock);
    QALIGN(src, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
        if (!m) { break; }
        hazardous_ptr(0, m);
        if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
        if (!m->valid) { continue; }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            continue;
        }
        break;
    } while(1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&m->lock);
        }
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%u): data structure locked or null (m=%p)\n", dest, src, me->thread_id, m);
    /* now m, if it exists, is locked - if m is NULL, then we're done! */
    if (m == NULL) {               /* already full! */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "dest=%p, src=%p (tid=%u): non-blocking success!\n", dest, src, me->thread_id);
    } else if (m->full != 1) {         /* not full... so we must block */
        QTHREAD_WAIT_TIMER_DECLARATION;
        X = ALLOC_ADDRRES();
        if (X == NULL) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            return QTHREAD_MALLOC_ERROR;
        }
        X->addr   = (aligned_t *)dest;
        X->waiter = me;
        X->next   = m->FFQ;
        m->FFQ    = X;
        qthread_debug(FEB_DETAILS, "dest=%p, src=%p (tid=%u): back to parent\n", dest, src, me->thread_id);
        me->thread_state          = QTHREAD_STATE_FEB_BLOCKED;
        me->rdata->blockedon.addr = m;
        QTHREAD_WAIT_TIMER_START();
        qthread_back_to_master(me);
        QTHREAD_WAIT_TIMER_STOP(me, febwait);
        qthread_debug(FEB_BEHAVIOR, "dest=%p, src=%p (tid=%u): succeeded after waiting\n", dest, src, me->thread_id);
    } else {                   /* exists AND is empty... weird, but that's life */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "dest=%p, src=%p (tid=%u): succeeded!\n", dest, src, me->thread_id);
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
    }
    QTHREAD_FEB_TIMER_STOP(febblock, me);
#else /* ifndef SST */
    QALIGN(src, alignedaddr);
    while (PIM_feb_try_readff(dest, alignedaddr) == 1) {
        qthread_yield(me);
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

int INTERNAL qthread_readFF_nb(aligned_t *restrict       dest,
                               const aligned_t *restrict src)
{                      /*{{{ */
    const aligned_t *alignedaddr;

    qthread_debug(FEB_CALLS, "dest=%p, src=%p\n", dest, src);
#ifndef SST
    qthread_addrstat_t *m       = NULL;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(src);
    qthread_t          *me      = qthread_internal_self();

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, READFF_NB);
    }
    qthread_debug(FEB_BEHAVIOR, "tid %u dest=%p src=%p...\n", me->thread_id, dest, src);
    QTHREAD_FEB_UNIQUERECORD(feb, src, me);
    QALIGN(src, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        qthread_addrstat_t *m2;
        m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
got_m:
        if (!m) { break; }
        hazardous_ptr(0, m);
        if (m != (m2 = qt_hash_get(FEBs[lockbin], (void *)alignedaddr))) {
            m = m2;
            goto got_m;
        }
        if (!m->valid) { continue; }
        QTHREAD_FASTLOCK_LOCK(&m->lock);
        if (!m->valid) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            continue;
        }
        break;
    } while(1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
        if (m) {
            QTHREAD_FASTLOCK_LOCK(&m->lock);
        }
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    qthread_debug(FEB_DETAILS, "data structure locked\n");
    /* now m, if it exists, is locked - if m is NULL, then we're done! */
    if (m == NULL) {               /* already full! */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", me->thread_id, dest, src);
    } else if (m->full != 1) {         /* not full... so we must block */
        qthread_debug(FEB_BEHAVIOR, "tid %u non-blocking fail\n", me->thread_id);
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        return QTHREAD_OPFAIL;
    } else {                   /* exists AND is empty... weird, but that's life */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", me->thread_id, dest, src);
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
    }
#else /* ifndef SST */
    QALIGN(src, alignedaddr);
    while (PIM_feb_try_readff(dest, alignedaddr) == 1) {
        return QTHREAD_OPFAIL;
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 * 3 - the src's FEB bits get changed from full to empty
 */

int API_FUNC qthread_readFE(aligned_t *restrict       dest,
                            const aligned_t *restrict src)
{                      /*{{{ */
    const aligned_t *alignedaddr;

#ifndef SST
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(src);
    qthread_t          *me      = qthread_internal_self();

    QTHREAD_FEB_TIMER_DECLARATION(febblock);

    assert(qthread_library_initialized);

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, READFE);
    }
    assert(me->rdata);
    qthread_debug(FEB_CALLS, "dest=%p, src=%p (tid=%i)\n", dest, src, me->thread_id);
    QTHREAD_FEB_UNIQUERECORD(feb, src, me);
    QTHREAD_FEB_TIMER_START(febblock);
    QALIGN(src, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], alignedaddr);
got_m:
        if (!m) {
            /* currently full; need to set to empty */
            m = qthread_addrstat_new();
            if (!m) { return QTHREAD_MALLOC_ERROR; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!qt_hash_put(FEBs[lockbin], alignedaddr, m)) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                qthread_addrstat_delete(m);
                continue;
            }
            break;
        } else {
            qthread_addrstat_t *m2;
            /* could be full or not, don't know */
            hazardous_ptr(0, m);
            if (m != (m2 = qt_hash_get(FEBs[lockbin], (void *)alignedaddr))) {
                m = m2;
                goto got_m;
            }
            if (!m->valid) { continue; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            break;
        }
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], alignedaddr);
        if (!m) {
            m = qthread_addrstat_new();
            if (!m) {
                qt_hash_unlock(FEBs[lockbin]);
                return QTHREAD_MALLOC_ERROR;
            }
            qassertnot(qt_hash_put_locked(FEBs[lockbin], alignedaddr, m), 0);
        }
        QTHREAD_FASTLOCK_LOCK(&(m->lock));
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    assert(m);
    qthread_debug(FEB_DETAILS, "data structure locked\n");
    /* by this point m is locked */
    if (m->full == 0) {            /* empty, thus, we must block */
        QTHREAD_WAIT_TIMER_DECLARATION;
        qthread_addrres_t *X = ALLOC_ADDRRES();

        if (X == NULL) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            return QTHREAD_MALLOC_ERROR;
        }
        X->addr   = (aligned_t *)dest;
        X->waiter = me;
        X->next   = m->FEQ;
        m->FEQ    = X;
        qthread_debug(FEB_DETAILS, "back to parent\n");
        me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
        /* so that the shepherd will unlock it */
        me->rdata->blockedon.addr = m;
        QTHREAD_WAIT_TIMER_START();
        qthread_back_to_master(me);
        QTHREAD_WAIT_TIMER_STOP(me, febwait);
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p after waiting\n", me->thread_id, dest, src);
    } else {                   /* full, thus IT IS OURS! MUAHAHAHA! */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", me->thread_id, dest, src);
        qthread_gotlock_empty(me->rdata->shepherd_ptr, m, (void *)alignedaddr, 0, NULL);
    }
    QTHREAD_FEB_TIMER_STOP(febblock, me);
#else /* ifndef SST */
    QALIGN(src, alignedaddr);
    while (PIM_feb_try_readfe(dest, alignedaddr) == 1) {
        qthread_yield(me);
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

/* This is the non-blocking version of the previous one */
int INTERNAL qthread_readFE_nb(aligned_t *restrict       dest,
                               const aligned_t *restrict src)
{                      /*{{{ */
    const aligned_t *alignedaddr;

    qthread_debug(FEB_CALLS, "dest=%p, src=%p\n", dest, src);
#ifndef SST
    qthread_addrstat_t *m;
    const int           lockbin = QTHREAD_CHOOSE_STRIPE2(src);
    qthread_t          *me      = qthread_internal_self();

    if (!me) {
        return qthread_feb_blocker_func(dest, (void *)src, READFE_NB);
    }
    qthread_debug(FEB_BEHAVIOR, "tid %u dest=%p src=%p...\n", me->thread_id, dest, src);
    QTHREAD_FEB_UNIQUERECORD(feb, src, me);
    QALIGN(src, alignedaddr);
    QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
# ifdef LOCK_FREE_FEBS
    do {
        m = qt_hash_get(FEBs[lockbin], alignedaddr);
        if (!m) {
            /* currently full; need to set to empty */
            m = qthread_addrstat_new();
            if (!m) { return QTHREAD_MALLOC_ERROR; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!qt_hash_put(FEBs[lockbin], alignedaddr, m)) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                qthread_addrstat_delete(m);
                continue;
            }
            break;
        } else {
            /* could be full or not, don't know */
            hazardous_ptr(0, m);
            if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
            if (!m->valid) { continue; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            break;
        }
    } while (1);
# else /* ifdef LOCK_FREE_FEBS */
    qt_hash_lock(FEBs[lockbin]);
    {
        m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], alignedaddr);
        if (!m) {
            m = qthread_addrstat_new();
            if (!m) {
                qt_hash_unlock(FEBs[lockbin]);
                return QTHREAD_MALLOC_ERROR;
            }
            qassertnot(qt_hash_put_locked(FEBs[lockbin], alignedaddr, m), 0);
        }
        QTHREAD_FASTLOCK_LOCK(&(m->lock));
    }
    qt_hash_unlock(FEBs[lockbin]);
# endif /* ifdef LOCK_FREE_FEBS */
    qthread_debug(FEB_DETAILS, "data structure locked\n");
    /* by this point m is locked */
    if (m->full == 0) {            /* empty, thus, we must fail */
        qthread_debug(FEB_BEHAVIOR, "tid %u non-blocking fail\n", me->thread_id);
        QTHREAD_FASTLOCK_UNLOCK(&m->lock);
        return QTHREAD_OPFAIL;
    } else {                   /* full, thus IT IS OURS! MUAHAHAHA! */
        if (dest && (dest != src)) {
            *(aligned_t *)dest = *(aligned_t *)src;
            MACHINE_FENCE;
        }
        qthread_debug(FEB_BEHAVIOR, "tid %u succeeded on %p=%p\n", me->thread_id, dest, src);
        qthread_gotlock_empty(me->rdata->shepherd_ptr, m, (void *)alignedaddr, 0, NULL);
    }
#else /* ifndef SST */
    QALIGN(src, alignedaddr);
    if (PIM_feb_try_readfe(dest, alignedaddr) == 1) {
        return QTHREAD_OPFAIL;
    }
#endif /* ifndef SST */
    return QTHREAD_SUCCESS;
}                      /*}}} */

#ifdef QTHREAD_COUNT_THREADS
extern aligned_t             threadcount;
extern aligned_t             maxconcurrentthreads;
extern double                avg_concurrent_threads;
extern aligned_t             maxeffconcurrentthreads;
extern double                avg_eff_concurrent_threads;
extern aligned_t             effconcurrentthreads;
extern aligned_t             concurrentthreads;
extern QTHREAD_FASTLOCK_TYPE concurrentthreads_lock;
extern QTHREAD_FASTLOCK_TYPE effconcurrentthreads_lock;
#endif
/*
 * This function walks the list of preconditions. When an empty variable is
 * encountered, it enqueues the "nascent" qthread in the associated FFQ. When
 * all preconditions are satisfied, the qthread state is set as "new".
 *
 * This is a modified readFF() that does not suspend the calling thread, but
 * simply enqueues the specified qthread in the FFQ associated with the target.
 */
int INTERNAL qthread_check_feb_preconds(qthread_t *t)
{   /*{{{*/
    aligned_t **these_preconds = (aligned_t **)t->preconds;

#if defined(QTHREAD_FEB_PROFILING)
    qthread_shepherd_t *const curshep = qthread_internal_getshep();
#endif
    QTHREAD_FEB_TIMER_DECLARATION(febblock);

    qthread_debug(FEB_FUNCTIONS, "t=%p, t->tid=%u\n", t, t->thread_id);
    assert(qthread_library_initialized);

    // Process input preconds
    while (these_preconds && (these_preconds[0] != NULL)) {
        aligned_t          *this_sync = these_preconds[(uintptr_t)these_preconds[0]];
        const int           lockbin   = QTHREAD_CHOOSE_STRIPE2(this_sync);
        const aligned_t    *alignedaddr;
        qthread_addrstat_t *m = NULL;

        QTHREAD_FEB_UNIQUERECORD2(feb, this_sync, curshep);
        QTHREAD_FEB_TIMER_START(febblock);
        QALIGN(this_sync, alignedaddr);
        QTHREAD_COUNT_THREADS_BINCOUNTER(febs, lockbin);
#ifdef LOCK_FREE_FEBS
        do {
            m = qt_hash_get(FEBs[lockbin], (void *)alignedaddr);
            if (!m) { break; }
            hazardous_ptr(0, m);
            if (m != qt_hash_get(FEBs[lockbin], (void *)alignedaddr)) { continue; }
            if (!m->valid) { continue; }
            QTHREAD_FASTLOCK_LOCK(&m->lock);
            if (!m->valid) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                continue;
            }
            break;
        } while(1);
#else   /* ifdef LOCK_FREE_FEBS */
        qt_hash_lock(FEBs[lockbin]);
        {
            m = (qthread_addrstat_t *)qt_hash_get_locked(FEBs[lockbin], (void *)alignedaddr);
            if (m) {
                QTHREAD_FASTLOCK_LOCK(&m->lock);
            }
        }
        qt_hash_unlock(FEBs[lockbin]);
#endif  /* ifdef LOCK_FREE_FEBS */
        qthread_debug(FEB_DETAILS, "precond=%p (tid=%u): data structure locked or null (m=%p, lockbin=%u)\n", this_sync, t->thread_id, m, lockbin);
        if (m == NULL) { /* already full! */
            these_preconds[0] = (aligned_t *)(((uintptr_t)these_preconds[0]) - 1);
        } else if (m->full == 1) {
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            these_preconds[0] = (aligned_t *)(((uintptr_t)these_preconds[0]) - 1);
        } else {
            // Need to wait on this one, add to appropriate FFQ
            qthread_addrres_t *X = NULL;

            X = ALLOC_ADDRRES();
            if (X == NULL) {
                QTHREAD_FASTLOCK_UNLOCK(&m->lock);
                abort();
                return QTHREAD_MALLOC_ERROR;
            }
            X->addr         = NULL;
            X->waiter       = t;
            X->next         = m->FFQ;
            m->FFQ          = X;
            t->thread_state = QTHREAD_STATE_NASCENT;
            QTHREAD_FASTLOCK_UNLOCK(&m->lock);
            return 1;
        }
        qthread_debug(FEB_DETAILS, "precond=%p (tid=%u): address already full (m=%p, val=%u)\n", this_sync, t->thread_id, m, *(aligned_t *)this_sync);
    }

    // All input preconds are full
    t->thread_state = QTHREAD_STATE_NEW;
#ifdef QTHREAD_COUNT_THREADS
    QTHREAD_FASTLOCK_LOCK(&concurrentthreads_lock);
    threadcount++;
    concurrentthreads++;
    assert(concurrentthreads <= threadcount);
    if (concurrentthreads > maxconcurrentthreads) {
        maxconcurrentthreads = concurrentthreads;
    }
    avg_concurrent_threads =
        (avg_concurrent_threads * (double)(threadcount - 1.0) / threadcount)
        + ((double)concurrentthreads / threadcount);
    QTHREAD_FASTLOCK_UNLOCK(&concurrentthreads_lock);
#endif /* ifdef QTHREAD_COUNT_THREADS */

    return 0;
} /*}}}*/

static int qt_feb_tf_call_cb(const qt_key_t            addr,
                             qthread_t *const restrict waiter,
                             void *restrict            tf_arg)
{   /*{{{*/
    qt_feb_callback_f f     = (qt_feb_callback_f)((void **)tf_arg)[0];
    void             *f_arg = ((void **)tf_arg)[1];
    void             *tls;

    if (waiter->rdata->tasklocal_size <= qlib->qthread_tasklocal_size) {
        if (waiter->flags & QTHREAD_BIG_STRUCT) {
            tls = &waiter->data[qlib->qthread_argcopy_size];
        } else {
            tls = waiter->data;
        }
    } else {
        if (waiter->flags & QTHREAD_BIG_STRUCT) {
            tls = *(void **)&waiter->data[qlib->qthread_argcopy_size];
        } else {
            tls = *(void **)&waiter->data[0];
        }
    }
    f((void *)addr, waiter->f, waiter->arg, waiter->ret, waiter->thread_id, tls, f_arg);
    return 0;
} /*}}}*/

static void qt_feb_call_tf(const qt_key_t      addr,
                           qthread_addrstat_t *m,
                           void               *arg)
{   /*{{{*/
    qt_feb_taskfilter_f tf    = (qt_feb_taskfilter_f)((void **)arg)[0];
    void               *f_arg = ((void **)arg)[1];

    QTHREAD_FASTLOCK_LOCK(&m->lock);
    for (int i = 0; i < 3; i++) {
        qthread_addrres_t *curs, **base;
        switch (i) {
            case 0: curs = m->EFQ; base = &m->EFQ; break;
            case 1: curs = m->FEQ; base = &m->FEQ; break;
            case 2: curs = m->FFQ; base = &m->FFQ; break;
        }
        for (; curs != NULL; curs = curs->next) {
            qthread_t *waiter = curs->waiter;
            void      *tls;
            switch(tf(addr, waiter, f_arg)) {
                case 0: // ignore, move to the next one
                    base = &curs->next;
                    break;
                case 2: // remove, move to the next one
                {
                    qthread_internal_assassinate(waiter);
                    *base = curs->next;
                    FREE_ADDRRES(curs);
                    break;
                }
                default:
                    QTHREAD_TRAP();
            }
        }
    }
    QTHREAD_FASTLOCK_UNLOCK(&m->lock);
} /*}}}*/

void INTERNAL qthread_feb_taskfilter(qt_feb_taskfilter_f tf,
                                     void               *arg)
{   /*{{{*/
    void *pass[2] = { tf, arg };

    for (unsigned int i = 0; i < QTHREAD_LOCKING_STRIPES; i++) {
        qt_hash_callback(FEBs[i], (qt_hash_callback_fn)qt_feb_call_tf, pass);
    }
} /*}}}*/

void INTERNAL qthread_feb_callback(qt_feb_callback_f cb,
                                   void             *arg)
{   /*{{{*/
    void *pass[2] = { cb, arg };

    qthread_feb_taskfilter(qt_feb_tf_call_cb, pass);
} /*}}}*/

/* vim:set expandtab: */
