#ifndef WORLD_SCENERY_H
#define WORLD_SCENERY_H
#include "world_group_reader.h"

/* Conservative load-time preview, NOT decoded retail activation flags.
 * Only exclusive membership in numeric event groups is suppressible. Unknown,
 * career, free-roam and shared memberships retain their previous behavior. */
typedef struct {
    uint16_t section, row, flags;
    unsigned char membership, checked;
} WGSelected;
typedef struct { WGSelected *items; size_t count; } WGSelection;
enum { WG_EVENT=1, WG_OTHER=2, WG_ACTIVE=4 };

/* Runtime policy remains deliberately narrower than retail semantics. Normal
 * free roam hides only placements proven exclusive to numeric event groups.
 * A requested preview keeps its explicit selection. A live race selects its
 * OWN authored group, so another event's road closures no longer stand on the
 * raced route. Direction/career activation timing is still undecoded: within
 * the selected event nothing is further filtered.
 *
 * Measured on STREAML4RG event 4701 (M160): 4827 group placements are loaded,
 * only 77 of them belong to BARRIERS_4701, and foreign-exclusive placements
 * including two other events' start/finish gantries sit at 0.00 m from 4701's
 * own 0x34148 racing line.
 *
 * A positive id here is a REQUEST. Some shipped events author no group at all
 * (L4RB 4201/4202/4203, L4RC 4341), and the builder degrades those back to the
 * unfiltered scene rather than failing to load. */
static int wg_runtime_selection(int preview_set,int preview_event,int race_event) {
    if(preview_set)return preview_event;
    return race_event>0?race_event:-1;
}

static int wg_event_id(const char *name) {
    const char *p=NULL;
    if(!strncmp(name,"BARRIERS_",9))p=name+9;
    else if(!strncmp(name,"PLAYER_BARRIERS_",16))p=name+16;
    if(!p||!*p)return 0;
    unsigned n=0;
    for(;*p;p++) {
        if(*p<'0'||*p>'9'||n>6553)return 0;
        n=n*10+(unsigned)(*p-'0');
    }
    return n>0&&n<=65535?(int)n:0;
}
/* Does this bundle author a group for the requested event? Free roam (-1) and
 * "unchanged" (0) are always answerable, so only a positive id is looked up.
 * Absence is an authoring fact, NOT a malformed table: the caller uses this to
 * tell the two apart instead of treating both as a load failure. */
static int wg_event_group_present(const WGTable *table,int event) {
    if(!table)return 0;
    if(event<=0)return 1;
    for(size_t p=0;p<table->group_bytes;) {
        const unsigned char *g=table->groups+p;
        uint32_t refs=wg_u32(g+48);
        if(wg_event_id((const char *)g+8)==event)return 1;
        p+=(size_t)((52ULL+2ULL*refs+3)&~3ULL);
    }
    return 0;
}
static int wg_selected_cmp(const void *aa,const void *bb) {
    const WGSelected *a=aa,*b=bb;
    if(a->section!=b->section)return a->section<b->section?-1:1;
    return (a->row>b->row)-(a->row<b->row);
}
static int wg_selection_open(const WGTable *table,int event,WGSelection *out) {
    memset(out,0,sizeof *out);
    if(event < -1 || event > 65535)return 0;
    if(!event)return 1;
    size_t n=table->override_count;
    WGSelected *items=n?calloc(n,sizeof *items):NULL;
    if(n&&!items)return 0;
    for(size_t i=0;i<n;i++) {
        const unsigned char *r=table->overrides+8*i;
        items[i].section=wg_u16(r);items[i].row=wg_u16(r+2);items[i].flags=wg_u16(r+4);
    }
    int found=event==-1;
    for(size_t p=0;p<table->group_bytes;) {
        const unsigned char *g=table->groups+p;
        int id=wg_event_id((const char *)g+8);
        unsigned bits=id?WG_EVENT:WG_OTHER;
        if(id>0&&id==event){bits|=WG_ACTIVE;found=1;}
        uint32_t refs=wg_u32(g+48);
        for(uint32_t j=0;j<refs;j++)items[wg_u16(g+52+2*(size_t)j)].membership|=(unsigned char)bits;
        p+=(size_t)((52ULL+2ULL*refs+3)&~3ULL);
    }
    if(n>1)qsort(items,n,sizeof *items,wg_selected_cmp);
    for(size_t i=1;i<n;i++)if(!wg_selected_cmp(items+i-1,items+i)){free(items);return 0;}
    if(!found){free(items);return 0;}
    out->items=items;out->count=n;return 1;
}
static const WGSelected *wg_selection_find(const WGSelection *s,unsigned section,unsigned row) {
    if(!s||!s->count||section>65535||row>65535)return NULL;
    WGSelected key={0};key.section=(uint16_t)section;key.row=(uint16_t)row;
    return bsearch(&key,s->items,s->count,sizeof *s->items,wg_selected_cmp);
}
static int wg_selection_visible(const WGSelection *s,unsigned section,unsigned row) {
    const WGSelected *i=wg_selection_find(s,section,row);
    return !i || !(i->membership&WG_EVENT) || (i->membership&(WG_OTHER|WG_ACTIVE));
}
#endif
