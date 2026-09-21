#ifndef OPENUG2_CAR_MOD_H
#define OPENUG2_CAR_MOD_H
#include "nfsu2.h"

static const char *const n2_mod_libraries[] = {
    "", "SPOILER", "SPOILER_HATCH", "SPOILER_SUV", "EXHAUST", "ROOF"
};

static void n2_mod_option(N2PartMenu *menu,int value,const char *label) {
    for(int i=0;i<menu->count;i++)if(menu->options[i].value==value)return;
    if(menu->count>=256)return;
    N2PartOption *o=&menu->options[menu->count++];o->value=value;
    snprintf(o->label,sizeof o->label,"%s",label);
}

/* Enumerate object names, not arbitrary byte runs or guessed numeric ranges. */
static void n2_mod_catalog_walk(const unsigned char *d,long beg,long end,
                                int library,N2PartMenu menus[N2_PART_COUNT]) {
    for(long o=beg;o+8<=end;) {
        uint32_t tag=n2_u32(d+o),size=n2_u32(d+o+4);long p=o+8;
        if((long)size>end-p)return;
        if(tag==0x80134010u) {
            char name[64],label[48];n2_car_mesh_name(d,p,p+size,name);
            int num=0;long at=0,n=0;int kind=n2_name_variant((unsigned char *)name,strlen(name),&num,&at,&n);
            int slot=n2_car_part(name);
            if(library)slot=library<=3?N2_PART_SPOILER:library==4?N2_PART_EXHAUST:N2_PART_SCOOP;
            if(slot>=0 && kind && !strstr(name,"_CF") && !strstr(name,"WIDE") && !strstr(name,"KITW")) {
                int value=(kind-1)*100+num+1;
                N2CarConfig cfg={.body_kit=num,.hood_style=num};
                if(!library || slot==N2_PART_EXHAUST)cfg.parts[slot]=value;
                N2Scene probe={0};n2_walk_car(d,o,p+size,&probe,NULL,0,&cfg);
                int renderable=probe.count>0;n2_free_scene(&probe);
                if(!renderable){o=p+size;continue;} /* named empty placeholders */
                if(library) {
                    int variant=strstr(name,"_DUAL")?1:strstr(name,"_OFFSET")?2:0;
                    value=library*1000+variant*100+num;
                    const char *type=library==1?"Coupe":library==2?"Hatch":library==3?"SUV":
                                     library==4?"Tip":variant==1?"Dual":variant==2?"Offset":"Single";
                    snprintf(label,sizeof label,"Style %02d (%s)",num,type);
                } else if(kind==1 && !num)snprintf(label,sizeof label,"Stock");
                else snprintf(label,sizeof label,"%s %02d",kind==1?"Kit":"Style",num);
                n2_mod_option(menus+slot,value,label);
            }
        } else if(tag && tag>>28==8)n2_mod_catalog_walk(d,p,p+size,library,menus);
        o=p+size;
    }
}

static void n2_mod_catalog(const char *root,const unsigned char *data,long len,
                            const N2Scene *car,N2PartMenu menus[N2_PART_COUNT]) {
    memset(menus,0,sizeof(N2PartMenu)*N2_PART_COUNT);
    for(int i=0;i<N2_PART_COUNT;i++)n2_mod_option(menus+i,0,i<N2_PART_SPOILER?"Follow kit preset":"Stock / none");
    n2_mod_catalog_walk(data,0,len,0,menus);
    for(int lib=1;lib<=5;lib++) {
        float m[16];
        int supported=lib<=3?n2_car_socket(data,len,car,0xc93b73fdu,m)==1:
                      lib==5?n2_car_socket(data,len,car,0x90c81258u,m)==1:
                      n2_car_socket(data,len,car,0xbcf8a18bu,m)==1 || n2_car_socket(data,len,car,0xbd7cf15eu,m)==1;
        if(!supported)continue;
        char path[1024];long bytes=0;
        snprintf(path,sizeof path,"%s/CARS/%s/GEOMETRY.BIN",root,n2_mod_libraries[lib]);
        unsigned char *d=n2_read_file(path,&bytes);
        if(d)n2_mod_catalog_walk(d,0,bytes,lib,menus);
        free(d);
    }
    for(int part=0;part<N2_PART_COUNT;part++) {
        N2PartMenu *m=menus+part;
        for(int i=1;i<m->count;i++) {
            N2PartOption option=m->options[i];int j=i;
            while(j && m->options[j-1].value>option.value){m->options[j]=m->options[j-1];j--;}
            m->options[j]=option;
        }
    }
}

/* Append complete material slices at the car's named socket. Spoilers/scoops
 * use source XYZ; exhaust tips use the same axis conversion as stock exhausts.
 * Painted library objects have empty texture packs and use material colours.
 * Carbon variants are excluded until their shared texture binding is supported. */
static int n2_mod_attach(const char *root,const unsigned char *data,long len,
                         int slot,int value,N2Scene *car) {
    int lib=value/1000,variant=value%1000/100,style=value%100;
    if(!root || lib<1 || lib>5 || !style || variant>2 ||
       (lib<=3 && slot!=N2_PART_SPOILER) || (lib==4 && slot!=N2_PART_EXHAUST) ||
       (lib==5 && slot!=N2_PART_SCOOP) || (lib!=5 && variant))return 0;
    float sockets[2][16];int ns=0;
    const uint32_t keys[]={slot==N2_PART_SPOILER?0xc93b73fdu:
                           slot==N2_PART_SCOOP?0x90c81258u:0xbcf8a18bu,0xbd7cf15eu};
    for(int i=0;i<(slot==N2_PART_EXHAUST?2:1);i++) {
        int found=n2_car_socket(data,len,car,keys[i],sockets[ns]);
        if(found<0)return 0;
        ns+=found;
    }
    if(!ns)return 0;
    char path[1024];long bytes=0;
    snprintf(path,sizeof path,"%s/CARS/%s/GEOMETRY.BIN",root,n2_mod_libraries[lib]);
    unsigned char *d=n2_read_file(path,&bytes);if(!d)return 0;
    N2CarConfig cfg={.hood_style=style};cfg.parts[N2_PART_EXHAUST]=101+style;
    N2Scene part={0};n2_walk_car(d,0,bytes,&part,NULL,0,&cfg);
    int kept=0;
    for(int i=0;i<part.count;i++) {
        N2Mesh *m=part.meshes+i;char name[64];long off=m->car_source;
        n2_car_mesh_name(d,off,off+n2_u32(d+off-4),name);
        int v=strstr(name,"_DUAL")?1:strstr(name,"_OFFSET")?2:0;
        if(strstr(name,"_CF") || v!=variant) {free(m->verts);free(m->idx);continue;}
        part.meshes[kept++]=*m;
    }
    part.count=kept;free(d);n2_car_dedupe_lod(&part);
    if(!part.count){n2_free_scene(&part);return 0;}
    int old=car->count,total=old+part.count*ns;
    N2Mesh *grown=(N2Mesh *)realloc(car->meshes,(size_t)total*sizeof *grown);
    if(!grown){n2_free_scene(&part);return 0;}car->meshes=grown;car->cap=total;
    for(int k=0;k<ns;k++)for(int i=0;i<part.count;i++) {
        N2Mesh *src=part.meshes+i,*dst=car->meshes+car->count++;
        *dst=*src;dst->car_source=0;dst->famkey=0;dst->car_part=(unsigned char)(slot+1);
        dst->verts=(float *)malloc((size_t)src->nverts*(src->authored_normals?8:5)*sizeof(float));
        dst->idx=(uint16_t *)malloc((size_t)src->nidx*sizeof(uint16_t));dst->vcol=NULL;
        if(!dst->verts || !dst->idx){n2_free_scene(&part);return 0;}
        memcpy(dst->verts,src->verts,(size_t)src->nverts*(src->authored_normals?8:5)*sizeof(float));
        memcpy(dst->idx,src->idx,(size_t)src->nidx*sizeof(uint16_t));
        n2_car_transform(dst,sockets[k],slot==N2_PART_EXHAUST);
    }
    n2_free_scene(&part);return 1;
}

static int n2_mod_assemble(const char *root,const unsigned char *data,long len,
                           const N2CarConfig *cfg,N2Scene *car) {
    for(int p=0;p<N2_PART_COUNT;p++) {
        int value=cfg->parts[p];if(!value)continue;
        if(value>=1000) {if(!n2_mod_attach(root,data,len,p,value,car))return 0;}
        else {
            if(value<1 || value>200)return 0;
            int found=0;
            for(int i=0;i<car->count;i++) {
                N2Mesh *m=car->meshes+i;
                if(m->car_part==p+1 && m->vkind==(value-1)/100+1 && m->vnum==(value-1)%100)found=1;
            }
            if(!found)return 0;
        }
    }
    return 1;
}
#endif
