#ifndef WORLD_GROUP_READER_H
#define WORLD_GROUP_READER_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Read-only views; caller owns both input buffers for the view's lifetime.
 * This decoder does not assign activation semantics to any flag or group. */
typedef struct {
    const unsigned char *overrides, *groups;
    size_t override_count, group_bytes, group_count;
} WGTable;
typedef struct {
    const char *group_name;
    uint32_t group_hash;
    uint16_t override_index, section, instance, flags, reference_count;
} WGMember;
typedef int (*WGVisit)(const WGMember *, void *);

static uint16_t wg_u16(const unsigned char *p) {
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1]<<8);
}
static uint32_t wg_u32(const unsigned char *p) {
    return (uint32_t)wg_u16(p) | (uint32_t)wg_u16(p+2)<<16;
}

/* Payloads of companion 0x34107 / 0x34108. Validate the complete table before
 * exposing a view: no callback can observe a valid prefix of corrupt input.
 * +0/+4 and +44 of each group remain uninterpreted, including activation. */
static int wg_open(const unsigned char *ov, size_t olen,
                   const unsigned char *groups, size_t glen, WGTable *out) {
    if (!out) return 0;
    memset(out,0,sizeof *out);
    if ((!ov && olen) || (!groups && glen) || olen%8 || olen/8>65536) return 0;
    size_t count=olen/8, ng=0, pos=0;
    uint32_t *refs=count?calloc(count,sizeof *refs):NULL;
    if (count && !refs) return 0;
    while (pos<glen) {
        if (glen-pos<52) goto invalid;
        const unsigned char *g=groups+pos;
        uint32_t n=wg_u32(g+48), hash=0xffffffffu;
        uint64_t span=(52ULL+2ULL*n+3ULL)&~3ULL;
        if (span>glen-pos) goto invalid;
        size_t k=0;
        for (;k<32 && g[8+k];k++) {
            if (g[8+k]<32 || g[8+k]>126) goto invalid;
            hash=hash*33u+g[8+k];
        }
        if (!k || k==32 || hash!=wg_u32(g+40)) goto invalid;
        for (uint32_t j=0;j<n;j++) {
            unsigned index=wg_u16(g+52+2*(size_t)j);
            if (index>=count || refs[index]==UINT16_MAX) goto invalid;
            refs[index]++;
        }
        pos+=(size_t)span;ng++;
    }
    for (size_t i=0;i<count;i++)
        if (refs[i]!=wg_u16(ov+8*i+6)) goto invalid;
    free(refs);
    out->overrides=ov;out->override_count=count;
    out->groups=groups;out->group_bytes=glen;out->group_count=ng;
    return 1;
invalid:
    free(refs);return 0;
}
typedef struct {
    const unsigned char *ov, *groups;
    size_t olen, glen;
    unsigned seen;
} WGLeaves;
static int wg_find(const unsigned char *data, size_t len, unsigned depth, WGLeaves *leaves) {
    if(depth>64)return 0; /* resource bound, not a claimed retail format limit */
    for(size_t p=0;p<len;) {
        if(len-p<8)return 0;
        uint32_t id=wg_u32(data+p), size=wg_u32(data+p+4);
        if(size>len-p-8)return 0;
        const unsigned char *body=data+p+8;
        if(id==0x34107u) {
            if(leaves->seen&1)return 0;
            leaves->seen|=1;leaves->ov=body;leaves->olen=size;
        } else if(id==0x34108u) {
            if(leaves->seen&2)return 0;
            leaves->seen|=2;leaves->groups=body;leaves->glen=size;
        } else if((id>>28)==8 && !wg_find(body,size,depth+1,leaves))return 0;
        p+=8+(size_t)size;
    }
    return 1;
}
static int wg_open_file(const unsigned char *data, size_t len, WGTable *out) {
    if(!out)return 0;
    memset(out,0,sizeof *out);
    WGLeaves leaves={0};
    if((!data&&len) || !wg_find(data,len,0,&leaves) || leaves.seen!=3)return 0;
    return wg_open(leaves.ov,leaves.olen,leaves.groups,leaves.glen,out);
}
/* Only accepts a successfully opened view whose input buffers are unchanged. */
static int wg_visit(const WGTable *table, WGVisit fn, void *ctx) {
    if (!table || !fn) return 0;
    for (size_t p=0;p<table->group_bytes;) {
        const unsigned char *g=table->groups+p;
        uint32_t count=wg_u32(g+48);
        for (uint32_t i=0;i<count;i++) {
            uint16_t index=wg_u16(g+52+2*(size_t)i);
            const unsigned char *ov=table->overrides+8*(size_t)index;
            WGMember m={(const char *)(g+8),wg_u32(g+40),index,
                        wg_u16(ov),wg_u16(ov+2),wg_u16(ov+4),wg_u16(ov+6)};
            if (!fn(&m,ctx)) return 0;
        }
        p+=(size_t)((52ULL+2ULL*count+3ULL)&~3ULL);
    }
    return 1;
}
#endif
