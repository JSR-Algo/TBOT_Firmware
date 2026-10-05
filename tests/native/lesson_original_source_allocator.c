#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
static struct { void* pointer; size_t size; } slots[65536];
static size_t live_bytes, peak_bytes, calls, fail_at, injections;
static void (*allocation_event)(size_t);
static void (*failure_event)(void);
void original_source_on_failure(void (*event)(void)) { failure_event=event; }
void original_source_on_allocation(void (*event)(size_t)) { allocation_event=event; }
void original_source_fault_reset(size_t nth) {
    if (live_bytes) abort();
    calls = 0; fail_at = nth; peak_bytes = 0; injections = 0;
}
// Fault selection for a forked exact-state checkpoint; retains live ownership.
void original_source_fail_at(size_t nth) { fail_at=nth; injections=0; }
size_t original_source_live(void) { return live_bytes; }
size_t original_source_peak(void) { return peak_bytes; }
size_t original_source_calls(void) { return calls; }
size_t original_source_injections(void) { return injections; }
static int refuse(void) { ++calls; if(allocation_event)allocation_event(calls); if(calls==fail_at){++injections;if(failure_event)failure_event();return 1;}return 0; }
static void record(void* p, size_t n) {
    if (!p) {if(failure_event)failure_event();return;}
    for (size_t i=0;i<65536;i++) if (!slots[i].pointer) {
        slots[i].pointer=p; slots[i].size=n; live_bytes+=n;
        if(live_bytes>peak_bytes)peak_bytes=live_bytes;
        return;
    }
    abort();
}
static size_t forget(void* p) {
    if (!p) return 0;
    for(size_t i=0;i<65536;i++)if(slots[i].pointer==p){
        size_t n=slots[i].size;slots[i].pointer=NULL;live_bytes-=n;return n;
    }
    abort();
}
void* source_malloc(size_t n) { if(refuse())return NULL;void*p=malloc(n);record(p,n);return p; }
void source_free(void*p) {forget(p);free(p);}
int source_posix_memalign(void**p,size_t align,size_t n) {
    if(refuse())return ENOMEM;int r=posix_memalign(p,align,n);if(!r)record(*p,n);else if(failure_event)failure_event();return r;
}
void* source_memalign(size_t align,size_t n) {void*p=NULL;if(source_posix_memalign(&p,align,n))return NULL;return p;}
void* source_realloc(void*p,size_t n) {
    if(refuse())return NULL;
    size_t old=forget(p);void*q=realloc(p,n);if(q)record(q,n);else {record(p,old);if(failure_event)failure_event();}return q;
}
