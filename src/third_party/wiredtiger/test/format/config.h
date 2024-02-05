/* DO NOT EDIT: automatically built by format/config.sh. */

#define C_TYPE_MATCH(cp, type)                                                                    \
    (!F_ISSET(cp, (C_TYPE_FIX | C_TYPE_ROW | C_TYPE_VAR)) ||                                      \
      ((type) == FIX && F_ISSET(cp, C_TYPE_FIX)) || ((type) == ROW && F_ISSET(cp, C_TYPE_ROW)) || \
      ((type) == VAR && F_ISSET(cp, C_TYPE_VAR)))

typedef struct {
    const char *name; /* Configuration item */
    const char *desc; /* Configuration description */

#define C_BOOL 0x001u        /* Boolean (true if roll of 1-to-100 is <= CONFIG->min) */
#define C_IGNORE 0x002u      /* Not a simple randomization, configured specially */
#define C_STRING 0x004u      /* String (rather than integral) */
#define C_TABLE 0x008u       /* Value is per table, not global */
#define C_TYPE_FIX 0x010u    /* Value is only relevant to FLCS */
#define C_TYPE_LSM 0x020u    /* Value is only relevant to LSM */
#define C_TYPE_ROW 0x040u    /* Value is only relevant to RS */
#define C_TYPE_VAR 0x080u    /* Value is only relevant to VLCS */
#define C_ZERO_NOTSET 0x100u /* Ignore zero values */
    uint32_t flags;

    uint32_t min;     /* Minimum value */
    uint32_t maxrand; /* Maximum value randomly chosen */
    uint32_t maxset;  /* Maximum value explicitly set */

    u_int off; /* Value offset */
} CONFIG;

#define V_MAX_TABLES_CONFIG 1000

#define V_GLOBAL_ASSERT_READ_TIMESTAMP 0
#define V_GLOBAL_ASSERT_WRITE_TIMESTAMP 1
#define V_GLOBAL_BACKUP 2
#define V_GLOBAL_BACKUP_INCREMENTAL 3
#define V_GLOBAL_BACKUP_INCR_GRANULARITY 4
#define V_GLOBAL_BLOCK_CACHE 5
#define V_GLOBAL_BLOCK_CACHE_CACHE_ON_CHECKPOINT 6
#define V_GLOBAL_BLOCK_CACHE_CACHE_ON_WRITES 7
#define V_GLOBAL_BLOCK_CACHE_SIZE 8
#define V_TABLE_BTREE_BITCNT 9
#define V_TABLE_BTREE_COMPRESSION 10
#define V_TABLE_BTREE_DICTIONARY 11
#define V_TABLE_BTREE_HUFFMAN_VALUE 12
#define V_TABLE_BTREE_INTERNAL_KEY_TRUNCATION 13
#define V_TABLE_BTREE_INTERNAL_PAGE_MAX 14
#define V_TABLE_BTREE_KEY_MAX 15
#define V_TABLE_BTREE_KEY_MIN 16
#define V_TABLE_BTREE_LEAF_PAGE_MAX 17
#define V_TABLE_BTREE_MEMORY_PAGE_MAX 18
#define V_TABLE_BTREE_PREFIX_LEN 19
#define V_TABLE_BTREE_PREFIX_COMPRESSION 20
#define V_TABLE_BTREE_PREFIX_COMPRESSION_MIN 21
#define V_TABLE_BTREE_REPEAT_DATA_PCT 22
#define V_TABLE_BTREE_REVERSE 23
#define V_TABLE_BTREE_SPLIT_PCT 24
#define V_TABLE_BTREE_VALUE_MAX 25
#define V_TABLE_BTREE_VALUE_MIN 26
#define V_GLOBAL_CACHE 27
#define V_GLOBAL_CACHE_EVICT_MAX 28
#define V_GLOBAL_CACHE_MINIMUM 29
#define V_GLOBAL_CHECKPOINT 30
#define V_GLOBAL_CHECKPOINT_LOG_SIZE 31
#define V_GLOBAL_CHECKPOINT_WAIT 32
#define V_TABLE_DISK_CHECKSUM 33
#define V_GLOBAL_DISK_DATA_EXTEND 34
#define V_GLOBAL_DISK_DIRECT_IO 35
#define V_GLOBAL_DISK_ENCRYPTION 36
#define V_TABLE_DISK_FIRSTFIT 37
#define V_GLOBAL_DISK_MMAP 38
#define V_GLOBAL_DISK_MMAP_ALL 39
#define V_GLOBAL_FILE_MANAGER_CLOSE_HANDLE_MINIMUM 40
#define V_GLOBAL_FILE_MANAGER_CLOSE_IDLE_TIME 41
#define V_GLOBAL_FILE_MANAGER_CLOSE_SCAN_INTERVAL 42
#define V_GLOBAL_FORMAT_ABORT 43
#define V_GLOBAL_FORMAT_INDEPENDENT_THREAD_RNG 44
#define V_GLOBAL_FORMAT_MAJOR_TIMEOUT 45
#define V_GLOBAL_IMPORT 46
#define V_GLOBAL_LOGGING 47
#define V_GLOBAL_LOGGING_ARCHIVE 48
#define V_GLOBAL_LOGGING_COMPRESSION 49
#define V_GLOBAL_LOGGING_FILE_MAX 50
#define V_GLOBAL_LOGGING_PREALLOC 51
#define V_TABLE_LSM_AUTO_THROTTLE 52
#define V_TABLE_LSM_BLOOM 53
#define V_TABLE_LSM_BLOOM_BIT_COUNT 54
#define V_TABLE_LSM_BLOOM_HASH_COUNT 55
#define V_TABLE_LSM_BLOOM_OLDEST 56
#define V_TABLE_LSM_CHUNK_SIZE 57
#define V_TABLE_LSM_MERGE_MAX 58
#define V_GLOBAL_LSM_WORKER_THREADS 59
#define V_GLOBAL_OPS_ALTER 60
#define V_GLOBAL_OPS_COMPACTION 61
#define V_GLOBAL_OPS_HS_CURSOR 62
#define V_TABLE_OPS_PCT_DELETE 63
#define V_TABLE_OPS_PCT_INSERT 64
#define V_TABLE_OPS_PCT_MODIFY 65
#define V_TABLE_OPS_PCT_READ 66
#define V_TABLE_OPS_PCT_WRITE 67
#define V_GLOBAL_OPS_PREPARE 68
#define V_GLOBAL_OPS_RANDOM_CURSOR 69
#define V_GLOBAL_OPS_SALVAGE 70
#define V_TABLE_OPS_TRUNCATE 71
#define V_GLOBAL_OPS_VERIFY 72
#define V_GLOBAL_QUIET 73
#define V_GLOBAL_RUNS_IN_MEMORY 74
#define V_GLOBAL_RUNS_OPS 75
#define V_TABLE_RUNS_ROWS 76
#define V_TABLE_RUNS_SOURCE 77
#define V_GLOBAL_RUNS_TABLES 78
#define V_GLOBAL_RUNS_THREADS 79
#define V_GLOBAL_RUNS_TIMER 80
#define V_TABLE_RUNS_TYPE 81
#define V_GLOBAL_RUNS_VERIFY_FAILURE_DUMP 82
#define V_GLOBAL_STATISTICS 83
#define V_GLOBAL_STATISTICS_SERVER 84
#define V_GLOBAL_STRESS_AGGRESSIVE_SWEEP 85
#define V_GLOBAL_STRESS_CHECKPOINT 86
#define V_GLOBAL_STRESS_CHECKPOINT_RESERVED_TXNID_DELAY 87
#define V_GLOBAL_STRESS_CHECKPOINT_PREPARE 88
#define V_GLOBAL_STRESS_FAILPOINT_EVICTION_FAIL_AFTER_RECONCILIATION 89
#define V_GLOBAL_STRESS_FAILPOINT_HS_DELETE_KEY_FROM_TS 90
#define V_GLOBAL_STRESS_HS_CHECKPOINT_DELAY 91
#define V_GLOBAL_STRESS_HS_SEARCH 92
#define V_GLOBAL_STRESS_HS_SWEEP 93
#define V_GLOBAL_STRESS_SPLIT_1 94
#define V_GLOBAL_STRESS_SPLIT_2 95
#define V_GLOBAL_STRESS_SPLIT_3 96
#define V_GLOBAL_STRESS_SPLIT_4 97
#define V_GLOBAL_STRESS_SPLIT_5 98
#define V_GLOBAL_STRESS_SPLIT_6 99
#define V_GLOBAL_STRESS_SPLIT_7 100
#define V_GLOBAL_TRANSACTION_IMPLICIT 101
#define V_GLOBAL_TRANSACTION_TIMESTAMPS 102
#define V_GLOBAL_WIREDTIGER_CONFIG 103
#define V_GLOBAL_WIREDTIGER_RWLOCK 104
#define V_GLOBAL_WIREDTIGER_LEAK_MEMORY 105

#define V_ELEMENT_COUNT 106
