/* GL-free, read-only ownership audit. Uses the production U2 section decoder,
 * including its filler/boundary rules; no scene is built or filtered. */
#include "../src/world_instance.c"
#include "world_group_reader.h"

typedef struct {
    WInstSection *sections;
    unsigned char *present;
    const char *filter;
    size_t printed, refs;
} Audit;

static int index_sections(const unsigned char *d,long begin,long end,unsigned depth,Audit *a) {
    if(depth>64)return 0;
    for(long p=begin;p<end;) {
        long next;
        if(end-p<8 || !chunk_end(p+8,n2_u32(d+p+4),end,&next))return 0;
        uint32_t id=n2_u32(d+p);
        if(id==0x80034100u) {
            WInstSection s;
            if(!winst_parse_section(d,p+8,next,&s) || s.region_id<0 ||
               s.region_id>=65536 || a->present[s.region_id])return 0;
            a->sections[s.region_id]=s;a->present[s.region_id]=1;
        } else if(id && (id>>28)==8 && !index_sections(d,p+8,next,depth+1,a))return 0;
        p=next;
    }
    return 1;
}

static int member(const WGMember *m,void *context) {
    Audit *a=context;a->refs++;
    if(!a->filter || strcmp(a->filter,m->group_name))return 1;
    const WInstSection *s=&a->sections[m->section];
    WInstPlacement p;
    if(!winst_decode_placement(s->placements+64L*m->instance,64,&p) ||
       p.type_index>=(unsigned)s->type_count)return 0;
    char name[33];memcpy(name,s->types+68L*p.type_index,32);name[32]=0;
    printf("MEMBER group=%s override=%u section=%u row=%u flags=%04x refs=%u name=%s XYZ=(%.3f,%.3f,%.3f)\n",
        m->group_name,m->override_index,m->section,m->instance,m->flags,
        m->reference_count,name,p.matrix[12],p.matrix[13],p.matrix[14]);
    a->printed++;return 1;
}

int main(int argc,char **argv) {
    if(argc<3 || argc>4) {
        fprintf(stderr,"usage: world_group_audit TRACK_ROOT L4RA [EXACT_GROUP_NAME]\n");return 2;
    }
    if(strlen(argv[2])!=4 || strncmp(argv[2],"L4R",3) ||
       !strchr("ABCDFGHR",argv[2][3]))return 2;
    int result=1;long len=0,clen=0;char stream[16];
    snprintf(stream,sizeof stream,"STREAM%s",argv[2]);
    unsigned char *d=winst_read_named(argv[1],stream,&len);
    unsigned char *c=winst_read_named(argv[1],argv[2],&clen);
    Audit a={0};WGTable t;
    a.sections=calloc(65536,sizeof *a.sections);a.present=calloc(65536,1);
    a.filter=argc==4?argv[3]:NULL;
    if(!d||!c||!a.sections||!a.present)goto done;
    if(!wg_open_file(c,(size_t)clen,&t) || !index_sections(d,0,len,0,&a))goto done;
    /* Check every override, even one not referenced by any group. Reject
     * a wrong section/row, wrong flag copy or invalid model type before output. */
    for(size_t i=0;i<t.override_count;i++) {
        const unsigned char *r=t.overrides+8*i;
        unsigned sid=wg_u16(r),row=wg_u16(r+2);
        const WInstSection *s=&a.sections[sid];
        if(!a.present[sid] || row>=(unsigned)s->placement_count)goto done;
        const unsigned char *p=s->placements+64L*row;
        if(winst_u16(p+0x1a)!=wg_u16(r+4) || winst_u16(p+0x18)>=(unsigned)s->type_count)goto done;
    }
    printf("GROUP_AUDIT bundle=%s groups=%zu overrides=%zu flags_match=%zu\n",
           argv[2],t.group_count,t.override_count,t.override_count);
    if(!wg_visit(&t,member,&a))goto done;
    if(a.filter && !a.printed) {fprintf(stderr,"No members in requested group: %s\n",a.filter);result=2;goto done;}
    printf("GROUP_AUDIT references=%zu partition/hash/index/refcount/placement=PASS activation=UNDECODED\n",a.refs);
    result=0;
done:
    if(result==1)fprintf(stderr,"Group audit failed: missing, malformed or inconsistent input.\n");
    free(a.sections);free(a.present);free(d);free(c);return result;
}
