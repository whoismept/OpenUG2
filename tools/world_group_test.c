#include <assert.h>
#include <stdio.h>
#include "../src/world_group_reader.h"
#include "../src/world_scenery.h"

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
    /* Normal open-world free roam must use the conservative selection that
     * removes event-exclusive placements. Explicit previews retain their
     * requested mode. A live race now REQUESTS its own authored group, so
     * another event's road closures cannot stand on the raced route; the
     * builder degrades a request the bundle does not author (M160). */
    assert(wg_runtime_selection(0, 0, 0) == -1);
    assert(wg_runtime_selection(1, -1, 0) == -1);
    assert(wg_runtime_selection(1, 4144, 0) == 4144);
    assert(wg_runtime_selection(0, 0, 4201) == 4201);
    assert(wg_runtime_selection(0, 0, 4701) == 4701);
    /* An explicit preview still wins over the active race id. */
    assert(wg_runtime_selection(1, -1, 4701) == -1);
    unsigned char ov[16], g[112];WGTable t;WGSelection sel_probe={0};
    fixture(ov,g);
    /* Catches missing decoder, wrong field offsets and a global/local index mixup. */
    assert(wg_open(ov,sizeof ov,g,sizeof g,&t));
    assert(t.override_count==2 && t.group_count==2);
    assert(wg_visit(&t,check_member,NULL) && visits==3);
    int calls=0;assert(!wg_visit(&t,cancel,&calls) && calls==1);
    /* Group presence is answered from the authored names only. The fixture
     * groups are "A" and "B", so no numeric event id is present, while
     * free roam and "unchanged" stay answerable without a lookup. */
    assert(wg_event_group_present(&t,-1) && wg_event_group_present(&t,0));
    assert(!wg_event_group_present(&t,4701));
    assert(!wg_event_group_present(NULL,4701));
    {   /* One real BARRIERS_<id> group: present for its own id only. Absence
         * of a sibling id is what the builder degrades on, and it must never
         * be confused with the table being rejected. */
        unsigned char bov[8], bg[56];
        WGTable bt;
        memset(bov,0,sizeof bov);memset(bg,0,sizeof bg);
        u16(bov+6,1);                       /* one reference to override 0 */
        memcpy(bg+8,"BARRIERS_4701",13);
        uint32_t h=0xffffffffu;
        for(const char *c="BARRIERS_4701";*c;c++)h=h*33u+(unsigned char)*c;
        u32(bg+40,h);u32(bg+48,1);u16(bg+52,0);
        assert(wg_open(bov,sizeof bov,bg,sizeof bg,&bt));
        assert(wg_event_group_present(&bt,4701));
        assert(!wg_event_group_present(&bt,4702));
        assert(wg_selection_open(&bt,4701,&sel_probe) && sel_probe.count==1);
        assert(sel_probe.items[0].membership==(WG_EVENT|WG_ACTIVE));
        assert(wg_selection_visible(&sel_probe,0,0));
        free(sel_probe.items);memset(&sel_probe,0,sizeof sel_probe);
        /* A sibling id the bundle does not author is rejected by the selection
         * helper; only wg_event_group_present separates that from corruption. */
        assert(!wg_selection_open(&bt,4702,&sel_probe));
    }

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
