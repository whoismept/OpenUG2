#ifndef OPENUG2_UG2_TRACK_COLLISION_H
#define OPENUG2_UG2_TRACK_COLLISION_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "asset_chunks.h"

typedef struct {
    unsigned char records[256][32]; /* Preserve activation bytes and count-like fields. */
    size_t count;
    uint64_t source_offset;
} UG2TrackGroups;
typedef struct { UG2TrackGroups result; int seen,valid; } UG2TCGroupsRead;
static int ug2_tc_groups_visit(const unsigned char *bytes,uint32_t id,long begin,long end,void *context) {
    UG2TCGroupsRead *read=context;
    if(!read->valid)return 0;
    if(id!=0x3412f)return id>>28==8;
    if(read->seen++ || !(end-begin) || (end-begin)%32 || (end-begin)/32>256)goto invalid;
    read->result.count=(size_t)(end-begin)/32;read->result.source_offset=(uint64_t)begin;
    for(size_t k=0;k<read->result.count;k++) {
        const unsigned char *r=bytes+begin+32*k,*name=r+9;
        const unsigned char *stop=memchr(name,0,23);
        if(!stop || stop==name)goto invalid;
        uint32_t hash=0xffffffffu;
        for(const unsigned char *p=name;p<stop;p++) {
            if(*p<32 || *p>126)goto invalid;
            hash=hash*33u+*p;
        }
        if(k ? hash!=asset_chunk_u32(r) : (asset_chunk_u32(r) || strcmp((const char*)name,"NONE")))goto invalid;
        memcpy(read->result.records[k],r,32);
    }
    return 0;
invalid:
    read->valid=0;return 0;
}
/* Success copies owned rows; malformed input retains the previous table. */
static int ug2_track_groups_read(const unsigned char *bytes,size_t size,UG2TrackGroups *out) {
    if(!bytes || !out || size>LONG_MAX)return 0;
    UG2TCGroupsRead read={0};read.valid=1;
    if(!asset_chunks_walk(bytes,0,(long)size,ug2_tc_groups_visit,&read) || !read.valid || read.seen!=1)return 0;
    *out=read.result;return 1;
}

#define UG2_TC_POLYGON_SIZE 32      /* fixed driving-collision polygon record */
#define UG2_TC_ATTRIBUTE_OFFSET 2   /* S5-11 one-byte surface attribute */
typedef struct {
    float corners[4][3];
    uint32_t section;
    uint64_t source_offset;
    unsigned char metadata[8]; /* +0 indexes activation; +1 extent; +3 flags. */
    unsigned char attribute;   /* +2 surface attribute, stored unchanged (S5-11.4) */
} UG2TrackPolygon;
/* S5-11 attribute-lookup acceleration. A uniform XY CSR grid built once after
   the polygons load: polygon k enters every cell its XY bounds overlap after
   dilation by the selector's barycentric tolerance, so the one cell holding
   (x,y) lists every polygon the brute-force selector could pick there, in
   ascending polygon order. Equal keys therefore keep the earlier polygon
   exactly as the full scan does. The index is optional data: an absent index
   falls back to the scan, so hand-built owners keep working. */
#define UG2_TC_INDEX_CELL 32.0f
#define UG2_TC_INDEX_PAD_REL 0.03f  /* > 2x the 1% barycentric dilation */
#define UG2_TC_INDEX_PAD_ABS 0.05f  /* metres, absorbs binary32 rounding of u,v,w */
#define UG2_TC_INDEX_MAXCELLS ((size_t)1<<26)
typedef struct {
    float x0,y0;   /* cell (0,0)'s lower corner (minimum raw polygon bound) */
    int gw,gh;     /* grid dimensions; 0 when not built */
    int *start;    /* cells+1 CSR offsets into list; NULL = not built */
    int *list;     /* ascending polygon indices, cell-major */
} UG2TrackIndex;
typedef struct {
    UG2TrackPolygon *polygons;
    size_t count, groups, total;
    UG2TrackGroups activation;
    UG2TrackIndex index; /* S5-11 lookup acceleration, built by read */
} UG2TrackCollision;
static void ug2_tc_index_reset(UG2TrackIndex *index) {
    if(!index)return;
    free(index->start);free(index->list);*index=(UG2TrackIndex){0};
}
static void ug2_track_collision_free(UG2TrackCollision *data) {
    if(data){free(data->polygons);ug2_tc_index_reset(&data->index);*data=(UG2TrackCollision){0};}
}
/* One monotone coordinate mapping for build and query: a query point inside a
   polygon's (dilated) cell range can only resolve to a cell the polygon was
   inserted into, including points outside the grid, which clamp to the nearest
   edge cell. Double subtraction keeps the mapping exact for binary32 inputs. */
static int ug2_tc_index_coord(float v,float origin,int n) {
    double c=floor(((double)v-(double)origin)/UG2_TC_INDEX_CELL);
    if(!(c>0.0))return 0; /* also catches NaN */
    if(c>=(double)n)return n-1;
    return (int)c;
}
/* Dilation bound: the selector accepts barycentrics u,v,w >= -0.01, and such a
   point lies within 2*0.01*diam of the triangle (centroid construction), so the
   raw XY bounds expanded by PAD_REL*diam + PAD_ABS cover every accepted point.
   Both triangles of a quad lie inside the quad's corner bounds and diameter. */
static void ug2_tc_index_cells(const UG2TrackPolygon *poly,float x0,float y0,
                               int gw,int gh,int c[4]) {
    float px0=INFINITY,py0=INFINITY,px1=-INFINITY,py1=-INFINITY;
    for(int v=0;v<4;v++) {
        px0=fminf(px0,poly->corners[v][0]);px1=fmaxf(px1,poly->corners[v][0]);
        py0=fminf(py0,poly->corners[v][1]);py1=fmaxf(py1,poly->corners[v][1]);
    }
    double dx=(double)px1-(double)px0,dy=(double)py1-(double)py0;
    float pad=UG2_TC_INDEX_PAD_REL*(float)sqrt(dx*dx+dy*dy)+UG2_TC_INDEX_PAD_ABS;
    c[0]=ug2_tc_index_coord(px0-pad,x0,gw);c[1]=ug2_tc_index_coord(px1+pad,x0,gw);
    c[2]=ug2_tc_index_coord(py0-pad,y0,gh);c[3]=ug2_tc_index_coord(py1+pad,y0,gh);
}
/* Build (or rebuild) the S5-11 index in data. 1 = index usable for this set
   (also for an empty set, which needs none); 0 = allocation failure or data
   outside the indexable domain, leaving no index so queries fall back to the
   exact scan. Called once by ug2_track_collision_read; exposed for tests. */
static int ug2_track_collision_index_build(UG2TrackCollision *data) {
    if(!data)return 0;
    ug2_tc_index_reset(&data->index);
    if(!data->count)return 1;
    if(!data->polygons)return 0;
    float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
    for(size_t k=0;k<data->count;k++)
        for(int v=0;v<4;v++) {
            float px=data->polygons[k].corners[v][0],py=data->polygons[k].corners[v][1];
            if(!isfinite(px)||!isfinite(py))return 0;
            x0=fminf(x0,px);y0=fminf(y0,py);x1=fmaxf(x1,px);y1=fmaxf(y1,py);
        }
    if(!(x0<=x1)||!(y0<=y1))return 0;
    double sx=((double)x1-(double)x0)/UG2_TC_INDEX_CELL;
    double sy=((double)y1-(double)y0)/UG2_TC_INDEX_CELL;
    if(sx>=(double)INT_MAX-1.0||sy>=(double)INT_MAX-1.0)return 0;
    int gw=(int)sx+1,gh=(int)sy+1;
    if(gw<=0||gh<=0)return 0;
    size_t cells=(size_t)gw*(size_t)gh;
    if(cells>UG2_TC_INDEX_MAXCELLS||cells>SIZE_MAX/sizeof(int)-1)return 0;
    int *start=(int*)calloc(cells+1,sizeof *start);
    if(!start)return 0;
    size_t total=0;
    for(size_t k=0;k<data->count;k++) {
        int c[4];ug2_tc_index_cells(data->polygons+k,x0,y0,gw,gh,c);
        total+=(size_t)(c[1]-c[0]+1)*(size_t)(c[3]-c[2]+1);
        if(total>(size_t)INT_MAX){free(start);return 0;}
        for(int cy=c[2];cy<=c[3];cy++)for(int cx=c[0];cx<=c[1];cx++)start[(size_t)cy*gw+cx+1]++;
    }
    for(size_t c=0;c<cells;c++)start[c+1]+=start[c];
    int *list=(int*)malloc(total*sizeof *list);
    int *cursor=(int*)malloc(cells*sizeof *cursor);
    if(!list||!cursor){free(start);free(list);free(cursor);return 0;}
    memcpy(cursor,start,cells*sizeof *cursor);
    for(size_t k=0;k<data->count;k++) {
        int c[4];ug2_tc_index_cells(data->polygons+k,x0,y0,gw,gh,c);
        for(int cy=c[2];cy<=c[3];cy++)for(int cx=c[0];cx<=c[1];cx++)
            list[cursor[(size_t)cy*gw+cx]++]=(int)k;
    }
    free(cursor);
    data->index.x0=x0;data->index.y0=y0;data->index.gw=gw;data->index.gh=gh;
    data->index.start=start;data->index.list=list;
    return 1;
}
static unsigned ug2_tc_u16(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static int ug2_tc_s16(const unsigned char *p){unsigned n=ug2_tc_u16(p);return n>32767?(int)n-65536:(int)n;}
static float ug2_tc_f32(const unsigned char *p){uint32_t bits=asset_chunk_u32(p);float f;memcpy(&f,&bits,4);return f;}
typedef struct {
    UG2TrackCollision result;
    const float *bounds;
    size_t capacity;
    int valid;
} UG2TCRead;
static int ug2_tc_group(const unsigned char *bytes,long begin,long end,UG2TCRead *read) {
    long leaves[4]={0},sizes[4]={0};unsigned seen=0;
    for(long p=begin;p<end;) {
        if(end-p<8)return 0;
        uint32_t id=asset_chunk_u32(bytes+p),size=asset_chunk_u32(bytes+p+4);
        if((uint64_t)size>(uint64_t)(end-p-8))return 0;
        int slot=id==0x34131?0:id==0x34134?1:id==0x34136?2:id==0x34137?3:-1;
        if(slot>=0) {
            if(seen&(1u<<slot))return 0;
            seen|=1u<<slot;leaves[slot]=p+8;sizes[slot]=size;
        } else if(id)return 0;
        p+=8+(long)size;
    }
    if(seen!=15 || sizes[0]!=48 || sizes[1]%UG2_TC_POLYGON_SIZE || sizes[2]%4 || sizes[3]%4)return 0;
    const unsigned char *header=bytes+leaves[0];
    size_t nx=ug2_tc_u16(header+4),ny=ug2_tc_u16(header+6),count=(size_t)sizes[1]/UG2_TC_POLYGON_SIZE,refs=(size_t)sizes[2]/4;
    if((uint64_t)nx*ny!=(uint64_t)sizes[3]/4)return 0;
    for(int k=0;k<2;k++)if(!isfinite(ug2_tc_f32(header+8+4*k)))return 0;
    float low[3],high[3];
    for(int k=0;k<3;k++) {
        low[k]=ug2_tc_f32(header+16+4*k);high[k]=ug2_tc_f32(header+32+4*k);
        /* Empty sections use +FLT_MAX/-FLT_MAX instead of geometric bounds. */
        if(!isfinite(low[k]) || !isfinite(high[k]) || (count && low[k]>high[k]))return 0;
    }
    size_t cursor=0;
    for(long p=leaves[3];p<leaves[3]+sizes[3];p+=4) {
        size_t start=ug2_tc_u16(bytes+p),n=ug2_tc_u16(bytes+p+2);
        if(start!=cursor || n>refs-cursor)return 0;
        cursor+=n;
    }
    if(cursor!=refs)return 0;
    for(long p=leaves[2];p<leaves[2]+sizes[2];p+=4)if(ug2_tc_u16(bytes+p)>=count)return 0;
    if(count>SIZE_MAX-read->result.total)return 0;
    read->result.groups++;read->result.total+=count;
    for(long p=leaves[1];p<leaves[1]+sizes[1];p+=UG2_TC_POLYGON_SIZE) {
        if(bytes[p]>=read->result.activation.count || ug2_tc_u16(bytes+p+6)!=65535)return 0;
        UG2TrackPolygon poly={0};poly.section=asset_chunk_u32(header);poly.source_offset=(uint64_t)p;
        memcpy(poly.metadata,bytes+p,8);poly.attribute=bytes[p+UG2_TC_ATTRIBUTE_OFFSET];
        int base=ug2_tc_s16(bytes+p+4);
        float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
        for(int v=0;v<4;v++) {
            float *q=poly.corners[v];q[0]=ug2_tc_s16(bytes+p+8+2*v)/8.f;
            q[1]=ug2_tc_s16(bytes+p+16+2*v)/8.f;q[2]=base+ug2_tc_s16(bytes+p+24+2*v)/256.f;
            for(int k=0;k<3;k++)if(q[k]<low[k]-.001f || q[k]>high[k]+.001f)return 0;
            x0=fminf(x0,q[0]);x1=fmaxf(x1,q[0]);y0=fminf(y0,q[1]);y1=fmaxf(y1,q[1]);
        }
        if(read->bounds && (x1<read->bounds[0] || y1<read->bounds[1] || x0>read->bounds[2] || y0>read->bounds[3]))continue;
        if(read->result.count==read->capacity) {
            size_t cap=read->capacity?read->capacity*2:256;
            if(cap<read->capacity || cap>SIZE_MAX/sizeof poly)return 0;
            UG2TrackPolygon *grown=realloc(read->result.polygons,cap*sizeof poly);
            if(!grown)return 0;
            read->result.polygons=grown;read->capacity=cap;
        }
        read->result.polygons[read->result.count++]=poly;
    }
    return 1;
}
static int ug2_tc_visit(const unsigned char *bytes,uint32_t id,long begin,long end,void *context) {
    UG2TCRead *read=context;
    if(!read->valid)return 0;
    if(id==0x80034130){read->valid=ug2_tc_group(bytes,begin,end,read);return 0;}
    return id>>28==8;
}
/* bounds is optional XY min/max; success replaces an initialized owner.
   Failure retains the previous owner, including after a malformed valid prefix. */
static int ug2_track_collision_read(const unsigned char *bytes,size_t size,
                                    const UG2TrackGroups *activation,const float bounds[4],UG2TrackCollision *out) {
    if(!bytes || !out || !activation || !activation->count || activation->count>256 || size>LONG_MAX || sizeof(float)!=4)return 0;
    if(bounds) {
        for(int k=0;k<4;k++)if(!isfinite(bounds[k]))return 0;
        if(bounds[0]>bounds[2] || bounds[1]>bounds[3])return 0;
    }
    UG2TCRead read={0};read.bounds=bounds;read.valid=1;
    read.result.activation=*activation;
    int valid=asset_chunks_walk(bytes,0,(long)size,ug2_tc_visit,&read) && read.valid && read.result.groups;
    /* The S5-11 lookup index is part of a ready collision set; failing to
       allocate it fails the read instead of silently scanning every query. */
    if(valid && !ug2_track_collision_index_build(&read.result))valid=0;
    if(!valid){ug2_track_collision_free(&read.result);return 0;}
    ug2_track_collision_free(out);*out=read.result;return 1;
}

/* S5-11 attribute lookup. Returns the one-byte surface attribute stored by the
   collision polygon selected at (x,y), or -1 when this set selects none. The
   selection reuses the reachable-wheel ground rule over the collision layer:
   covering face (same barycentric tolerance), inside the [wheel_z-reach_down,
   wheel_z+reach_up] window, nearest to wheel_z with the query's rising-ramp
   preference, and near-vertical faces rejected. The byte is returned exactly
   as loaded (no remap or clamp, S5-11.4); the 0..28 domain check is the
   caller's S5-11.2 step. */
static void ug2_tc_polygon_select(const UG2TrackPolygon *poly,float x,float y,
                                  float wheel_z,float reach_up,float reach_down,
                                  int *found,float *best_key,int *best_attribute) {
    static const int tri[2][3]={{0,1,2},{0,2,3}};
    for(int t=0;t<2;t++) {
        const float *a=poly->corners[tri[t][0]],*b=poly->corners[tri[t][1]],
                    *c=poly->corners[tri[t][2]];
        float d=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1]);
        if(d>-1e-9f && d<1e-9f)continue;
        float u=((b[1]-c[1])*(x-c[0])+(c[0]-b[0])*(y-c[1]))/d;
        float v=((c[1]-a[1])*(x-c[0])+(a[0]-c[0])*(y-c[1]))/d;
        float w=1-u-v;
        if(u<-0.01f || v<-0.01f || w<-0.01f)continue;
        float z=u*a[2]+v*b[2]+w*c[2],dz=z-wheel_z;
        if(dz>reach_up || dz<-reach_down)continue;
        float e1x=b[0]-a[0],e1y=b[1]-a[1],e1z=b[2]-a[2];
        float e2x=c[0]-a[0],e2y=c[1]-a[1],e2z=c[2]-a[2];
        float nx=e1y*e2z-e1z*e2y,ny=e1z*e2x-e1x*e2z,nz=e1x*e2y-e1y*e2x;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz);
        if(!(nl>1e-9f) || fabsf(nz)<.30f*nl)continue; /* vertical face: wall, not support */
        int ramp=fabsf(nx)+fabsf(ny)>fabsf(nz)*.005f;
        float key=dz>0 && ramp && u>=0 && v>=0 && w>=0 ? -dz : fabsf(dz);
        if(*found && key>=*best_key)continue;
        *found=1;*best_key=key;*best_attribute=poly->attribute;
    }
}
static int ug2_tc_surface_attribute(const UG2TrackCollision *data,float x,float y,
                                    float wheel_z,float reach_up,float reach_down) {
    int found=0,best_attribute=0;float best_key=1e30f;
    if(!data || !data->count || !data->polygons)return -1;
    const UG2TrackIndex *ix=&data->index;
    /* Non-finite query points keep the scan so the historically finite-output
       result (a miss, or the scan's own NaN-key ordering) is unchanged. */
    if(ix->start && ix->list && ix->gw>0 && ix->gh>0 && isfinite(x) && isfinite(y)) {
        int cx=ug2_tc_index_coord(x,ix->x0,ix->gw),cy=ug2_tc_index_coord(y,ix->y0,ix->gh);
        size_t cell=(size_t)cy*(size_t)ix->gw+(size_t)cx;
        for(int n=ix->start[cell];n<ix->start[cell+1];n++) {
            size_t k=(size_t)ix->list[n];
            if(k<data->count)
                ug2_tc_polygon_select(data->polygons+k,x,y,wheel_z,reach_up,reach_down,
                                      &found,&best_key,&best_attribute);
        }
    } else {
        for(size_t k=0;k<data->count;k++)
            ug2_tc_polygon_select(data->polygons+k,x,y,wheel_z,reach_up,reach_down,
                                  &found,&best_key,&best_attribute);
    }
    return found?best_attribute:-1;
}
#endif
