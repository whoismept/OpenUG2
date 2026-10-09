/* Asset-free compiled collision corrections: unchanged draw/ground payload,
   all material slices, exact overrides and forward/reverse/airborne contacts. */
#include <assert.h>
#include "world.h"
#include "world_collision_rules.h"

static N2Mesh mesh(uint32_t key,uint64_t id,float x,int ground) {
    float verts[]={x,-4,0,0,0,x,4,0,0,0,x,4,1,0,0,x,-4,1,0,0,
                   x-5,-5,0,0,0,x+5,-5,0,0,0,x+5,5,0,0,0,x-5,5,0,0,0};
    uint16_t idx[]={0,1,2,0,2,3,4,5,6,4,6,7};
    N2Mesh m={0};m.verts=malloc(sizeof verts);m.idx=malloc(sizeof idx);assert(m.verts&&m.idx);
    memcpy(m.verts,verts,sizeof verts);memcpy(m.idx,idx,sizeof idx);
    m.nverts=8;m.nidx=ground?12:6;m.cat=ground?N2_TERRAIN:N2_OTHER;
    m.scen=ground?N2_SC_TERRAIN:N2_SC_WALL;m.world_model_key=key;m.placement_id=id;
    strcpy(m.sname,"TRUNCATED_SHARED_NAME");return m;
}
static int contact(const N2Mesh *m,float x,float z) {
    N2Scene s={(N2Mesh*)m,1,1};float pos[]={x,0,z},vel[]={.1f,0};
    float bb[]={-.8f,-.4f,0,.8f,.4f,1.4f};
    return collide_body_mesh_wall(pos,vel,0,bb,z+.05f,z+1.4f,&s,0,.30f,INFINITY,NULL);
}
static void compiled_rules(void) {
    const WCollisionEdits *e=&city_collision_edits;
    assert(wce_map(e,"STREAML4RA")&&wce_map(e,"STREAML4RA.BUN")&&!wce_map(e,"STREAML4RB"));
    for (int n=0;n<e->count;n++) {
        const WCollisionRule *r=e->rules+n;
        assert(r->kind>=0&&r->kind<=2&&isfinite(r->height)&&r->height>=0&&r->height<=1000);
        assert(r->kind==2 ? !r->key : r->key!=0);
        if (r->kind==0) assert(r->key<=UINT32_MAX&&!r->count&&!r->points);
        else assert(r->height>0&&r->count>=2&&r->count<=16383&&r->points);
        for (int k=0;k<r->count;k++) for (int a=0;a<3;a++)
            assert(isfinite(r->points[k][a])&&fabsf(r->points[k][a])<100000);
        for (int k=0;k<n;k++) assert(r->kind==2||r->kind!=e->rules[k].kind||r->key!=e->rules[k].key);
    }
}
int main(void) {
    compiled_rules();
    static const float replacement[][3]={{16,-4,0},{16,4,0}},standalone[][3]={{40,-4,0},{40,4,0}};
    static const WCollisionRule rules[]={
        {0,10,0,0,NULL},{0,11,8,0,NULL},{1,202,8,2,replacement},{2,0,8,2,standalone}
    };
    const WCollisionEdits e={"TEST",4,rules};
    N2Scene s={0};s.count=s.cap=6;s.meshes=calloc(6,sizeof *s.meshes);assert(s.meshes);
    s.meshes[0]=mesh(10,101,0,1);s.meshes[1]=mesh(10,102,4,1);
    s.meshes[2]=mesh(20,103,6,0); /* same name, different model: keep */
    s.meshes[3]=mesh(11,202,12,0);s.meshes[4]=mesh(11,202,12,0);
    s.meshes[5]=mesh(11,203,30,0);
    uint64_t ids[6];float saved[6][40];uint16_t indices[6][12];
    for (int n=0;n<6;n++) {ids[n]=n2_world_source_id(s.meshes+n);memcpy(saved[n],s.meshes[n].verts,sizeof saved[n]);
        memcpy(indices[n],s.meshes[n].idx,sizeof indices[n]);}
    assert(contact(s.meshes,.1f,0));assert(wce_apply(&e,&s)&&s.count==7);
    for (int n=0;n<6;n++) {
        assert(ids[n]==n2_world_source_id(s.meshes+n));
        assert(!memcmp(saved[n],s.meshes[n].verts,sizeof saved[n]));
        assert(!memcmp(indices[n],s.meshes[n].idx,sizeof indices[n]));
    }
    assert(!contact(s.meshes,.1f,0)&&!contact(s.meshes+1,4.1f,0));
    assert(contact(s.meshes+2,6.1f,0));assert(!contact(s.meshes+3,12.1f,0));
    assert(contact(s.meshes+3,16.1f,6)&&!contact(s.meshes+4,16.1f,6));
    assert(contact(s.meshes+5,30.1f,6)&&!contact(s.meshes+5,30.1f,9));
    assert(contact(s.meshes+6,40.1f,6)&&s.meshes[6].cat==N2_COLLISION);
    N2Scene invisible={s.meshes+6,1,1};float bounds[][4]={{40,-4,40,4}};GLuint textures[]={0};
    WorldBatchUpload *upload=upload_world_batches_begin(&invisible,bounds,textures,0,NULL,NULL);
    N2Batch *batches=NULL;int nbatch=0;assert(upload);
    assert(upload_world_batches_step(&upload,1,&batches,&nbatch,NULL)==1&&!upload&&nbatch==0);
    free(batches);
    float faces[9];assert(!phys_wall_debug_face(s.meshes,0,.3f,faces));
    float ground=99;N2Scene terrain={s.meshes,2,2};
    assert(world_ground_at(&terrain,-1,0,1,&ground)!=WSURF_NONE&&fabsf(ground)<1e-6f);
    float from[]={-1,0,1},to[]={-1,0,-1};assert(world_ground_sweep(&terrain,from,to,NULL)<1);
    float anchor[]={-2,0,.5f},eye[]={2,0,.5f};N2Scene one={s.meshes,1,1};
    assert(world_camera_clip(&one,NULL,anchor,eye,.1f)==1);
    one.meshes=s.meshes+3;anchor[0]=14;eye[0]=18;anchor[2]=eye[2]=6;
    assert(world_camera_clip(&one,NULL,anchor,eye,.1f)<1);
    float obst[8][4],z[8][2];int src[8],count=phys_collect_walls(&s,obst,src,z,8);
    assert(count==4);
    const float bb[]={-.8f,-.4f,0,.8f,.4f,1.4f};
    for (int side=-1;side<=1;side+=2) {
        float old[]={40+4*side,0,6},pos[]={40-4*side,0,6},vel[]={-side*2,0};
        assert(world_body_walls_move(&s,old,pos,vel,0,bb,6,7.4f,-.5f,
            obst,z,count,src,NULL,0,NULL,NULL)>0);
        assert(side*(pos[0]-40)>.79f);
    }
    for (int n=0;n<s.count;n++) {free(s.meshes[n].verts);free(s.meshes[n].idx);
        free(s.meshes[n].wall_verts);free(s.meshes[n].wall_idx);}
    free(s.meshes);puts("world_collision_test: PASS");return 0;
}
