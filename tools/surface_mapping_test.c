/* S5-11 world surface attribute -> numeric surface ID (PHY-13).
 *
 * Loads the approved test-vectors/surface_mapping.json and checks the
 * implementation against every field: the identity mapping over the 0..28
 * channel domain, the shipped-value membership, the fixed fallback IDs, the
 * boundary/representative attribute vectors (out-of-domain = no defined row)
 * and the 29 x 152-byte source channel-record layout. Synthetic records and
 * polygons only; no original data or runtime capture is read.
 *
 *   make surface-mapping-test
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "contact.h"
#include "ug2_track_collision.h"

static const char *g_dir;
static int g_fail, checks;
#include "test_json.h"

static void chk(const char *what,int ok) {
    checks++;
    if(!ok){printf("FAIL %s\n",what);g_fail++;}
}
static int in_set(int v,const int *set,int n) {
    for(int k=0;k<n;k++)if(set[k]==v)return 1;
    return 0;
}

static void lef32(unsigned char *p,float v) {
    uint32_t b;memcpy(&b,&v,4);
    for(int k=0;k<4;k++)p[k]=(unsigned char)(b>>(8*k));
}
static UG2TrackPolygon *quad(UG2TrackPolygon *p,float x0,float y0,float x1,float y1,
                             float z,unsigned attribute) {
    const float c[4][3]={{x0,y0,z},{x1,y0,z},{x1,y1,z},{x0,y1,z}};
    memcpy(p->corners,c,sizeof c);p->attribute=(unsigned char)attribute;
    return p;
}

static void mapping_table(const J *root,const J *rows) {
    chk("mapping is the identity",!strcmp(jstrv(root,"mapping"),"identity"));
    const J *sd=jget(root,"surface_id");
    chk("surface_id.count is the 29-row domain",jnum(sd,"count",-1)==CT_SURFACE_ID_COUNT);
    chk("surface_id.min is the domain low bound",jnum(sd,"min",-1)==CT_SURFACE_ID_MIN);
    chk("surface_id.max is the domain high bound",jnum(sd,"max",-1)==CT_SURFACE_ID_MAX);
    chk("rows has one entry per domain id",rows&&rows->n==CT_SURFACE_ID_COUNT);
    int seen[CT_SURFACE_ID_COUNT]={0};
    for(int k=0;k<rows->n;k++) {
        const J *r=rows->kid[k];
        int attribute=(int)jnum(r,"attribute",-1),id=(int)jnum(r,"surface_id",-2);
        chk("row attribute is its index",attribute==k);
        chk("ct_surface_id maps the row by identity",ct_surface_id(attribute)==id);
        chk("row id selects an S5-7 channel row",ct_surface_id(attribute)>=0&&ct_surface(ct_surface_id(attribute))!=NULL);
        if(attribute>=0&&attribute<CT_SURFACE_ID_COUNT)seen[attribute]=1;
    }
    for(int k=0;k<CT_SURFACE_ID_COUNT;k++)chk("identity is total over 0..28",seen[k]);
    chk("out-of-domain 29 selects no row",ct_surface_id(CT_SURFACE_ID_COUNT)==CT_SURFACE_ID_INVALID);
    chk("negative attribute selects no row",ct_surface_id(-1)==CT_SURFACE_ID_INVALID);
}

static void shipped(const J *root,const J *rows) {
    const J *set=jget(root,"shipped_attribute_values");
    int values[256];int n=set?set->n:0;
    for(int k=0;k<n;k++)values[k]=(int)set->kid[k]->num;
    chk("shipped set is nonempty",n>0);
    for(int k=0;k<n;k++)
        chk("shipped attribute is inside 0..28",values[k]>=0&&values[k]<CT_SURFACE_ID_COUNT);
    for(int v=0;v<CT_SURFACE_ID_COUNT;v++)
        chk("used_by_shipped_data matches the shipped set",
            (jbool(rows->kid[v],"used_by_shipped_data")!=0)==(in_set(v,values,n)!=0));
    chk("shipped polygon total is positive",jnum(root,"shipped_polygon_count",0)>0);
    const J *dom=jget(root,"shipped_dominant");
    chk("dominant attribute is a shipped value",in_set((int)jnum(dom,"attribute",-1),values,n)!=0);
    double count=jnum(dom,"polygon_count",-1);
    chk("dominant count stays within the shipped total",
        count>=0&&count<=jnum(root,"shipped_polygon_count",0));
}

static void vectors(const J *root) {
    const J *vs=jget(root,"vectors");
    chk("boundary/representative vectors present",vs&&vs->n>=7);
    if(!vs)return;
    int nulls=0,ids=0;
    for(int k=0;k<vs->n;k++) {
        const J *v=vs->kid[k];
        int attribute=(int)jnum(v,"attribute",-1);
        const J *exp=jget(v,"expected_surface_id");
        if(exp&&exp->t==J_NULL) {
            nulls++;
            chk("out-of-domain vector resolves to no row",
                ct_surface_id(attribute)==CT_SURFACE_ID_INVALID);
        } else {
            ids++;
            chk("vector attribute maps to its own id",
                ct_surface_id(attribute)==(int)jnum(v,"expected_surface_id",-2));
        }
    }
    chk("both boundary kinds are covered",nulls>=1&&ids>=6);
}

static void fallbacks(const J *root) {
    const J *f=jget(root,"fallback_ids");
    chk("no-pack fallback is the fixed default",jnum(f,"no_collision_pack",-1)==CT_SURFACE_ID_DEFAULT);
    chk("no-polygon fallback is the fixed default",jnum(f,"no_polygon_found",-1)==CT_SURFACE_ID_DEFAULT);
    chk("forced-override fallback is the fixed default",jnum(f,"forced_override",-1)==CT_SURFACE_ID_DEFAULT);
    chk("failed-steep fallback is the fixed steep id",jnum(f,"failed_steep_normal_test",-1)==CT_SURFACE_ID_STEEP);

    /* Forced override substitutes ID 1 after bump sampling (S5-3/S5-11.5). */
    CtGround g={.height=0,.normal={0,0,1},.found=1,.surface=20,.override_surface=1};
    float p[3]={0,0,1},up[3]={0,0,1};int s=-1;
    ct_ground_distance(p,up,&g,0,&s);
    chk("forced override uses the fixed default",s==CT_SURFACE_ID_DEFAULT);
    /* A failed steep-normal test forces 0. */
    g.override_surface=0;g.normal[0]=1;g.normal[2]=0;
    ct_ground_distance(p,up,&g,0,&s);
    chk("failed steep test forces the steep id",s==CT_SURFACE_ID_STEEP);
    /* On the found path the selected polygon's own attribute is the id. */
    g.normal[0]=0;g.normal[2]=1;g.surface=20;
    ct_ground_distance(p,up,&g,0,&s);
    chk("found polygon keeps its attribute id",s==20);
}

static void selection(void) {
    UG2TrackPolygon p[4]={0};UG2TrackCollision data={0};
    chk("null set selects no polygon",ug2_tc_surface_attribute(NULL,.5f,.5f,.1f,1,1)==-1);
    chk("empty set selects no polygon",ug2_tc_surface_attribute(&data,.5f,.5f,.1f,1,1)==-1);
    quad(&p[0],0,0,1,1,0,20);
    data.polygons=p;data.count=1;data.total=1;
    chk("covering polygon returns its stored attribute",
        ug2_tc_surface_attribute(&data,.5f,.5f,.1f,1,1)==20);
    chk("polygon outside the reach window selects nothing",
        ug2_tc_surface_attribute(&data,.5f,.5f,10,1,1)==-1);
    quad(&p[1],0,0,1,1,2,15);data.count=2;data.total=2;
    chk("nearest polygon to the wheel wins",
        ug2_tc_surface_attribute(&data,.5f,.5f,1.9f,1,1)==15);
    /* Attribute 0 is a selected row, not the "no polygon" sentinel. */
    quad(&p[2],2,2,3,3,0,0);data.count=3;data.total=3;
    chk("attribute 0 selects row 0",ug2_tc_surface_attribute(&data,2.5f,2.5f,.1f,1,1)==0);
    /* The stored byte is returned unchanged; domain validity is the caller's check. */
    quad(&p[3],4,4,5,5,0,29);data.count=4;data.total=4;
    chk("stored byte above the domain is returned unchanged",
        ug2_tc_surface_attribute(&data,4.5f,4.5f,.1f,1,1)==29);
    chk("out-of-domain attribute selects no S5-7 row",ct_surface_id(29)==CT_SURFACE_ID_INVALID);
    /* A vertical face is a wall, never tyre support. */
    data.count=1;
    const float wall[4][3]={{0,5,0},{1,5,0},{1,5,1},{0,5,1}};
    memcpy(p[0].corners,wall,sizeof wall);p[0].attribute=20;
    chk("vertical face selects nothing",ug2_tc_surface_attribute(&data,.5f,5,.1f,1,1)==-1);
}

static CtGround probe(void *user,const float q[3],int wheel) {
    (void)user;(void)q;(void)wheel;
    CtGround g={.found=1,.height=0,.surface=20,.normal={0,0,1}};
    return g;
}
static void contact_carries_id(void) {
    CtState s;CtCar car={0};car.mass=1;
    for(int a=0;a<2;a++){car.axle[a].lower=-1;car.axle[a].upper=1;car.axle[a].stiffness=1;}
    for(int i=0;i<4;i++)car.radius[i]=.33f;
    ct_init(&s,&car);
    CtPose pose={0};
    pose.axes.forward[0]=1;pose.axes.lateral[1]=1;pose.axes.up[2]=1;
    pose.position[2]=.5f;
    ct_post(&s,&pose,(float[4]){0},1.0f,probe,NULL);
    for(int i=0;i<4;i++)chk("contact carries the selected polygon id",s.wheel[i].surface==20);
    ct_export(&s,&pose);
    for(int i=0;i<4;i++)chk("exported contact keeps the selected polygon id",s.exported_surface[i]==20);
}

static void layout(const J *root) {
    const J *c=jget(root,"channel_record");
    chk("record count is 29",jnum(c,"count",-1)==CT_SURFACE_RECORD_COUNT);
    chk("record size is 152 bytes",jnum(c,"record_size_bytes",-1)==CT_SURFACE_RECORD_SIZE);
    const J *off=jget(c,"channel_offsets");
    chk("frequency offset",jnum(off,"frequency",-1)==CT_SURFACE_OFFSET_FREQUENCY);
    chk("amplitude offset",jnum(off,"amplitude",-1)==CT_SURFACE_OFFSET_AMPLITUDE);
    chk("peak offset",jnum(off,"peak",-1)==CT_SURFACE_OFFSET_PEAK);
    chk("slide offset",jnum(off,"slide",-1)==CT_SURFACE_OFFSET_SLIDE);
    chk("spin offset",jnum(off,"spin",-1)==CT_SURFACE_OFFSET_SPIN);
    chk("body-drag offset",jnum(off,"body_drag",-1)==CT_SURFACE_OFFSET_BODY_DRAG);
    const J *dom=jget(c,"id_domain");
    assert(dom&&dom->n==2);
    chk("record id domain low",(int)dom->kid[0]->num==CT_SURFACE_ID_MIN);
    chk("record id domain high",(int)dom->kid[1]->num==CT_SURFACE_ID_MAX);
    /* Every surfaces.json row placed at its own record offsets decodes back to
       the same independent channels: no channel is reordered or shared. */
    J *sr=jload("surfaces.json");
    if(!sr)return;
    chk("29 channel rows",sr->n==CT_SURFACE_RECORD_COUNT);
    for(int k=0;k<sr->n;k++) {
        const J *row=sr->kid[k];
        chk("channel row id is its index",(int)jnum(row,"id",-1)==k);
        unsigned char rec[CT_SURFACE_RECORD_SIZE];memset(rec,0,sizeof rec);
        lef32(rec+CT_SURFACE_OFFSET_FREQUENCY,(float)jnum(row,"frequency",0));
        lef32(rec+CT_SURFACE_OFFSET_AMPLITUDE,(float)jnum(row,"amplitude",0));
        lef32(rec+CT_SURFACE_OFFSET_PEAK,(float)jnum(row,"peak",0));
        lef32(rec+CT_SURFACE_OFFSET_SLIDE,(float)jnum(row,"slide",0));
        lef32(rec+CT_SURFACE_OFFSET_SPIN,(float)jnum(row,"spin",0));
        lef32(rec+CT_SURFACE_OFFSET_BODY_DRAG,(float)jnum(row,"body_drag",0));
        CtSurface s;ct_surface_record(rec,&s);
        const CtSurface *t=ct_surface(k);
        chk("decoded record keeps the six independent channels",
            s.peak==t->peak&&s.slide==t->slide&&s.spin==t->spin&&
            s.body_drag==t->body_drag&&s.frequency==t->frequency&&s.amplitude==t->amplitude);
    }
    jfree(sr);
}

int main(int argc,char **argv) {
    g_dir=argc>1?argv[1]:"docs/cleanroom-driving-physics/stage5/test-vectors";
    J *root=jload("surface_mapping.json");
    if(!root)return 1;
    const J *rows=jget(root,"rows");
    assert(rows);
    mapping_table(root,rows);
    shipped(root,rows);
    vectors(root);
    fallbacks(root);
    selection();
    contact_carries_id();
    layout(root);
    jfree(root);
    printf("surface_mapping_test: %d checks, %d failures\n",checks,g_fail);
    return !!g_fail;
}
