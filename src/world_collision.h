#ifndef OPENUG2_WORLD_COLLISION_H
#define OPENUG2_WORLD_COLLISION_H
#include "physics.h"
#include "ug2_track_collision.h"
#include "world_scenery.h"

static int wce_bun(N2Scene *scene,const UG2TrackCollision *data,uint32_t bundle,int event) {
    if(!scene || !data || event < -1 || event > 65535 || !data->activation.count || data->activation.count>256 ||
       scene->count<0 || data->count>(size_t)(INT_MAX-scene->count) ||
       (data->count && !data->polygons))return 0;
    N2Mesh *walls=data->count?calloc(data->count,sizeof *walls):NULL;
    if(data->count && !walls)return 0;
    int count=0;
    for(size_t k=0;k<data->count;k++) {
        const UG2TrackPolygon *poly=data->polygons+k;float verts[20]={0},face[9];
        unsigned group=poly->metadata[0];
        if(group>=data->activation.count)goto fail;
        const char *name=(const char*)data->activation.records[group]+9;
        int owner=wg_event_id(name);
        /* shortcut: named group selection; decode retail activation flags before widening this policy. */
        if(event && ((owner && owner!=event) || wg_isolated_closure(name)))continue;
        for(int v=0;v<4;v++)memcpy(verts+v*5,poly->corners[v],3*sizeof(float));
        uint16_t indices[]={0,1,2,0,2,3};
        N2Mesh m={0};m.verts=verts;m.idx=indices;m.nverts=4;m.nidx=6;
        m.cat=N2_COLLISION;m.scen=N2_SC_WALL;m.ground_disabled=1;
        if(!phys_wall_debug_face(&m,0,WALL_MIN_FACE_SPAN,face) &&
           !phys_wall_debug_face(&m,1,WALL_MIN_FACE_SPAN,face))continue;
        m.verts=malloc(sizeof verts);m.idx=malloc(sizeof indices);
        if(!m.verts || !m.idx){free(m.verts);free(m.idx);goto fail;}
        memcpy(m.verts,verts,sizeof verts);memcpy(m.idx,indices,sizeof indices);
        uint64_t id=UINT64_C(1469598103934665603);
        const uint64_t provenance[]={UINT64_C(0x554732504f4c5900),bundle,poly->source_offset};
        for(size_t p=0;p<sizeof provenance;p++){id^=((const unsigned char*)provenance)[p];id*=UINT64_C(1099511628211);}
        m.placement_id=id?id:1;strcpy(m.sname,"BUN_TRACK_POLYGON");
        walls[count++]=m;
    }
    if(count) {
        size_t total=(size_t)scene->count+count;
        if(total>SIZE_MAX/sizeof *scene->meshes)goto fail;
        N2Mesh *grown=realloc(scene->meshes,total*sizeof *grown);
        if(!grown)goto fail;
        scene->meshes=grown;scene->cap=(int)total;
    }
    for(int n=0;n<scene->count;n++) {
        N2Mesh *m=scene->meshes+n;
        if(m->prop_id)continue;
        if(m->cat!=N2_ROAD && m->cat!=N2_TERRAIN && m->cat!=N2_COLLISION &&
           m->scen!=N2_SC_BUILDING && m->scen!=N2_SC_WALL && m->scen!=N2_SC_STRUCT)continue;
        free(m->wall_verts);free(m->wall_idx);m->wall_verts=NULL;m->wall_idx=NULL;
        m->wall_nverts=m->wall_nidx=0;m->wall_policy=1;m->wall_height=0;
    }
    if(count)memcpy(scene->meshes+scene->count,walls,(size_t)count*sizeof *walls);
    scene->count+=count;free(walls);return 1;
fail:
    for(int k=0;k<count;k++){free(walls[k].verts);free(walls[k].idx);}
    free(walls);return 0;
}

/* Compiled collision corrections. Buildings retain their authored facades.
   Height zero removes body walls; exact sources win over model rules. */
typedef struct {
    int kind; /* 0 model, 1 source, 2 standalone */
    uint64_t key;
    float height;
    int count;
    const float (*points)[3];
} WCollisionRule;
typedef struct { const char *map; int count; const WCollisionRule *rules; } WCollisionEdits;

static int wce_map(const WCollisionEdits *e,const char *track) {
    size_t n=strlen(track);
    if (n>4 && !strcmp(track+n-4,".BUN")) n-=4;
    return n==strlen(e->map) && !strncmp(track,e->map,n);
}
static const WCollisionRule *wce_source(const WCollisionEdits *e,const N2Mesh *m) {
    uint64_t id=n2_world_source_id(m);
    for (int k=0;k<e->count;k++) if (e->rules[k].kind==1 && e->rules[k].key==id) return e->rules+k;
    return NULL;
}
static int wce_wall(N2Mesh *m,const WCollisionRule *r,int standalone) {
    int count=r->count-1;
    float *v=calloc((size_t)count*4*5,sizeof *v);
    uint16_t *idx=malloc((size_t)count*6*sizeof *idx);
    if (!v || !idx) {free(v);free(idx);return 0;}
    for (int k=0;k<count;k++) {
        const float *a=r->points[k],*b=r->points[k+1];
        float p[4][3]={{a[0],a[1],a[2]},{b[0],b[1],b[2]},
            {b[0],b[1],b[2]+r->height},{a[0],a[1],a[2]+r->height}};
        const int order[6]={0,1,2,0,2,3};
        for (int n=0;n<4;n++) memcpy(v+(k*4+n)*5,p[n],sizeof p[n]);
        for (int n=0;n<6;n++) idx[k*6+n]=(uint16_t)(k*4+order[n]);
    }
    if (standalone) {
        m->verts=v;m->idx=idx;m->nverts=count*4;m->nidx=count*6;
        m->cat=N2_COLLISION;m->scen=N2_SC_WALL;
        strcpy(m->sname,"LOCAL_COLLISION_WALL");
    } else {
        free(m->wall_verts);free(m->wall_idx);
        m->wall_verts=v;m->wall_idx=idx;m->wall_nverts=count*4;m->wall_nidx=count*6;
    }
    return 1;
}
static int wce_apply(const WCollisionEdits *e,N2Scene *s,int local) {
    int count=e?e->count:0;
    int *owners=malloc((size_t)(count?count:1)*sizeof *owners);
    if (!owners) return 0;
    for (int k=0;k<count;k++) owners[k]=-1;
    for (int n=0;n<s->count;n++) {
        N2Mesh *m=s->meshes+n;
        const WCollisionRule *r=e&&local?wce_source(e,m):NULL;
        if (m->scen==N2_SC_BUILDING && !r) {
            m->wall_policy=0;m->wall_height=0;
            continue;
        }
        if (r && owners[r-e->rules]<0) owners[r-e->rules]=n;
        if (!r) for (int k=0;k<count;k++) if (e->rules[k].kind==0 &&
            e->rules[k].key==m->world_model_key) {r=e->rules+k;break;}
        if (!r) for (int k=0;k<count;k++) if (e->rules[k].kind==0 &&
            e->rules[k].key==m->world_base_model_key) {r=e->rules+k;break;}
        if (r) {
            m->wall_policy=r->height>0?2:1;
            m->wall_height=r->kind==0?r->height:0;
        }
    }
    if (!phys_prepare_boundaries(s)) goto fail;
    for (int k=0;k<count;k++) {
        const WCollisionRule *r=e->rules+k;
        if (!local || r->kind==0 || r->height==0) continue;
        if (r->kind==1) {
            /* All slices were masked above; own one replacement without hashing
               the whole ground payload again for every exact source rule. */
            if (owners[k]>=0 && !wce_wall(s->meshes+owners[k],r,0)) goto fail;
        } else {
            N2Mesh *grown=realloc(s->meshes,(size_t)(s->count+1)*sizeof *grown);
            if (!grown) goto fail;
            s->meshes=grown;s->cap=s->count+1;
            N2Mesh wall={0};
            if (!wce_wall(&wall,r,1)) goto fail;
            s->meshes[s->count++]=wall;
        }
    }
    free(owners);return 1;
fail:
    free(owners);return 0;
}

static int wce_roadside(const WCollisionEdits *e,N2Scene *s,int local) {
    for (int n=0;n<s->count;n++) {
        N2Mesh *m=s->meshes+n;
        /* Marked movable scenery has its own contact/knockdown path. Do not
           disable it as decorative scenery before the BUN pass preserves it. */
        if(m->prop_id){m->wall_policy=0;m->ground_disabled=1;continue;}
        const unsigned char *name=(const unsigned char*)m->sname;size_t len=strlen(m->sname);
        int water=n2_icontains(name,len,"WATER") || n2_icontains(name,len,"OCEAN");
        /* Race guide panels and hanging foliage are not permanent roadside walls. */
        int boundary=!water && !n2_world_effect_only(m) &&
            !n2_icontains(name,len,"TRACKBARRIER") && !n2_icontains(name,len,"GTBARRIER") &&
            !n2_icontains(name,len,"OVERHANG") &&
            (m->scen==N2_SC_WALL || m->cat==N2_COLLISION ||
             n2_icontains(name,len,"PLANTER") ||
             (m->scen==N2_SC_TERRAIN && (strstr(m->sname,"WALL") ||
              strstr(m->sname,"GUARD") || !strncmp(m->sname,"TRN_MEDIAN",10))));
        /* Water and some barriers share ROAD/TERRAIN draw categories. */
        m->ground_disabled=water || m->scen==N2_SC_BUILDING || m->scen==N2_SC_PROP ||
                           m->scen==N2_SC_TREE || m->scen==N2_SC_WALL;
        int ground=!m->ground_disabled && (m->cat==N2_ROAD || m->cat==N2_TERRAIN);
        m->wall_policy=boundary || ground?0:1;
        m->wall_height=0;
        /* Keep authored wall heights; reviewed guardrail edits supply their 8 m walls. */
        if (boundary && !ground && m->scen!=N2_SC_WALL) {
            m->wall_policy=2;m->wall_height=2.5f;
        }
    }
    return wce_apply(e,s,local);
}
#endif
