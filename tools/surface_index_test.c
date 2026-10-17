/* S5-11 lookup-index equivalence and timing (performance-only change).
 *
 * The S5-11 world attribute selector may be accelerated by the CSR XY grid in
 * ug2_track_collision.h, but its polygon choice must be byte-identical to the
 * full scan. For synthetic polygon sets (empty, single, stacked/overlapping,
 * near-vertical and steep faces, cell-boundary points, negative coordinates,
 * out-of-grid points) and a deterministic LCG fuzz, this compares the indexed
 * lookup and the unindexed fallback against an independent brute-force
 * reference written directly from the same selection rule. A timing check
 * shows the indexed path is dramatically faster on a large synthetic set.
 *
 * Synthetic records only; no original data or runtime capture is read.
 *
 *   make surface-index-test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ug2_track_collision.h"

static long checks,failures,found_ref,missing_ref;
static void chk(int ok,const char *what) {
    checks++;
    if(!ok){failures++;printf("FAIL %s\n",what);}
}

static unsigned rng;
static unsigned rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static float frnd(float lo,float hi){return lo+(hi-lo)*(float)(rnd()&0xffffff)/16777215.0f;}

/* Independent brute force: the S5-11 selection rule over every polygon. */
static int ref_attribute(const UG2TrackPolygon *polys,size_t count,float x,float y,
                         float wheel_z,float reach_up,float reach_down) {
    static const int tri[2][3]={{0,1,2},{0,2,3}};
    int found=0,best=0;float best_key=1e30f;
    for(size_t k=0;k<count;k++) {
        const UG2TrackPolygon *poly=polys+k;
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
            if(!(nl>1e-9f) || fabsf(nz)<.30f*nl)continue;
            int ramp=fabsf(nx)+fabsf(ny)>fabsf(nz)*.005f;
            float key=dz>0 && ramp && u>=0 && v>=0 && w>=0 ? -dz : fabsf(dz);
            if(found && key>=best_key)continue;
            found=1;best_key=key;best=poly->attribute;
        }
    }
    return found?best:-1;
}

/* Indexed lookup, unindexed fallback and reference must all agree. */
static void check_query(UG2TrackCollision *indexed,UG2TrackPolygon *polys,size_t count,
                        float x,float y,float wheel_z,float reach_up,float reach_down,
                        const char *what) {
    int ref=ref_attribute(polys,count,x,y,wheel_z,reach_up,reach_down);
    int got=ug2_tc_surface_attribute(indexed,x,y,wheel_z,reach_up,reach_down);
    UG2TrackCollision plain={.polygons=polys,.count=count};
    int scan=ug2_tc_surface_attribute(&plain,x,y,wheel_z,reach_up,reach_down);
    if(ref<0)missing_ref++;else found_ref++;
    checks++;
    if(got!=ref || scan!=ref) {
        failures++;
        printf("FAIL %s: x=%.9g y=%.9g z=%.9g up=%.9g down=%.9g ref=%d indexed=%d scan=%d\n",
               what,x,y,wheel_z,reach_up,reach_down,ref,got,scan);
    }
}

static void quad(UG2TrackPolygon *p,float x0,float y0,float x1,float y1,float z,unsigned attr) {
    const float c[4][3]={{x0,y0,z},{x1,y0,z},{x1,y1,z},{x0,y1,z}};
    memset(p,0,sizeof *p);memcpy(p->corners,c,sizeof c);p->attribute=(unsigned char)attr;
}
/* Flat in y, linear in x: plane slope s has nz/nl = 1/sqrt(1+s*s). */
static void ramp(UG2TrackPolygon *p,float x0,float y0,float x1,float y1,
                 float z0,float slope,unsigned attr) {
    float z1=z0+slope*(x1-x0);
    const float c[4][3]={{x0,y0,z0},{x1,y0,z1},{x1,y1,z1},{x0,y1,z0}};
    memset(p,0,sizeof *p);memcpy(p->corners,c,sizeof c);p->attribute=(unsigned char)attr;
}
/* Plane x = const: both triangles are exactly vertical. */
static void wall(UG2TrackPolygon *p,float x,float y0,float y1,float z0,float z1,unsigned attr) {
    const float c[4][3]={{x,y0,z0},{x,y1,z0},{x,y1,z1},{x,y0,z1}};
    memset(p,0,sizeof *p);memcpy(p->corners,c,sizeof c);p->attribute=(unsigned char)attr;
}

static void basics(void) {
    UG2TrackCollision data={0};
    static UG2TrackPolygon p[8];
    chk(ug2_track_collision_index_build(&data)==1,"empty set builds an empty index");
    chk(ug2_tc_surface_attribute(&data,0,0,0,1,1)==-1,"empty set selects nothing");
    chk(ug2_tc_surface_attribute(NULL,0,0,0,1,1)==-1,"null set selects nothing");
    check_query(&data,NULL,0,.5f,.5f,0,1,1,"empty set query");
    chk(ug2_track_collision_index_build(NULL)==0,"null owner refuses the index");

    quad(&p[0],0,0,10,10,0,20);
    data.polygons=p;data.count=1;data.total=1;
    chk(ug2_track_collision_index_build(&data)==1,"single polygon builds an index");
    chk(data.index.start&&data.index.list&&data.index.gw>0&&data.index.gh>0,"single polygon index present");
    check_query(&data,p,1,5,5,.1f,1,1,"center");
    check_query(&data,p,1,0,0,.1f,1,1,"low corner");
    check_query(&data,p,1,10,10,.1f,1,1,"high corner");
    check_query(&data,p,1,0,.5f,0,1,1,"bbox edge");
    check_query(&data,p,1,5,5,1,1,1,"exact upper reach edge");
    check_query(&data,p,1,5,5,-1,1,1,"exact lower reach edge");
    check_query(&data,p,1,5,5,1.0001f,1,1,"above the reach window");
    check_query(&data,p,1,5,5,-1.0001f,1,1,"below the reach window");
    check_query(&data,p,1,-1,-1,.1f,1,1,"outside the bbox");
    check_query(&data,p,1,11,5,.1f,1,1,"east of the bbox");
    check_query(&data,p,1,5,-1,.1f,1,1,"south of the bbox");
    check_query(&data,p,1,-.05f,5,.1f,1,1,"inside the barycentric tolerance");
    check_query(&data,p,1,-.2f,5,.1f,1,1,"outside the barycentric tolerance");
    /* A large polygon accepted from just outside the grid's raw bounds. */
    quad(&p[0],0,0,100,100,0,7);
    chk(ug2_track_collision_index_build(&data)==1,"large polygon rebuilds");
    check_query(&data,p,1,-.5f,50,.1f,1,1,"left of the grid, inside tolerance");
    check_query(&data,p,1,-2,50,.1f,1,1,"left of the grid, outside tolerance");
    check_query(&data,p,1,100.5f,50,.1f,1,1,"right of the raw bound, inside tolerance");
    check_query(&data,p,1,50,-.5f,.1f,1,1,"below the grid, inside tolerance");
    check_query(&data,p,1,50,100.5f,.1f,1,1,"above the raw bound, inside tolerance");
    ug2_tc_index_reset(&data.index); /* polygons are static; release only the index */
}

static void stacked(void) {
    static UG2TrackPolygon p[6];
    UG2TrackCollision data={0};
    quad(&p[0],0,0,10,10,0,5);
    quad(&p[1],0,0,10,10,2,9);
    data.polygons=p;data.count=2;data.total=2;
    chk(ug2_track_collision_index_build(&data)==1,"stacked set builds");
    check_query(&data,p,2,5,5,1.9f,1,1,"stacked: upper polygon is nearer");
    check_query(&data,p,2,5,5,.1f,1,1,"stacked: lower polygon is nearer");
    check_query(&data,p,2,5,5,1,1,1,"stacked: equal keys keep the earlier polygon");
    check_query(&data,p,2,5,5,2.5f,1,1,"stacked: outside both windows");
    /* Many identical overlapping polygons: ties must keep index order. */
    for(int k=0;k<5;k++)quad(&p[k],0,0,10,10,(float)k,10+k);
    data.count=5;data.total=5;
    chk(ug2_track_collision_index_build(&data)==1,"overlap set rebuilds");
    for(int k=0;k<5;k++)
        check_query(&data,p,5,5,5,(float)k,1,1,"overlap tie keeps the earliest polygon");
    ug2_tc_index_reset(&data.index);
}

static void slopes(void) {
    static UG2TrackPolygon p[4];
    UG2TrackCollision data={0};
    data.polygons=p;data.total=1;
    /* Exactly vertical face is a wall, never support. */
    wall(&p[0],5,0,10,0,2,20);data.count=1;
    chk(ug2_track_collision_index_build(&data)==1,"wall set builds");
    check_query(&data,p,1,5,5,1,2,2,"vertical face selects nothing");
    /* nz/nl crosses 0.30 between slope 3 and 3.5. */
    ramp(&p[0],0,0,10,10,0,3.f,21);data.count=1;
    chk(ug2_track_collision_index_build(&data)==1,"steep ramp builds");
    check_query(&data,p,1,5,5,3,4,4,"slope 3 ramp is accepted");
    ramp(&p[0],0,0,10,10,0,3.5f,22);
    chk(ug2_track_collision_index_build(&data)==1,"very steep ramp builds");
    check_query(&data,p,1,5,5,3,4,4,"slope 3.5 ramp is rejected");
    /* Rising-ramp preference: the above-wheel ramp key -dz beats the nearer
       below-wheel flat face. */
    quad(&p[0],0,0,10,10,0,30);
    ramp(&p[1],0,0,10,10,4,1.f,31);data.count=2;
    chk(ug2_track_collision_index_build(&data)==1,"ramp-preference set builds");
    check_query(&data,p,2,5,5,3,4,4,"rising ramp wins over the nearer flat face");
    ug2_tc_index_reset(&data.index);
}

static void boundaries(void) {
    enum { N=9 };
    static UG2TrackPolygon p[N];
    UG2TrackCollision data={0};
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)
        quad(&p[i*3+j],(float)i*UG2_TC_INDEX_CELL,(float)j*UG2_TC_INDEX_CELL,
             (float)i*UG2_TC_INDEX_CELL+1,(float)j*UG2_TC_INDEX_CELL+1,0,(unsigned)(i*3+j+1));
    data.polygons=p;data.count=N;data.total=N;
    chk(ug2_track_collision_index_build(&data)==1,"lattice builds");
    chk(data.index.x0==0&&data.index.y0==0,"lattice origin is the raw minimum");
    for(int k=0;k<=3;k++)for(int m=0;m<=3;m++) {
        float x=(float)k*UG2_TC_INDEX_CELL,y=(float)m*UG2_TC_INDEX_CELL;
        check_query(&data,p,N,x,y,0,1,1,"query exactly on a cell boundary");
        check_query(&data,p,N,x-1e-4f,y,0,1,1,"query just below a cell boundary");
        check_query(&data,p,N,x+1e-4f,y,0,1,1,"query just above a cell boundary");
        check_query(&data,p,N,x,y-1e-4f,0,1,1,"query just below a cell boundary in y");
        check_query(&data,p,N,x,y+1e-4f,0,1,1,"query just above a cell boundary in y");
    }
    /* A polygon straddling a cell boundary must be reachable from both sides. */
    quad(&p[0],(float)UG2_TC_INDEX_CELL-1,(float)UG2_TC_INDEX_CELL-1,
         (float)UG2_TC_INDEX_CELL+1,(float)UG2_TC_INDEX_CELL+1,5,40);
    data.count=1;data.total=1;
    chk(ug2_track_collision_index_build(&data)==1,"straddling polygon builds");
    check_query(&data,p,1,UG2_TC_INDEX_CELL-1+1e-3f,UG2_TC_INDEX_CELL,5,1,1,"straddle left cell");
    check_query(&data,p,1,UG2_TC_INDEX_CELL+1-1e-3f,UG2_TC_INDEX_CELL,5,1,1,"straddle right cell");
    check_query(&data,p,1,UG2_TC_INDEX_CELL,UG2_TC_INDEX_CELL,5,1,1,"straddle boundary");
    ug2_tc_index_reset(&data.index);
}

static void negatives(void) {
    static UG2TrackPolygon p[4];
    UG2TrackCollision data={0};
    quad(&p[0],-700,-700,-660,-660,1,11);
    quad(&p[1],-650,-650,-610,-610,1,12);
    quad(&p[2],-100.5f,-100.5f,-99.5f,-99.5f,0,13);
    data.polygons=p;data.count=3;data.total=3;
    chk(ug2_track_collision_index_build(&data)==1,"negative-coordinate set builds");
    check_query(&data,p,3,-680,-680,1.1f,1,1,"negative center");
    check_query(&data,p,3,-655,-655,1.1f,1,1,"negative between");
    check_query(&data,p,3,-640,-640,1.1f,1,1,"negative outside");
    check_query(&data,p,3,-100.5f,-100.5f,0,2,2,"negative corner of the tiny quad");
    check_query(&data,p,3,-100,-655,1.1f,1,1,"mixed negative coordinates");
    for(int k=0;k<=4;k++) {
        float bx=data.index.x0+(float)k*UG2_TC_INDEX_CELL;
        float by=data.index.y0+(float)k*UG2_TC_INDEX_CELL;
        check_query(&data,p,3,bx,by,1.1f,1,1,"negative cell boundary");
        check_query(&data,p,3,bx-1e-4f,by,1.1f,1,1,"negative boundary minus epsilon");
        check_query(&data,p,3,bx,by+1e-4f,1.1f,1,1,"negative boundary plus epsilon in y");
    }
    ug2_tc_index_reset(&data.index);
}

static float q8(float v){return roundf(v*8.0f)/8.0f;}
static float q256(float v){return roundf(v*256.0f)/256.0f;}

/* Loaded polygons are quantized to s16/8 in x,y and base+s16/256 in z, well
   inside +/-4096. This fuzz mirrors that domain, including one-quantum
   slivers at large coordinates, where binary32 barycentric rounding is the
   only thing the dilation pad has to absorb. */
static void quantized_set(unsigned seed,int npolys,int nqueries) {
    UG2TrackPolygon *p=(UG2TrackPolygon*)calloc((size_t)npolys,sizeof *p);
    if(!p){chk(0,"quantized allocation");return;}
    rng=seed;
    for(int i=0;i<npolys;i++) {
        float cx=q8(frnd(-4000,4000)),cy=q8(frnd(-4000,4000));
        float hx=q8(frnd(0.125f,150)),hy=q8(frnd(0.125f,150));
        if(rnd()%6==0)hx=0.125f;                /* one-quantum slivers */
        if(rnd()%6==0)hy=0.125f;
        float z=q256(frnd(-50,200));
        unsigned attr=(unsigned)(rnd()%256);
        if(rnd()%4==0)ramp(&p[i],cx-hx,cy-hy,cx+hx,cy+hy,z,q256(frnd(-5,5)),attr);
        else quad(&p[i],cx-hx,cy-hy,cx+hx,cy+hy,z,attr);
    }
    UG2TrackCollision data={.polygons=p,.count=(size_t)npolys,.total=(size_t)npolys};
    if(!ug2_track_collision_index_build(&data)) {
        chk(0,"quantized index build");
        free(p);
        return;
    }
    for(int q=0;q<nqueries;q++) {
        float x,y,wz;
        if(rnd()%2) {
            int k=(int)(rnd()%(unsigned)npolys),v=(int)(rnd()%4u);
            x=q8(p[k].corners[v][0]+frnd(-.5f,.5f));
            y=q8(p[k].corners[v][1]+frnd(-.5f,.5f));
            wz=q256(p[k].corners[v][2]+frnd(-1.5f,1.5f));
        } else {
            x=q8(frnd(-4096,4096));y=q8(frnd(-4096,4096));wz=q256(frnd(-60,220));
        }
        float up=frnd(.05f,3),down=frnd(.05f,3);
        check_query(&data,p,(size_t)npolys,x,y,wz,up,down,"quantized");
        if(rnd()%8==0) { /* exactly on a vertex, outside both rasterisations */
            int k=(int)(rnd()%(unsigned)npolys);
            check_query(&data,p,(size_t)npolys,p[k].corners[0][0],p[k].corners[0][1],wz,up,down,"quantized vertex");
        }
    }
    ug2_track_collision_free(&data);
}

static void fuzz_set(unsigned seed,int npolys,int nqueries) {    UG2TrackPolygon *p=(UG2TrackPolygon*)calloc((size_t)npolys,sizeof *p);
    if(!p){chk(0,"fuzz allocation");return;}
    rng=seed;
    for(int i=0;i<npolys;i++) {
        float cx=frnd(-400,1600),cy=frnd(-400,1600);
        float hx=frnd(0.02f,30),hy=frnd(0.02f,30);
        if(rnd()%8==0)hx=frnd(0.005f,.1f);      /* slivers */
        if(rnd()%8==0)hy=frnd(0.005f,.1f);
        float z=frnd(-3,15);
        unsigned attr=(unsigned)(rnd()%256);    /* include out-of-domain bytes */
        if(rnd()%3==0)ramp(&p[i],cx-hx,cy-hy,cx+hx,cy+hy,z,frnd(-5,5),attr);
        else if(rnd()%5==0)wall(&p[i],cx,cy-hy,cy+hy,z,z+frnd(.1f,8),attr);
        else quad(&p[i],cx-hx,cy-hy,cx+hx,cy+hy,z,attr);
    }
    UG2TrackCollision data={.polygons=p,.count=(size_t)npolys,.total=(size_t)npolys};
    if(!ug2_track_collision_index_build(&data)) {
        chk(0,"fuzz index build");
        free(p);
        return;
    }
    for(int q=0;q<nqueries;q++) {
        float x,y,wz;
        if(rnd()%2) { /* half the queries land near a polygon, so hits (and
                         the nearest/ramp/tie branches) are well covered */
            int k=(int)(rnd()%(unsigned)npolys);
            float ccx=(p[k].corners[0][0]+p[k].corners[1][0]+p[k].corners[2][0]+p[k].corners[3][0])*.25f;
            float ccy=(p[k].corners[0][1]+p[k].corners[1][1]+p[k].corners[2][1]+p[k].corners[3][1])*.25f;
            float ccz=(p[k].corners[0][2]+p[k].corners[1][2]+p[k].corners[2][2]+p[k].corners[3][2])*.25f;
            float ex=fabsf(p[k].corners[0][0]-ccx),ey=fabsf(p[k].corners[0][1]-ccy);
            x=ccx+frnd(-1,1)*ex;y=ccy+frnd(-1,1)*ey;wz=ccz+frnd(-2,2);
        } else {
            x=frnd(-700,1900);y=frnd(-700,1900);wz=frnd(-8,25);
        }
        float up=frnd(.05f,3),down=frnd(.05f,3);
        if(rnd()%4==0) { /* query exactly at a vertex and just off it */
            int k=(int)(rnd()%(unsigned)npolys),v=(int)(rnd()%4u);
            x=p[k].corners[v][0]+(rnd()%3?0.0f:1e-4f);
            y=p[k].corners[v][1]+(rnd()%3?0.0f:-1e-4f);
        }
        check_query(&data,p,(size_t)npolys,x,y,wz,up,down,"fuzz");
    }
    ug2_track_collision_free(&data);
}

/* Extreme lattice slivers: long thin quads and degenerate/collinear records
   spanning the shipped coordinate range, where the barycentric ratios are the
   least well conditioned. */
/* Query points synthesized directly in the barycentric tolerance band around
   arbitrary quads, including points outside the raw XY bounds by the dilation
   amount, where an undersized index pad would lose the polygon. */
static void tolerance_band(unsigned seed,int npolys,int nqueries) {
    static const int tri[2][3]={{0,1,2},{0,2,3}};
    UG2TrackPolygon *p=(UG2TrackPolygon*)calloc((size_t)npolys,sizeof *p);
    if(!p){chk(0,"tolerance allocation");return;}
    rng=seed;
    for(int i=0;i<npolys;i++) {
        float cx=frnd(-4000,4000),cy=frnd(-4000,4000),h=frnd(0.05f,300);
        for(int v=0;v<4;v++) {
            p[i].corners[v][0]=q8(cx+frnd(-h,h));
            p[i].corners[v][1]=q8(cy+frnd(-h,h));
            p[i].corners[v][2]=q256(frnd(-50,200));
        }
        p[i].attribute=(unsigned char)(rnd()%256);
    }
    UG2TrackCollision data={.polygons=p,.count=(size_t)npolys,.total=(size_t)npolys};
    if(!ug2_track_collision_index_build(&data)){chk(0,"tolerance index build");free(p);return;}
    for(int q=0;q<nqueries;q++) {
        int k=(int)(rnd()%(unsigned)npolys),t=(int)(rnd()%2u);
        const float *a=p[k].corners[tri[t][0]],*b=p[k].corners[tri[t][1]],*c=p[k].corners[tri[t][2]];
        float u=frnd(-.02f,1.02f),v=frnd(-.02f,1.02f),w=1-u-v;
        float x=u*a[0]+v*b[0]+w*c[0];
        float y=u*a[1]+v*b[1]+w*c[1];
        float z=u*a[2]+v*b[2]+w*c[2];
        check_query(&data,p,(size_t)npolys,x,y,z+frnd(-1,1),frnd(.05f,3),frnd(.05f,3),
                    "tolerance band");
    }
    ug2_track_collision_free(&data);
}

static void slivers(void) {
    static UG2TrackPolygon p[4];
    UG2TrackCollision data={0};
    const float c[4][4][3]={
        /* long horizontal sliver, 1/8 m tall, one end tapered to a point */
        {{0,0,0},{4095.875f,0.125f,0},{4095.75f,0.125f,0},{0,0.25f,0}},
        /* 8 km long strip one quantum tall */
        {{-4096,-4096,1},{4095.875f,-4096,1},{4095.875f,-4095.875f,1},{-4096,-4095.875f,1}},
        /* thin diagonal sliver across the range */
        {{-4095.875f,-4096,2},{4095.875f,4095.75f,2},{4095.75f,4095.75f,2},{-4095.875f,-4095.875f,2}},
        /* fully degenerate: all four corners equal */
        {{1,1,3},{1,1,3},{1,1,3},{1,1,3}},
    };
    for(int k=0;k<4;k++) {
        memset(&p[k],0,sizeof p[k]);
        memcpy(p[k].corners,c[k],sizeof c[k]);
        p[k].attribute=(unsigned char)(50+k);
    }
    data.polygons=p;data.count=4;data.total=4;
    chk(ug2_track_collision_index_build(&data)==1,"sliver set builds");
    for(int k=0;k<4;k++)for(int e=0;e<4;e++)for(int s=0;s<=8;s++) {
        float t=(float)s/8;
        const float *a=p[k].corners[e],*b=p[k].corners[(e+1)%4];
        float x=a[0]+t*(b[0]-a[0]),y=a[1]+t*(b[1]-a[1]),wz=a[2]+t*(b[2]-a[2]);
        check_query(&data,p,4,x,y,wz,1,1,"sliver edge sample");
        check_query(&data,p,4,x+0.02f,y-0.02f,wz,1,1,"sliver edge jitter");
        check_query(&data,p,4,x-0.12f,y+0.12f,wz,1,1,"sliver edge outside jitter");
    }
    check_query(&data,p,4,2000,0.0625f,.1f,1,1,"sliver interior");
    check_query(&data,p,4,1,1,3,1,1,"degenerate polygon selects nothing");
    ug2_tc_index_reset(&data.index);
}

static void unbounded(void) {
    static UG2TrackPolygon p[1];
    UG2TrackCollision data={0};
    quad(&p[0],0,0,1,1,0,7);
    data.polygons=p;data.count=1;data.total=1;
    p[0].corners[0][0]=NAN;
    chk(!ug2_track_collision_index_build(&data),"non-finite bounds refuse the index");
    chk(!data.index.start&&!data.index.list,"failed build leaves no index");
    check_query(&data,p,1,.5f,.5f,0,1,1,"fallback keeps the scan on unbounded data");
    quad(&p[0],0,0,1,1,0,7);
    chk(ug2_track_collision_index_build(&data)==1,"rebuild after failure");
    chk(ug2_track_collision_index_build(&data)==1,"rebuild over an existing index");
    check_query(&data,p,1,.5f,.5f,.1f,1,1,"rebuild keeps the result");
    ug2_tc_index_reset(&data.index);
}

static void timing(void) {
    enum { NPOLY=100000,NQUERY=200 };
    UG2TrackPolygon *p=(UG2TrackPolygon*)calloc(NPOLY,sizeof *p);
    if(!p){chk(0,"timing allocation");return;}
    rng=0xdeadbeefu;
    for(int i=0;i<NPOLY;i++) {
        float cx=frnd(0,2000),cy=frnd(0,2000),hx=frnd(.5f,4),hy=frnd(.5f,4);
        quad(&p[i],cx-hx,cy-hy,cx+hx,cy+hy,frnd(0,2),(unsigned)(rnd()%29));
    }
    UG2TrackCollision data={.polygons=p,.count=NPOLY,.total=NPOLY};
    if(!ug2_track_collision_index_build(&data)){chk(0,"timing index build");free(p);return;}
    static float qx[NQUERY],qy[NQUERY],qz[NQUERY];
    rng=0xabcdef01u;
    for(int q=0;q<NQUERY;q++){qx[q]=frnd(0,2000);qy[q]=frnd(0,2000);qz[q]=frnd(0,2);}
    volatile long sink=0;
    clock_t t0=clock();
    for(int q=0;q<NQUERY;q++)sink+=ug2_tc_surface_attribute(&data,qx[q],qy[q],qz[q],1,1);
    clock_t t1=clock();
    for(int q=0;q<NQUERY;q++)sink+=ref_attribute(p,NPOLY,qx[q],qy[q],qz[q],1,1);
    clock_t t2=clock();
    double ti=(double)(t1-t0)/CLOCKS_PER_SEC,tb=(double)(t2-t1)/CLOCKS_PER_SEC;
    printf("surface_index_test timing: indexed %.4fs vs brute %.4fs (%.0fx, %d polys, %d queries)\n",
           ti,tb,ti>0?tb/ti:0.0,NPOLY,NQUERY);
    chk(tb>ti*4.0,"indexed lookup is dramatically faster than brute force");
    (void)sink;
    ug2_track_collision_free(&data);
}

int main(void) {
    basics();
    stacked();
    slopes();
    boundaries();
    negatives();
    fuzz_set(0x5eed0001u,250,2500);
    fuzz_set(0x5eed0002u,400,2500);
    fuzz_set(0x5eed0003u,120,2500);
    fuzz_set(0x5eed0004u,600,2000);
    fuzz_set(0x5eed0005u,1,2000);
    fuzz_set(0x5eed0006u,30,2500);
    quantized_set(0x5107ed01u,200,3000);
    quantized_set(0x5107ed02u,600,3000);
    quantized_set(0x5107ed03u,40,3000);
    quantized_set(0x5107ed04u,1200,2000);
    tolerance_band(0x7a11e001u,300,4000);
    tolerance_band(0x7a11e002u,80,4000);
    slivers();
    unbounded();
    timing();
    printf("surface_index_test: %ld checks, %ld failures (%ld hits, %ld misses)\n",
           checks,failures,found_ref,missing_ref);
    return failures?1:0;
}
