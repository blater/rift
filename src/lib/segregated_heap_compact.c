#include "segregated_heap_internal.h"

#ifdef RIFT_HEAP_COMPACTION_STATS
rift_heap_compact_status rift_heap_compact_routine(
    rift_heap_compact_report *report) {
  return rift_heap_private_compact_slice(report);
}
#endif
