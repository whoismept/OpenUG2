#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ug2_track_collision.h"
static void u16(unsigned char *p,unsigned value){p[0]=value;p[1]=value>>8;}
static void u32(unsigned char *p,uint32_t value){for(int k=0;k<4;k++)p[k]=(unsigned char)(value>>(8*k));}
static void f32(unsigned char *p,float f){uint32_t bits;memcpy(&bits,&f,4);u32(p,bits);}
static size_t leaf(unsigned char *d,size_t p,uint32_t id,size_t size){u32(d+p,id);u32(d+p+4,(uint32_t)size);return p+8;}
static size_t groups_fixture(unsigned char *d) {
    static const char *names[]={"NONE","PLAYER_BARRIERS_4004","BARRIERS_4103","BARRIERS_CAREER0","BARRIERS_DEMO","COMMON"};
    memset(d,0,512);size_t p=leaf(d,0,0x3412f,32*6);
    for(int k=0;k<6;k++) {
        unsigned char *r=d+p+32*k;uint32_t hash=0xffffffffu;
        for(const char *c=names[k];*c;c++)hash=hash*33u+(unsigned char)*c;
        u32(r,k?hash:0);u32(r+4,17);r[8]=1;strcpy((char*)r+9,names[k]);
    }
    return p+32*6;
}
static void groups_test(void) {
    unsigned char data[512],bad[512];size_t size=groups_fixture(data);UG2TrackGroups table={0};
    assert(ug2_track_groups_read(data,size,&table));
    assert(table.count==6 && table.source_offset==8 && !memcmp(table.records[1],data+40,32));
    UG2TrackGroups saved=table;
    assert(!ug2_track_groups_read(data,size-1,&table) && !memcmp(&table,&saved,sizeof table));
    memcpy(bad,data,size);u32(bad+4,191);assert(!ug2_track_groups_read(bad,size,&table));
    memcpy(bad,data,size);memset(bad+40+9,'A',23);assert(!ug2_track_groups_read(bad,size,&table));
    memcpy(bad,data,size);bad[40+9]=1;assert(!ug2_track_groups_read(bad,size,&table));
    memcpy(bad,data,size);u32(bad+40,0);assert(!ug2_track_groups_read(bad,size,&table));
    memcpy(bad,data,size);u32(bad+8,1);assert(!ug2_track_groups_read(bad,size,&table));
    memcpy(bad,data,size);memcpy(bad+size,data,size);assert(!ug2_track_groups_read(bad,2*size,&table));
    assert(!ug2_track_groups_read(bad,size+1,&table)); /* valid table, malformed tail */
    unsigned char oversized[8+257*32]={0};leaf(oversized,0,0x3412f,257*32);
    memcpy(oversized+8,data+8,32);
    for(int k=1;k<257;k++)memcpy(oversized+8+32*k,data+40,32);
    assert(!ug2_track_groups_read(oversized,sizeof oversized,&table));
    UG2TrackGroups limit={0};u32(oversized+4,256*32);
    assert(ug2_track_groups_read(oversized,8+256*32,&limit) && limit.count==256);
    memset(bad,0,sizeof bad);leaf(bad,0,0x3412f,0);assert(!ug2_track_groups_read(bad,8,&table));
    u32(bad,0);assert(!ug2_track_groups_read(bad,8,&table));
    assert(!memcmp(&table,&saved,sizeof table));
}
static size_t fixture(unsigned char *d) {
    memset(d,0,512);size_t h=leaf(d,8,0x34131,48);u32(d+h,121);u16(d+h+4,2);u16(d+h+6,2);
    f32(d+h+8,-32);f32(d+h+12,-32);
    const float limits[]={-2,-1,10,0,4,1,12,0};for(int k=0;k<8;k++)f32(d+h+16+4*k,limits[k]);
    size_t poly=leaf(d,h+48,0x34134,96);
    const int xyz[3][12]={{-16,-8,-8,-16,-8,-8,0,0,128,128,128,128},
        {16,16,16,16,-8,8,8,-8,0,0,512,512},
        {24,32,24,24,0,0,8,8,256,256,256,256}};
    for(int n=0;n<3;n++) {unsigned char *p=d+poly+32*n;p[0]=5;p[1]=79;u16(p+2,0x1234);u16(p+4,10);u16(p+6,65535);
        for(int k=0;k<12;k++)u16(p+8+2*k,(unsigned)xyz[n][k]);}
    size_t refs=leaf(d,poly+96,0x34136,16);const unsigned ids[]={0,1,1,2};
    for(int k=0;k<4;k++){u16(d+refs+4*k,ids[k]);u16(d+refs+4*k+2,0x8000);}
    size_t cells=leaf(d,refs+16,0x34137,16);const unsigned ranges[]={0,1,1,1,2,0,2,2};
    for(int k=0;k<8;k++)u16(d+cells+2*k,ranges[k]);
    size_t end=cells+16;u32(d,0x80034130);u32(d+4,(uint32_t)(end-8));return end;
}
int main(void) {
    groups_test();
    unsigned char companion[512];size_t companion_size=groups_fixture(companion);UG2TrackGroups activation={0};
    assert(ug2_track_groups_read(companion,companion_size,&activation));
    unsigned char data[512],bad[512];size_t size=fixture(data);UG2TrackCollision out={0};
    assert(ug2_track_collision_read(data,size,&activation,NULL,&out));
    assert(out.count==3 && out.groups==1 && out.total==3);
    /* A ready set carries the S5-11 CSR lookup index (grid dims > 0). */
    assert(out.index.start && out.index.list && out.index.gw>0 && out.index.gh>0);
    assert(out.index.start[0]==0);
    assert(out.polygons[0].corners[0][0]==-2 && out.polygons[0].corners[0][1]==-1 && out.polygons[0].corners[0][2]==10.5f);
    assert(out.polygons[1].corners[2][2]==12 && out.polygons[1].section==121 && out.polygons[1].source_offset==104);
    assert(!memcmp(out.polygons[1].metadata,data+104,8));
    /* The +2 attribute byte is exposed as stored, not translated (S5-11). */
    assert(out.polygons[0].attribute==data[72+2] && out.polygons[1].attribute==data[104+2]);
    assert(out.polygons[1].attribute==(unsigned char)0x34 && out.polygons[1].metadata[2]==out.polygons[1].attribute);
    float bounds[]={1.9f,-.2f,2.1f,.2f};assert(ug2_track_collision_read(data,size,&activation,bounds,&out));
    assert(out.count==1 && out.total==3 && out.polygons[0].source_offset==104);
    UG2TrackPolygon *saved=out.polygons;
    assert(!ug2_track_collision_read(data,size-1,&activation,NULL,&out) && out.polygons==saved);
    memcpy(bad,data,size);memcpy(bad+size,data,size);
    assert(!ug2_track_collision_read(bad,2*size-1,&activation,NULL,&out) && out.polygons==saved && out.count==1);
    const size_t offsets[]={68,12,72+6,176,204,20};
    for(int k=0;k<6;k++) {
        memcpy(bad,data,size);
        if(k==0)u32(bad+offsets[k],95); /* fractional polygon record */
        if(k==1)u32(bad+offsets[k],47); /* wrong header shape */
        if(k==2)u16(bad+offsets[k],0); /* unrecognized record sentinel */
        if(k==3)u16(bad+offsets[k],3); /* reference beyond polygon set */
        if(k==4)u16(bad+offsets[k],0); /* cell ranges do not partition references */
        if(k==5)u16(bad+offsets[k],3); /* grid dimensions disagree */
        assert(!ug2_track_collision_read(bad,size,&activation,NULL,&out) && out.polygons==saved && out.count==1);
    }
    memcpy(bad,data,size);f32(bad+32,NAN);assert(!ug2_track_collision_read(bad,size,&activation,NULL,&out));
    memcpy(bad,data,size);u16(bad+80,30000);assert(!ug2_track_collision_read(bad,size,&activation,NULL,&out));
    memcpy(bad,data,size);bad[72]=6;
    assert(!ug2_track_collision_read(bad,size,&activation,bounds,&out) && out.polygons==saved);
    assert(!ug2_track_collision_read(data,size,NULL,NULL,&out) && out.polygons==saved);
    float empty[]={40,40,50,50};assert(ug2_track_collision_read(data,size,&activation,empty,&out) && !out.count && out.total==3);
    assert(!out.index.start && !out.index.list && !out.index.gw && !out.index.gh);
    ug2_track_collision_free(&out);ug2_track_collision_free(&out);
    /* Empty original sections retain an inverted finite bounds sentinel. */
    memset(data,0,sizeof data);size_t h=leaf(data,8,0x34131,48);
    u16(data+h+4,1);u16(data+h+6,1);
    for(int k=0;k<3;k++){f32(data+h+16+4*k,0x1.fffffep127f);f32(data+h+32+4*k,-0x1.fffffep127f);}
    size_t p=leaf(data,h+48,0x34134,0);p=leaf(data,p,0x34136,0);p=leaf(data,p,0x34137,4);
    size=p+4;u32(data,0x80034130);u32(data+4,(uint32_t)(size-8));
    assert(ug2_track_collision_read(data,size,&activation,NULL,&out) && out.groups==1 && !out.count && !out.total);
    assert(!out.index.start);
    ug2_track_collision_free(&out);
    puts("ug2_track_collision_test: PASS");
}
