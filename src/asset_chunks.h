#ifndef OPENUG2_ASSET_CHUNKS_H
#define OPENUG2_ASSET_CHUNKS_H
#include <stdint.h>

/* Shared framing only: little-endian tag/byte-count followed by a payload.
 * The format reader decides which tags contain children and decodes leaves.
 * Offsets remain relative to the input buffer; no assets are copied/owned here.
 * Callers must supply a range inside their buffer. A visitor returns nonzero
 * to descend, zero to skip the payload. Traversal is depth-first, in file order.
 * Failure returns zero; visitors may already have seen a valid prefix. */
typedef int (*AssetChunkVisitor)(const unsigned char *data,uint32_t tag,
                                long payload,long end,void *context);

static uint32_t asset_chunk_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static int asset_chunks_walk_depth(const unsigned char *data,long beg,long end,
                                   AssetChunkVisitor visit,void *context,int depth) {
    if(depth>=64)return 0; /* bound the stack for malformed nested containers */
    for(long at=beg;at<end;) {
        if(end-at<8)return 0;
        uint32_t tag=asset_chunk_u32(data+at),size=asset_chunk_u32(data+at+4);
        long payload=at+8;
        if((uint64_t)size>(uint64_t)(end-payload))return 0;
        long next=payload+(long)size;
        if(visit(data,tag,payload,next,context) &&
           !asset_chunks_walk_depth(data,payload,next,visit,context,depth+1))return 0;
        at=next;
    }
    return 1;
}

static int asset_chunks_walk(const unsigned char *data,long beg,long end,
                             AssetChunkVisitor visit,void *context) {
    if(!data || !visit || beg<0 || end<beg)return 0;
    return asset_chunks_walk_depth(data,beg,end,visit,context,0);
}
#endif
