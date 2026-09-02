#include <assert.h>
#include <stdio.h>
#include "../src/world_group_reader.h"

static void u16(unsigned char *p, unsigned n) {p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void u32(unsigned char *p, unsigned n) {u16(p,n);u16(p+2,n>>16);}
static void fixture(unsigned char *ov, unsigned char *g) {
    memset(ov,0,16);memset(g,0,112);
    u16(ov,17);u16(ov+2,3);u16(ov+4,20);u16(ov+6,2);
    u16(ov+8,18);u16(ov+10,7);u16(ov+12,0);u16(ov+14,1);
    /* Hash('A')=32, hash('B')=33 from seed -1, h*33+c. */
    u32(g,11);u32(g+4,11);g[8]='A';u32(g+40,32);u32(g+48,2);
    u16(g+52,1);u16(g+54,0);
    u32(g+56,11);u32(g+60,11);g[64]='B';u32(g+96,33);u32(g+104,1);
    u16(g+108,0);g[110]=0xcc;g[111]=0xcc; /* alignment is not a member */
}
static int visits;
static int check_member(const WGMember *m, void *ctx) {
    (void)ctx;
    const unsigned section[]={18,17,17}, row[]={7,3,3}, flags[]={0,20,20};
    const unsigned idx[]={1,0,0}, refs[]={1,2,2};
    assert(visits<3);
    assert(m->section==section[visits] && m->instance==row[visits]);
    assert(m->flags==flags[visits] && m->override_index==idx[visits]);
    assert(m->reference_count==refs[visits]);
    assert(!strcmp(m->group_name,visits==2?"B":"A"));
    assert(m->group_hash==(visits==2?33u:32u));
    visits++;return 1;
}
static int cancel(const WGMember *m, void *ctx) {(void)m;(*(int*)ctx)++;return 0;}
int main(void) {
    unsigned char ov[16], g[112];WGTable t;
    fixture(ov,g);
    /* Catches missing decoder, wrong field offsets and a global/local index mixup. */
    assert(wg_open(ov,sizeof ov,g,sizeof g,&t));
    assert(t.override_count==2 && t.group_count==2);
    assert(wg_visit(&t,check_member,NULL) && visits==3);
    int calls=0;assert(!wg_visit(&t,cancel,&calls) && calls==1);
    assert(!wg_visit(&t,NULL,NULL));
    /* Every truncation must fail before any consumer can use a partial table. */
    for(size_t n=0;n<sizeof g;n++)assert(!wg_open(ov,sizeof ov,g,n,&t));
    for(size_t n=0;n<sizeof ov;n++)assert(!wg_open(ov,n,g,sizeof g,&t));
    fixture(ov,g);u16(g+108,2);assert(!wg_open(ov,16,g,112,&t));
    assert(!t.groups && !t.overrides && !t.group_count);
    fixture(ov,g);u32(g+104,0xffffffffu);assert(!wg_open(ov,16,g,112,&t));
    fixture(ov,g);g[96]^=1;assert(!wg_open(ov,16,g,112,&t));
    fixture(ov,g);memset(g+64,'X',32);assert(!wg_open(ov,16,g,112,&t));
    fixture(ov,g);g[64]=0;assert(!wg_open(ov,16,g,112,&t));
    fixture(ov,g);u16(ov+6,1);assert(!wg_open(ov,16,g,112,&t));
    assert(!wg_open(NULL,16,g,112,&t));assert(!wg_open(ov,16,NULL,112,&t));
    assert(!wg_open(ov,16,g,112,NULL));
    assert(wg_open(NULL,0,NULL,0,&t));
    assert(wg_visit(&t,check_member,NULL) && visits==3);
    /* The audit must not hand a claimed chunk size past EOF to the reader. */
    unsigned char file[152]={0};fixture(ov,g);
    u32(file,0x80012345);u32(file+4,144);
    u32(file+8,0x34107);u32(file+12,16);memcpy(file+16,ov,16);
    u32(file+32,0x34108);u32(file+36,112);memcpy(file+40,g,112);
    assert(wg_open_file(file,sizeof file,&t));
    visits=0;assert(wg_visit(&t,check_member,NULL)&&visits==3);
    for(size_t n=0;n<sizeof file;n++)assert(!wg_open_file(file,n,&t));
    u32(file+4,0xffffffffu);assert(!wg_open_file(file,sizeof file,&t));u32(file+4,144);
    u32(file+36,0xffffffffu);assert(!wg_open_file(file,sizeof file,&t));u32(file+36,112);
    u32(file+12,4);assert(!wg_open_file(file,sizeof file,&t));u32(file+12,16);
    u32(file+32,0x34107);assert(!wg_open_file(file,sizeof file,&t));u32(file+32,0x34108);
    assert(!wg_open_file(NULL,sizeof file,&t));
    assert(!wg_open_file(file,sizeof file,NULL));
    unsigned char empty[16]={0};u32(empty,0x34107);u32(empty+8,0x34108);
    assert(wg_open_file(empty,sizeof empty,&t) && !t.group_count);
    unsigned char deep[70*8+sizeof file];
    for(unsigned i=0;i<70;i++) {u32(deep+8*i,0x80000001);u32(deep+8*i+4,(unsigned)sizeof deep-8*i-8);}
    memcpy(deep+70*8,file,sizeof file);
    assert(!wg_open_file(deep,sizeof deep,&t));
    puts("world_group_test: PASS (linkage, padding, truncation, corruption, cancellation, empty tables)");
    return 0;
}
