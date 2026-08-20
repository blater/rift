#ifndef RIFT_SEGREGATED_HEAP_INTERNAL_H
#define RIFT_SEGREGATED_HEAP_INTERNAL_H

#include "pools.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(RIFT_HEAP_ROUTINE_COMPACTION)
#error segregated_heap_internal.h is private to the routine compaction build
#endif
#ifdef __SDCC
#error routine compaction movement is deferred on ZX Next
#endif

#define RIFT_HEAP_ROUTINE_HEADER_BUDGET 8u
#define RIFT_HEAP_ROUTINE_MOVE_BUDGET 1u
#define RIFT_HEAP_ROUTINE_COPY_BUDGET 192u
typedef enum rift_heap_move_class {
  RIFT_HEAP_MOVE_MOVABLE = 0,
  RIFT_HEAP_MOVE_PIN_BARRIER,
  RIFT_HEAP_MOVE_RAW_BARRIER,
  RIFT_HEAP_MOVE_INVALID
} rift_heap_move_class;

typedef enum rift_heap_compact_status {
  RIFT_HEAP_COMPACT_IDLE = 0,
  RIFT_HEAP_COMPACT_PROGRESS,
  RIFT_HEAP_COMPACT_PASS_COMPLETE,
  RIFT_HEAP_COMPACT_INVALID
} rift_heap_compact_status;

#ifdef RIFT_HEAP_COMPACTION_STATS
typedef struct rift_heap_compact_report {
  unsigned char headers_scanned;
  unsigned char moves;
  unsigned char index_reads;
  unsigned char index_writes;
  unsigned char bin_edits;
  unsigned char boundary_writes;
  unsigned char frontier_writes;
  unsigned char pin_barriers;
  unsigned char raw_barriers;
  unsigned char budget_barriers;
  uint16_t copied_bytes;
  uint16_t tail_contracted;
} rift_heap_compact_report;
#endif

typedef struct rift_heap_move_ticket {
  rift_pool_offset_t owner;
  unsigned char slot;
  unsigned char level;
#ifdef RIFT_HEAP_COMPACTION_STATS
  unsigned char index_reads;
  unsigned char index_writes;
#endif
} rift_heap_move_ticket;

extern unsigned char rift_managed_maintenance_due;
void managed_heap_maintenance_safepoint(void);
#define managed_maintenance_safepoint()                                  \
  do {                                                                   \
    if (rift_managed_maintenance_due) managed_heap_maintenance_safepoint(); \
  } while (0)

#ifdef RIFT_HEAP_COMPACTION_STATS
rift_heap_compact_status rift_heap_private_compact_slice(
    rift_heap_compact_report *report);
#else
rift_heap_compact_status rift_heap_private_compact_slice(void);
#endif
void rift_heap_movement_barrier_changed(void);

#ifdef RIFT_HEAP_COMPACTION_STATS
rift_heap_compact_status rift_heap_compact_routine(
    rift_heap_compact_report *report);
#endif

rift_heap_move_class rift_managed_move_prepare(
    void *raw_payload, rift_pool_offset_t live_tag,
    rift_heap_move_ticket *ticket);
void rift_managed_move_commit(void *old_raw_payload, void *new_raw_payload,
                              rift_pool_offset_t live_tag,
                              const rift_heap_move_ticket *ticket);

#ifdef RIFT_ALLOCATOR_TEST
uint16_t rift_heap_test_movement_epoch(void);
void rift_heap_test_set_movement_epoch(uint16_t epoch);
size_t rift_heap_test_cursor_scan(void);
unsigned int rift_heap_test_compaction_debt(void);
#endif

#endif
