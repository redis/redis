/* LAZY FREE - a type aware wrapper for Background Job Manager (BJM)
 *
 * LAZY FREE is a wrapper around BJM which supports lazy free operations for
 * fundamental Redis types (dict, list, rax, robj).  The utility is type aware
 * and determines if items should be released on the main thread or a background
 * thread based on the effort required (size) of the item.
 * 
 * A generic capability is provided for higher-level (non-fundamental) types.
 * The generic capability exists primarily to allow other types to be included
 * in the metrics.  The decision regarding effort is left to the caller, so
 * generic operations are ALWAYS executed via BJM.
 */
#ifndef __LAZYFREE_H__
#define __LAZYFREE_H__

#include "server.h"
#include "bjm.h"


/* For small items, passing the item to a background thread is more work than
 * just freeing immediately.  This value specifies the "effort" required before
 * sending to BJM.  Effort is a fairly arbitrary value that loosely corresponds
 * to the number of allocations in the object.  Example, for dictionaries and
 * lists, this corresponds to the size/length of the item.
 * 
 * Specified in the header in-case other code would like to use this when
 * assessing higher-level type for lazy free.
 */
#define LAZYFREE_THRESHOLD 64


///////////////////////////////
// INITIALIZATION AND METRICS
///////////////////////////////

/* Initialize lazyfree. */
void lazyfreeInit();

/* Get count of items queued or in-progress. */
size_t lazyfreeGetPendingObjectsCount(void);

/* Cumulative number of items that have completed lazyfree operation in the
 * background thread.  Doesn't include small obects freed on main thread. */
size_t lazyfreeGetFreedObjectsCount(void);

/* Reset the cumulative counter.  */
void lazyfreeResetStats(void);


///////////////////////////////
// LAZYFREE OF FUNDAMENTAL TYPES
// Note:  Only fundamental, low-level, types should be included here.
//        Higher-level constructs should be maintained in the proper supporting
//        module.
///////////////////////////////

/* LazyFree an arbitrary robj.
 * This does NOT support modules.  Modules may require DBID/KEY info.  This
 * function does not handle items which are currently in the main dictionary, 
 * so it can't possibly provide DBID/KEY to a module.
 * 
 * May short-circuit BJM for small items, if so, metrics will be untouched. */
void lazyfreeObject(robj *o);   // For non-module robjs only

/* LazyFree a dictionary, invoking the dictionary's free function (if any) for
 * each item.
 * 
 * May short-circuit BJM for small items, if so, metrics will be untouched. */ 
void lazyfreeDict(dict *d);

/* LazyFree a list, invoking the list's free function (if any) for each item. */
void lazyfreeList(list *l);

/* LazyFree a RAX.  Since RAX does support a configured free callback one may be
 * provided here.  The free_callback will be passed the data item attached to
 * each RAX entry.
 * 
 * May short-circuit BJM for small items, if so, metrics will be untouched. */ 
void lazyfreeRax(rax *r);
void lazyfreeRaxWithCallback(rax *r, void (*free_callback)(void*));


///////////////////////////////
// GENERIC FUNCTION FOR ARBITRARY LAZY FREE OPERATIONS
///////////////////////////////

/* Execute an arbitrary BJM function on the background thread while maintaining
 * lazyfree metrics.
 *  - lazyfreeGeneric will increase metrics by "cardinality"
 *  - lazyfreeGenericComplete will adjust processed metrics and "cardinality"
 *    MUST match the cardinality provided to lazyfreeGeneric.
 * 
 * NOTE:  Other than for inclusion in the lazyfree metrics, this interface adds
 *        no functionality.  Unless inclusion in lazyfree metrics is required, 
 *        it is recommended to invoke BJM directly.
 */
void lazyfreeGeneric(long cardinality, bjmJobFuncHandle func, void *item);
void lazyfreeGenericComplete(long cardinality);

#endif
