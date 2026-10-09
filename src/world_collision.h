#ifndef OPENUG2_WORLD_COLLISION_H
#define OPENUG2_WORLD_COLLISION_H
#include "physics.h"

/* Compiled collision corrections. Height zero removes body walls; exact sources win. */
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
static int wce_apply(const WCollisionEdits *e,N2Scene *s) {
    int *owners=malloc((size_t)(e->count?e->count:1)*sizeof *owners);
    if (!owners) return 0;
    for (int k=0;k<e->count;k++) owners[k]=-1;
    for (int n=0;n<s->count;n++) {
        N2Mesh *m=s->meshes+n;
        const WCollisionRule *r=wce_source(e,m);
        if (r && owners[r-e->rules]<0) owners[r-e->rules]=n;
        if (!r) for (int k=0;k<e->count;k++) if (e->rules[k].kind==0 &&
            e->rules[k].key==m->world_model_key) {r=e->rules+k;break;}
        if (r) {
            m->wall_policy=r->height>0?2:1;
            m->wall_height=r->kind==0?r->height:0;
        }
    }
    if (!phys_prepare_boundaries(s)) goto fail;
    for (int k=0;k<e->count;k++) {
        const WCollisionRule *r=e->rules+k;
        if (r->kind==0 || r->height==0) continue;
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
#endif
