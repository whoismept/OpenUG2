/* Independent native asset-driven HUD. All placement comes from package data. */
#include "hud.h"
#include "hud_asset.h"
#include "hud_texture.h"
#include "hud_baseline.h"
#include "hud_race.h"
#include "race.h"
#include "map_bake.h"

/* One bake on the worker. event>0 is a race map, 0 the free-roam map. */
typedef struct {
 SDL_Thread *thread; SDL_atomic_t done; Uint32 begin;
 const MbLayers *layers;
 unsigned char *src,*rgb; int w,h,event,size,active,cached;
 float *route; int route_n,route_loop;   /* the racing line, copied: owned here */
 char cache[1100];    /* empty: no cache. cached: read this; otherwise write it */
} MapJob;
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define PACK_COUNT 22
#define FONT_COUNT 64
typedef struct { const unsigned char *slots; uint32_t size; } Slots;
typedef struct { unsigned char *data; size_t len; N2Tpk tpk; Slots *slots; size_t slot_count; } Pack;
typedef struct { RhiTexture id; int w,h; } Texture;
/* atlas is the set 8 repacked glyph atlas (transparent one-texel gutter per
 * cell); cells[i] is glyph i's cell in it, inv_w/inv_h its texel size. With
 * cells NULL (repack failed) the original atlas and header sizes are used. */
typedef struct { HaFont metrics; Texture atlas; HaGlyphCell *cells; float inv_w,inv_h; } Font;
/* The HUD's own shader parameters (std140); samplers: art on unit 0, silhouette
 * on unit 1. Field names match the uniforms in init_shader's GLSL. */
typedef struct HudParams {
 float tint[4];      /* @0  */
 float dialClip[4];  /* @16 */
 float halfSize[2];  /* @32 */
 float roads;        /* @40 */
 int32_t art, silhouette;   /* @44, @48 (sampler units) */
} HudParams;
static const RhiParamField hud_param_fields[] = {
 {"tint",       RHI_PARAM_VEC4,  offsetof(HudParams, tint),       1},
 {"dialClip",   RHI_PARAM_VEC4,  offsetof(HudParams, dialClip),   1},
 {"halfSize",   RHI_PARAM_VEC2,  offsetof(HudParams, halfSize),   1},
 {"roads",      RHI_PARAM_FLOAT, offsetof(HudParams, roads),      1},
 {"art",        RHI_PARAM_INT,   offsetof(HudParams, art),        1},
 {"silhouette", RHI_PARAM_INT,   offsetof(HudParams, silhouette), 1},
};
static const RhiParamLayout hud_param_layout = {
 hud_param_fields, sizeof hud_param_fields / sizeof hud_param_fields[0],
 sizeof(HudParams), "HudParams"
};
struct Hud {
 HudBaseline *auxiliary;
 HaPackage package; Pack packs[PACK_COUNT]; Font fonts[FONT_COUNT]; size_t font_count;
 Texture *textures;
 RhiShader hud_shader; HudParams *hud_params;   /* the shader + its parameter block */
 RhiPipeline tri_pipe, line_pipe;               /* quads/markers; map route lines (width 2) */
 RhiBuffer vbo, ibo, line_ibo;                  /* streamed verts; quad + line-strip index sets */
 int speed,gear,needle,unit,mapnode,masknode,north,player,border,gauge_root;
 struct { int node,skin,scale; Texture texture; } skin_cache[256]; size_t skin_cache_count;
 HaMapFrame pick_frame; unsigned char *mask_alpha; int mask_width,mask_height;
 int layout_mode,layout_ready,race_nodes[HR_FIELD_COUNT];
 int drift_angle_left,drift_angle_right,drift_angle_compact;
 int drift_turbo_fill,drift_turbo_lines,drift_turbo_red[2];
 char *data_root;
 int race_map_event;
 Texture race_map;
 const N2Scene *bake_scene; const float *bake_nav; const int *bake_start,*bake_list;
 int bake_nnav,bake_size,map_sync,want_city,cache_off;
 MbLayers layers;
 MapJob job,pending,city_pending;  /* job runs on the worker; the others wait, src owned */
 int job_busy;
 Texture city_map;   /* baked, optional; replaces TRACKMAP4000 */
 double race_map_origin[3];
 double origin[3]; int track_ok,preview,skin; char missing[1024]; unsigned reported,resolved,drawn;
 /* Capture-amendment targets resolved by the C3-8 key rule, and settings. */
 HudSettings settings;
 int bank_value,bank_label,bank_backing,bank_ok;
 int widget_backing,widget_dpad,widget_highlight,widget_centre,widget_mail,widget_ok;
 int nos_backing,nos_alpha,shift_icon,glow_node,glow_active; double glow_start; HaShiftPhase shift_phase;
 Texture centre_texture;
 float text_offset[2];
 int tint_on; unsigned char tint_rgba[4]; float clip[4];
 /* Project widescreen policy: per node, the classification centre (logical x) of its
    top-level group, computed once at load; NaN = the group is never moved. */
 double *group_cx;
};
static void diagnostic(Hud*h,const char*s){if(!h)return;if(!strstr(h->missing,s)){size_t n=strlen(h->missing);if(n+strlen(s)+2<sizeof h->missing)snprintf(h->missing+n,sizeof h->missing-n,"%s%s",n?"; ":"",s);fprintf(stderr,"HUD: %s\n",s);}}
static unsigned char *readfile(const char*root,const char*relative,size_t*n){char path[4096];if(snprintf(path,sizeof path,"%s/%s",root,relative)>=(int)sizeof path)return NULL;FILE*f=fopen(path,"rb");if(!f)return NULL;if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long z=ftell(f);if(z<0||z>256*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}unsigned char*d=malloc(z?(size_t)z:1);if(!d){fclose(f);return NULL;}if(fread(d,1,(size_t)z,f)!=(size_t)z){free(d);fclose(f);return NULL;}fclose(f);*n=(size_t)z;return d;}
static RhiTexture upload(N2Tex*t){RhiTexture id=upload_tex(t);render_texture_clamp_linear(id);return id;}
static void catalog(Pack*p,const unsigned char*d,size_t n,unsigned depth){
 if(depth>HA_DEPTH)return;
 for(size_t at=0;at<n;){if(n-at<8)return;uint32_t type=n2_u32(d+at);size_t z=n2_u32(d+at+4);if(z>n-at-8)return;
 if(type==0x33310003u){if(z%24==0&&p->slot_count<HA_LIMIT){Slots*next=realloc(p->slots,(p->slot_count+1)*sizeof *next);if(next){p->slots=next;p->slots[p->slot_count++]=(Slots){d+at+8,(uint32_t)z};}}}
 else if((type&0x80000000u)&&type!=0xb3320000u)catalog(p,d+at+8,z,depth+1);
 at+=8+z;}
}
static int slot_present(const Slots*s,uint32_t key){for(size_t i=0;i<s->size;i+=24)if(n2_u32(s->slots+i)==key)return 1;return 0;}
static void retain_mask(Hud*h,uint32_t key,const N2Tex*t){if(key!=ha_texture_hash("Minimap_Mask.tga"))return;size_t n=(size_t)t->w*t->h;unsigned char*a=malloc(n);if(!a)return;if(t->alpha)memcpy(a,t->alpha,n);else memset(a,255,n);free(h->mask_alpha);h->mask_alpha=a;h->mask_width=t->w;h->mask_height=t->h;}
/* Decoded pixels for `key` from packs [first,last): 1 with t->rgb (and
 * t->alpha, NULL meaning opaque) owned by the caller. */
static int decode_key(Hud*h,uint32_t key,int first,int last,N2Tex*t){
 int present=0;
 for(int i=first;i<last;i++){Pack*p=h->packs+i;if(!p->data)continue;HaPixels pixels;
  int decoded=ha_texture_find(p->data,p->len,key,&pixels);
  if(decoded==1){memset(t,0,sizeof *t);t->w=(int)pixels.width;t->h=(int)pixels.height;size_t count=(size_t)t->w*t->h;t->rgb=malloc(count*3);t->alpha=malloc(count);if(t->rgb&&t->alpha){for(size_t j=0;j<count;j++){memcpy(t->rgb+j*3,pixels.rgba+j*4,3);t->alpha[j]=pixels.rgba[j*4+3];}free(pixels.rgba);return 1;}free(t->rgb);free(t->alpha);free(pixels.rgba);}
  else if(decoded<0){present=1;if(decoded==-2)continue;}
  for(size_t j=0;j<p->slot_count;j++)if(slot_present(p->slots+j,key)){
   present=1;
   if(n2_load_car_tex_from_slots(p->data,(long)p->len,p->slots[j].slots,p->slots[j].size,key,t)){free(t->dxt);t->dxt=NULL;return 1;}
  }
  if(n2_tpk_decode(p->data,(long)p->len,p->tpk,key,t)){free(t->dxt);t->dxt=NULL;return 1;}
 }
 if(present)diagnostic(h,"texture catalog entry present but decode unsupported or failed");
 return 0;
}
static Texture lookup(Hud*h,uint32_t key,int first,int last){
 Texture out={0};N2Tex t;if(!decode_key(h,key,first,last,&t))return out;
 out.id=upload(&t);out.w=t.w;out.h=t.h;if(out.id.id)retain_mask(h,key,&t);else out.w=out.h=0;free(t.rgb);free(t.alpha);return out;
}
/* Set 8 G6 item 2: the font atlas is repacked so every glyph cell has a
 * transparent one-texel gutter; sampled GL_LINEAR with GL_CLAMP_TO_EDGE, the
 * texels just outside a cell then read alpha 0 whatever the original atlas
 * neighbours or edge hold. On failure the original atlas is used as is. */
static void font_atlas(Hud*h,Font*f){
 N2Tex t;f->inv_w=f->metrics.inv_w;f->inv_h=f->metrics.inv_h;
 if(!decode_key(h,ha_hash(f->metrics.texture),0,7,&t)){diagnostic(h,"original font atlas missing");return;}
 size_t count=(size_t)t.w*t.h;unsigned char*rgba=malloc(count*4);HaGlyphAtlas a={0};
 if(rgba){for(size_t j=0;j<count;j++){memcpy(rgba+j*4,t.rgb+j*3,3);rgba[j*4+3]=t.alpha?t.alpha[j]:255;}}
 if(rgba&&ha_glyph_atlas(rgba,(unsigned)t.w,(unsigned)t.h,f->metrics.glyphs,f->metrics.count,&a)){
  N2Tex p;memset(&p,0,sizeof p);p.w=(int)a.width;p.h=(int)a.height;size_t n=(size_t)p.w*p.h;p.rgb=malloc(n*3);p.alpha=malloc(n);
  if(p.rgb&&p.alpha){for(size_t j=0;j<n;j++){memcpy(p.rgb+j*3,a.rgba+j*4,3);p.alpha[j]=a.rgba[j*4+3];}f->atlas.id=upload(&p);}
  if(f->atlas.id.id){f->atlas.w=p.w;f->atlas.h=p.h;f->cells=a.cells;a.cells=NULL;f->inv_w=1.0f/p.w;f->inv_h=1.0f/p.h;}
  free(p.rgb);free(p.alpha);ha_glyph_atlas_free(&a);
 }
 if(!f->atlas.id.id){diagnostic(h,"glyph atlas repack failed; sampling the original atlas");f->atlas.id=upload(&t);if(f->atlas.id.id){f->atlas.w=t.w;f->atlas.h=t.h;}else diagnostic(h,"original font atlas missing");}
 free(rgba);free(t.rgb);free(t.alpha);
}
static Texture named(Hud*h,const char*s,int first,int last){return lookup(h,ha_hash(s),first,last);}
static int equalfont(const char*a,const char*b){size_t n=strlen(a),m=strlen(b);if(n>4&&!strcmp(a+n-4,".ffn"))n-=4;if(m>4&&!strcmp(b+m-4,".ffn"))m-=4;if(n!=m)return 0;for(size_t i=0;i<n;i++){unsigned char x=a[i],y=b[i];if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return 0;}return 1;}
static void metadata(Hud*h,const unsigned char*d,size_t n,unsigned depth){if(depth>HA_DEPTH){diagnostic(h,"archive depth limit");return;}for(size_t at=0;at<n;){if(n-at<8){diagnostic(h,"truncated archive");return;}uint32_t type=n2_u32(d+at);size_t z=n2_u32(d+at+4);if(z>n-at-8){diagnostic(h,"archive extent exceeds container");return;}const unsigned char*b=d+at+8;if(type==0x30201&&h->font_count<FONT_COUNT){Font*f=h->fonts+h->font_count;if(ha_font(b,z,&f->metrics)){font_atlas(h,f);h->font_count++;}else diagnostic(h,"unsupported font header; fallback required");}else if(type&0x80000000u)metadata(h,b,z,depth+1);at+=8+z;}}
/* Each TRACKMAP uses the origin and square world extent in its TrackInfo row. */
static int track_map_origin(const Pack *global, int event, double origin[3]) {
 if(!global->data)return 0;
 N2Leaf leaves[8];int count=0;
 n2_find_leaves(global->data,0,(long)global->len,0x34201u,leaves,&count,8);
 for(int i=0;i<count;i++){
  if(leaves[i].size%296)continue;
  const unsigned char *data=global->data+leaves[i].off;
  for(size_t at=0;at<leaves[i].size;at+=296){
   if(n2_u32(data+at+140)!=(uint32_t)event)continue;
   for(int k=0;k<3;k++){
    uint32_t bits=n2_u32(data+at+172+4*k);float value;
    memcpy(&value,&bits,sizeof value);origin[k]=value;
   }
   return isfinite(origin[0])&&isfinite(origin[1])&&isfinite(origin[2])&&origin[2]>0;
  }
 }
 return 0;
}

static Texture upload_baked(unsigned char *rgb,int size){
 N2Tex baked;memset(&baked,0,sizeof baked);
 baked.w=size;baked.h=size;baked.rgb=rgb;
 RhiTexture id=upload_tex(&baked);
 free(rgb);
 if(!id.id)return (Texture){0};
 render_texture_clamp_edges(id);
 return (Texture){id,size,size};
}

static int map_job_run(void*data){
 MapJob*job=data;
 if(job->cached)job->rgb=map_cache_read(job->cache,job->size);
 else{
  job->rgb=map_bake_from_layers(job->layers,job->src,job->w,job->h,job->event>0,job->route,job->route_n,job->route_loop);
  if(job->rgb&&job->cache[0])map_cache_write(job->cache,job->rgb,job->size);
 }
 SDL_AtomicSet(&job->done,1);
 return 0;
}

static void map_job_start(Hud*h,const MapJob*request){
 h->job=*request;
 h->job.layers=&h->layers;h->job.rgb=NULL;h->job.begin=SDL_GetTicks();
 SDL_AtomicSet(&h->job.done,0);
 h->job.thread=SDL_CreateThread(map_job_run,"map-bake",&h->job);
 h->job_busy=1;
 if(!h->job.thread)map_job_run(&h->job);   /* no thread available: still correct, just blocking */
}

/* The texture upload stays here: GL belongs to the main thread. */
static void map_job_finish(Hud*h){
 MapJob*job=&h->job;
 if(job->thread)SDL_WaitThread(job->thread,NULL);
 free(job->src);free(job->route);
 Uint32 worked=SDL_GetTicks()-job->begin,start=SDL_GetTicks();
 const char*how=job->cached?"loaded":"baked";
 if(job->cached&&!job->rgb){
  /* Damaged: drop it so the next run bakes afresh. Retail image for this session. */
  remove(job->cache);
  diagnostic(h,"map cache file unreadable; removed");
 }
 int keep=job->rgb&&(job->event?job->event==h->race_map_event:h->want_city);
 Texture t=keep?upload_baked(job->rgb,job->size):(Texture){0};
 if(!keep)free(job->rgb);
 if(t.id.id&&job->event){
  rhi_texture_destroy(h->race_map.id);h->race_map=t;
  printf("race map: event %d %dx%d %s in %u ms, upload %u ms\n",job->event,t.w,t.h,how,worked,SDL_GetTicks()-start);
 }else if(t.id.id){
  h->city_map=t;h->want_city=0;
  printf("city map: %dx%d %s in %u ms, upload %u ms\n",t.w,t.h,how,worked,SDL_GetTicks()-start);
 }
 h->job_busy=0;
}

static void map_job_drop(MapJob*job){free(job->src);free(job->route);memset(job,0,sizeof *job);}

/* Finish a done job, then start whatever waits. A race goes first; the free-roam
 * map keeps waiting behind it. */
static void map_jobs_pump(Hud*h,int wait){
 for(;;){
  if(h->job_busy&&(wait||SDL_AtomicGet(&h->job.done)))map_job_finish(h);
  if(h->job_busy)return;
  if(h->pending.active){map_job_start(h,&h->pending);memset(&h->pending,0,sizeof h->pending);}
  else if(h->city_pending.active){map_job_start(h,&h->city_pending);memset(&h->city_pending,0,sizeof h->city_pending);}
  if(!h->job_busy||!wait)return;
 }
}

/* Fill in where this map lives. Returns 1 when a finished copy is there. */
static int map_job_locate(Hud*h,MapJob*job,const char*name,const HaPixels*pixels){
 uint32_t hash=map_cache_hash(pixels->rgba,(size_t)pixels->width*pixels->height*4);
 job->size=h->bake_size;job->active=1;
 if(h->cache_off||!map_cache_path(job->cache,sizeof job->cache,h->data_root,name,job->size,hash)){
  job->cache[0]=0;return 0;
 }
 job->cached=map_cache_exists(job->cache);
 return job->cached;
}

static int map_layers_ready(Hud*h,const double frame[3]){
 if(map_bake_layers_match(&h->layers,frame,h->bake_size))return 1;
 if(h->job_busy)map_job_finish(h);   /* the worker reads these layers */
 map_bake_layers_free(&h->layers);
 Uint32 begin=SDL_GetTicks();
 if(!map_bake_layers(&h->layers,h->bake_scene,h->bake_nav,h->bake_nnav,h->bake_start,h->bake_list,frame,h->bake_size))return 0;
 printf("map layers: %dx%d in %u ms\n",h->bake_size,h->bake_size,SDL_GetTicks()-begin);
 return 1;
}

/* Takes pixels when it returns 1. City-wide race maps spend about 10 m on a
 * texel; the small ones are already as sharp as a baked map. */
static int queue_race_bake(Hud*h,int event,const char*name,HaPixels*pixels,const HudState*s){
 if(h->bake_size<=0||!h->bake_scene||h->race_map_origin[2]<MB_RACE_MIN_EXTENT)return 0;
 MapJob job;memset(&job,0,sizeof job);
 job.event=event;
 if(map_job_locate(h,&job,name,pixels)){map_job_drop(&h->pending);h->pending=job;return 0;}
 if(!map_layers_ready(h,h->race_map_origin))return 0;
 job.src=pixels->rgba;job.w=(int)pixels->width;job.h=(int)pixels->height;
 if(s->route_xy&&s->route_n>1){
  size_t bytes=(size_t)s->route_n*2*sizeof(float);
  job.route=malloc(bytes);
  if(job.route){memcpy(job.route,s->route_xy,bytes);job.route_n=s->route_n;job.route_loop=s->route_loop;}
 }
 map_job_drop(&h->pending);
 h->pending=job;
 return 1;
}

static void load_race_map(Hud *h, int event, const HudState *s) {
 if(h->race_map_event==event)return;
 rhi_texture_destroy(h->race_map.id);
 h->race_map=(Texture){0};
 h->race_map_event=event;
 if(event<=0 || !h->data_root)return;

 char name[32],path[64];
 snprintf(name,sizeof name,"TRACKMAP%d",event);
 snprintf(path,sizeof path,"TRACKS/%s.BIN",name);
 size_t len=0;
 unsigned char *data=readfile(h->data_root,path,&len);
 HaPixels pixels;
 if(data && track_map_origin(&h->packs[5],event,h->race_map_origin) &&
    ha_texture_find(data,len,ha_hash(name),&pixels)==1){
  RhiTexture id=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,
     RHI_FORMAT_RGBA8,(int)pixels.width,(int)pixels.height,RHI_TEXTURE_SAMPLED});
  if(id.id && rhi_texture_upload(id,0,0,pixels.rgba,0)==RHI_OK){
   render_texture_clamp_linear(id);
   h->race_map=(Texture){id,(int)pixels.width,(int)pixels.height};
  }else rhi_texture_destroy(id);
  if(!queue_race_bake(h,event,name,&pixels,s))free(pixels.rgba);
 }
 free(data);
 if(!h->race_map.id.id){
  char message[96];
  snprintf(message,sizeof message,"%s unavailable; using route overlay",name);
  diagnostic(h,message);
 }
}

static int node_named(Hud*h,const char*s){uint32_t key=ha_hash(s);for(size_t i=0;i<h->package.count;i++)if(h->package.nodes[i].valid&&h->package.nodes[i].name==key)return (int)i;return -1;}
static int node_key(Hud*h,uint32_t key,uint32_t kind){
 int found=-1;for(size_t i=0;i<h->package.count;i++)if(h->package.nodes[i].name==key){
  if(found>=0)return -1;found=(int)i;}
 return found>=0&&h->package.nodes[found].valid&&h->package.nodes[found].kind==kind?found:-1;
}
static int descendant(Hud*h,int i,int root){for(unsigned d=0;i>=0&&d<HA_DEPTH;d++,i=h->package.nodes[i].parent)if(i==root)return 1;return 0;}
static const char *resource_name(Hud*h,HaNode*n){return n->resource<h->package.resource_count?h->package.resources[n->resource].name:NULL;}
static int node_resource(Hud*h,const char*s){for(size_t i=0;i<h->package.count;i++){const char*r=resource_name(h,h->package.nodes+i);if(r&&!strcmp(r,s))return (int)i;}return -1;}
static int skin_resource(const char*r);
static int race_image(Hud*h,int i){
 const char*r=resource_name(h,h->package.nodes+i);if(!r)return 0;
 return !strcmp(r,"3rdperson_BoxCorner.tga")||(!strcmp(r,"3rdperson_BoxTime.tga")&&h->layout_mode!=3)||
   !strncmp(r,"Drag_Position_Backing_",22);
}
static int drag_gauge(Hud*h,int i){
 const char*r=resource_name(h,h->package.nodes+i);
 return h->layout_mode==2&&descendant(h,i,h->gauge_root)&&r&&!strncmp(r,"Drag_RPM_",9);
}
/* Project policy: widescreen HUD fitment (not original behaviour). Logical translation of the
 * top-level group containing node i under the selected policy at this viewport. */
static double fit_shift(const Hud*h,int i,int w,int ht){
 if(!h->group_cx||i<0||(size_t)i>=h->package.count)return 0;
 return ha_fit_translation(h->settings.fit_policy,w,ht,h->group_cx[i]);
}
/* Static eligibility for classification: geometry the standard path can draw (gauge, map, amendment
 * elements, readout text). Authoring sample items and event widgets are excluded. */
static int fit_eligible(Hud*h,int i){
 const HaNode*n=h->package.nodes+i;if(!n->valid||!n->appearance)return 0;
 if(n->kind==HA_KIND_TEXT){for(int k=0;k<HR_FIELD_COUNT;k++)if(i==h->race_nodes[k])return 1;return i==h->speed||i==h->gear||i==h->unit||i==h->bank_label||i==h->bank_value;}
 if(n->kind!=HA_KIND_IMAGE)return 0;
 if(race_image(h,i)||drag_gauge(h,i))return 1;
 if(i==h->drift_angle_left||i==h->drift_angle_right||i==h->drift_angle_compact||
    i==h->drift_turbo_fill||i==h->drift_turbo_lines||
    i==h->drift_turbo_red[0]||i==h->drift_turbo_red[1])return 1;
 const char*r=resource_name(h,(HaNode*)n);
 if(i==h->needle||i==h->mapnode||i==h->masknode||i==h->north||i==h->player||descendant(h,i,h->border))return 1;
 if(i==h->bank_backing||i==h->widget_backing||i==h->widget_dpad||i==h->widget_highlight||i==h->widget_centre||i==h->widget_mail||i==h->nos_backing||i==h->nos_alpha||i==h->shift_icon||i==h->glow_node)return 1;
 return descendant(h,i,h->gauge_root)&&r&&(skin_resource(r)||!strcmp(r,"3rdperson_rpmdial.tga")||!strcmp(r,"3rdperson_redline.tga"));
}
static void fit_classify(Hud*h){
 size_t count=h->package.count;if(!count)return;
 h->group_cx=malloc(count*sizeof *h->group_cx);double *lo=malloc(count*sizeof *lo),*hi=malloc(count*sizeof *hi);int *top=malloc(count*sizeof *top);
 if(!h->group_cx||!lo||!hi||!top){free(h->group_cx);h->group_cx=NULL;free(lo);free(hi);free(top);diagnostic(h,"widescreen group classification unavailable; no group moved");return;}
 for(size_t i=0;i<count;i++){h->group_cx[i]=NAN;lo[i]=INFINITY;hi[i]=-INFINITY;int a=(int)i;unsigned d=0;while(h->package.nodes[a].parent>=0&&d++<HA_DEPTH)a=h->package.nodes[a].parent;top[i]=a;}
 for(size_t i=0;i<count;i++){
  if(!fit_eligible(h,(int)i))continue;
  const HaNode*n=h->package.nodes+i;float c[4][3],z[3]={0};int pts=0;
  if(n->kind==HA_KIND_IMAGE){float hw=n->size[0]/2,hh=n->size[1]/2;float l[4][3]={{-hw,-hh,0},{hw,-hh,0},{hw,hh,0},{-hw,hh,0}};for(int j=0;j<4;j++)ha_point(&h->package,i,l[j],c[j],0);pts=4;}
  else{ha_point(&h->package,i,z,c[0],0);pts=1;}   /* text: its anchor point; extent depends on the live string */
  for(int j=0;j<pts;j++){if(!isfinite(c[j][0]))continue;int t=top[i];if(c[j][0]<lo[t])lo[t]=c[j][0];if(c[j][0]>hi[t])hi[t]=c[j][0];}
 }
 for(size_t i=0;i<count;i++){int t=top[i];h->group_cx[i]=lo[t]<=hi[t]?(lo[t]+hi[t])/2:NAN;}

 free(lo);free(hi);free(top);
}
static void init_shader(Hud*h){
#ifdef N2_GLES
 const char*prefix="precision mediump float;\n";
#else
 const char*prefix="#version 120\n";
#endif
 const char*vertex="attribute vec2 position;attribute vec2 sampleUV;attribute vec2 clipUV;varying vec2 uv;varying vec2 muv;varying vec2 ndc;void main(){uv=sampleUV;muv=clipUV;ndc=position;gl_Position=vec4(position,0.0,1.0);}";
 /* dialClip: centre (NDC), limit (radians), mode 0 off / 1 keep <= / 2 keep >=,
    with dial angle 0 straight down increasing screen-clockwise (C1-4). */
 const char*fragment="varying vec2 uv;varying vec2 muv;varying vec2 ndc;uniform sampler2D art;uniform sampler2D silhouette;uniform vec4 tint;uniform float roads;uniform vec4 dialClip;uniform vec2 halfSize;void main(){if(dialClip.w>0.5){vec2 d=(ndc-dialClip.xy)*halfSize;float a=atan(-d.x,-d.y);if(a<0.0)a+=6.28318531;if(dialClip.w<1.5){if(a>dialClip.z)discard;}else if(a<dialClip.z)discard;}if(roads>1.5&&(muv.x<0.0||muv.x>1.0||muv.y<0.0||muv.y>1.0))discard;vec4 c=roads>1.5?vec4(1.0):texture2D(art,uv);if(roads>0.5)c.a=texture2D(silhouette,muv).a;gl_FragColor=c*tint;}";
 char vs[1024],fs[2048];snprintf(vs,sizeof vs,"%s%s",prefix,vertex);snprintf(fs,sizeof fs,"%s%s",prefix,fragment);
 static const char*const attributes[]={"position","sampleUV","clipUV"};
 static const RhiParamLayout*const blocks[]={&hud_param_layout};
 h->hud_shader=rhi_shader_create(&(RhiShaderDesc){vs,fs,attributes,3,"hud",blocks,1});
 h->hud_params=(HudParams*)rhi_shader_params(h->hud_shader,0);
 if(!h->hud_params){diagnostic(h,"HUD shader unavailable");return;}
 h->hud_params->art=0;h->hud_params->silhouette=1;
 /* Translucent 2D: no depth, source-over alpha, no culling. Lines drawn at
    width 2 (the map route); triangles ignore line width. */
 RhiPipelineDesc pd;rhi_pipeline_desc_init(&pd);pd.shader=h->hud_shader;
 pd.vertex.attribs[0]=(RhiVertexAttrib){0,0,0,RHI_VERTEX_FLOAT2};
 pd.vertex.attribs[1]=(RhiVertexAttrib){1,0,8,RHI_VERTEX_FLOAT2};
 pd.vertex.attribs[2]=(RhiVertexAttrib){2,0,16,RHI_VERTEX_FLOAT2};
 pd.vertex.attrib_count=3;pd.vertex.stride[0]=24;
 pd.depth_test=0;pd.depth_write=0;pd.cull=RHI_CULL_NONE;
 pd.blend=1;pd.src_rgb=pd.src_alpha=RHI_BLEND_SRC_ALPHA;pd.dst_rgb=pd.dst_alpha=RHI_BLEND_ONE_MINUS_SRC_ALPHA;
 pd.primitive=RHI_PRIMITIVE_TRIANGLES;pd.line_width=1.0f;h->tri_pipe=rhi_pipeline_create(&pd);
 pd.primitive=RHI_PRIMITIVE_LINES;pd.line_width=2.0f;h->line_pipe=rhi_pipeline_create(&pd);
 /* Vertices are streamed per draw; index sets are constant. A quad is two
    triangles [0,1,2,0,2,3] (TRIANGLE_FAN of 4). A line strip of n points is
    n-1 segments [0,1,1,2,...]: one index pair per segment over the streamed
    points, so the vertices need no duplicating. */
 h->vbo=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_VERTEX,RHI_UPDATE_STREAM,(size_t)(HA_LIMIT+1)*6*sizeof(float),NULL});
 static const uint16_t quad_idx[6]={0,1,2,0,2,3};
 h->ibo=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_INDEX,RHI_UPDATE_STATIC,sizeof quad_idx,quad_idx});
 uint16_t*strip=malloc((size_t)HA_LIMIT*2*sizeof *strip);
 if(strip){for(int i=0;i<HA_LIMIT;i++){strip[2*i]=(uint16_t)i;strip[2*i+1]=(uint16_t)(i+1);}
  h->line_ibo=rhi_buffer_create(&(RhiBufferDesc){RHI_BUFFER_INDEX,RHI_UPDATE_STATIC,(size_t)HA_LIMIT*2*sizeof *strip,strip});free(strip);}
 if(!h->tri_pipe.id||!h->line_pipe.id||!h->vbo.id||!h->ibo.id||!h->line_ibo.id)diagnostic(h,"HUD pipeline setup failed");}
/* C3-8 lookup keys (whitelisted capture set 3 data keys). Stored name ids are
 * opaque data values; lookup names use the case-folded name hash. */
static int amend_key(Hud*h,uint32_t name,uint32_t kind,int root,uint32_t parent,const char*alias){HaKey k={name,kind,root,parent,alias};return ha_lookup(&h->package,&k);}
static void resolve_amendment_targets(Hud*h){
 const uint32_t bank=0x3F8644CAu,backing=0x30D59B88u,widget=0x3345911Du,tach=0x5164D4EAu;
 int bank_group=amend_key(h,bank,HA_KIND_GROUP,1,0,NULL),backing_group=amend_key(h,backing,HA_KIND_GROUP,1,0,NULL);
 h->bank_value=amend_key(h,0x1930B057u,HA_KIND_TEXT,0,bank,NULL);
 h->bank_label=amend_key(h,0x31B0D166u,HA_KIND_TEXT,0,bank,NULL);
 h->bank_backing=amend_key(h,ha_hash("Show_case_backing"),HA_KIND_IMAGE,0,backing,"Show_case_backing.tga");
 h->bank_ok=bank_group>=0&&backing_group>=0&&h->bank_value>=0&&h->bank_label>=0&&h->bank_backing>=0;
 int widget_group=amend_key(h,widget,HA_KIND_GROUP,1,0,NULL);
 h->widget_backing=amend_key(h,ha_hash("Engage_Backing"),HA_KIND_IMAGE,0,widget,"Engage_Backing.tga");
 h->widget_dpad=amend_key(h,ha_hash("Engage_DPAD_Backing"),HA_KIND_IMAGE,0,widget,"Engage_DPAD_Backing.tga");
 h->widget_highlight=amend_key(h,0xA206A0B4u,HA_KIND_IMAGE,0,widget,"Engage_DPAD_Highlight.tga");
 h->widget_centre=amend_key(h,0x50910822u,HA_KIND_IMAGE,0,widget,NULL);
 h->widget_mail=amend_key(h,ha_hash("Engage_SMS_mail"),HA_KIND_IMAGE,1,0,"Engage_SMS_mail.tga");
 h->widget_ok=widget_group>=0&&h->widget_backing>=0&&h->widget_dpad>=0&&h->widget_highlight>=0&&h->widget_centre>=0&&h->widget_mail>=0;
 int tach_group=amend_key(h,tach,HA_KIND_GROUP,1,0,NULL);
 h->nos_backing=tach_group<0?-1:amend_key(h,0x11461255u,HA_KIND_IMAGE,0,tach,"Tach_NOS_Backing.tga");
 h->nos_alpha=tach_group<0?-1:amend_key(h,0x11495CD6u,HA_KIND_IMAGE,0,tach,"Tach_NOS_Alpha.tga");
 h->shift_icon=tach_group<0?-1:amend_key(h,0x25CF2F74u,HA_KIND_IMAGE,0,tach,"Shift_Up_Icon.tga");
 /* E2: N2O glow label, a root-level image keyed by its stem hash. */
 h->glow_node=amend_key(h,0xFE2467E3u,HA_KIND_IMAGE,1,0,"Tach_NOS_Gamebreaker.tga");
 /* Centre figure: catalog texture id from the free-roam texture pack only;
    never the slot's referenced image or a generated imitation. */
 if(!h->centre_texture.id.id)h->centre_texture=lookup(h,0xEA570722u,0,1);
}
void hud_default_settings(HudSettings*o){if(!o)return;memset(o,0,sizeof *o);o->gear_colour[0]=255;o->gear_colour[1]=128;o->gear_colour[2]=64;o->highlight_colour[0]=255;o->highlight_colour[1]=190;o->highlight_colour[2]=50;o->highlight_colour[3]=255;ha_shift_default_colour(o->shift_colour);o->shift_indicator=1;o->shift_flash=1;o->shift_icon_automatic=1;o->nos_a0_degrees=91;o->nos_a1_degrees=239;o->gear_standstill_mps=HA_STANDSTILL_MPS;o->glow_ms_per_unit=HA_GLOW_MS_PER_UNIT;o->fit_policy=HA_FIT_WIDE;}
void hud_configure(Hud*h,const HudSettings*s){if(h&&s){h->settings=*s;if(h->settings.fit_policy!=HA_FIT_43)h->settings.fit_policy=HA_FIT_WIDE;hud_baseline_set_fit_policy(h->auxiliary,h->settings.fit_policy);}}
int hud_nos_glow_event(Hud*h,double now_ms){
 if(!h||!ha_glow_trigger(h->settings.nos_glow,now_ms,h->glow_active,h->glow_start,h->settings.glow_ms_per_unit))return 0;
 h->glow_active=1;h->glow_start=now_ms;return 1;
}
static void release_layout(Hud*h){
 for(size_t c=0;c<h->skin_cache_count;c++)rhi_texture_destroy(h->skin_cache[c].texture.id);
 for(size_t i=0;i<h->package.resource_count;i++)if(h->textures&&h->textures[i].id.id)rhi_texture_destroy(h->textures[i].id);
 free(h->textures);h->textures=NULL;free(h->mask_alpha);h->mask_alpha=NULL;h->mask_width=h->mask_height=0;
 free(h->group_cx);h->group_cx=NULL;ha_free(&h->package);h->skin_cache_count=0;
 h->speed=h->gear=h->needle=h->unit=h->mapnode=h->masknode=h->north=h->player=h->border=h->gauge_root=-1;
 h->drift_angle_left=h->drift_angle_right=h->drift_angle_compact=-1;
 h->drift_turbo_fill=h->drift_turbo_lines=h->drift_turbo_red[0]=h->drift_turbo_red[1]=-1;
 for(int k=0;k<HR_FIELD_COUNT;k++)h->race_nodes[k]=-1;
 h->shift_phase=(HaShiftPhase){0};h->glow_active=0;h->layout_ready=0;
}
static int layout_mode(const HudState*s){
 if(!s->racing)return 0;
 return s->race_kind==N2_RACE_DRAG?2:s->race_kind==N2_RACE_DRIFT?3:s->race_kind==N2_RACE_STREETX?4:1;
}
static int load_layout(Hud*h,int mode){
 if(h->layout_mode==mode)return h->layout_ready;
 int pack=mode==2?20:mode==3?21:mode?19:0;
 int kind=mode==2?N2_RACE_DRAG:mode==3?N2_RACE_DRIFT:mode==4?N2_RACE_STREETX:N2_RACE_CIRCUIT;
 HaPackage next={0};Pack*p=h->packs+pack;
 int ok=p->data&&hud_race_package_read(p->data,p->len,hud_race_package(kind),&next);
 Texture*textures=ok?calloc(next.resource_count?next.resource_count:1,sizeof *textures):NULL;
 release_layout(h);h->layout_mode=mode;
 if(!textures){ha_free(&next);diagnostic(h,"requested driving HUD package unavailable");return 0;}
 h->package=next;h->textures=textures;
 if(h->package.diagnostics)diagnostic(h,"rejected package objects or references");
 for(size_t i=0;i<h->package.resource_count;i++){HaResource*r=h->package.resources+i;
  if(r->kind!=1||!r->name)continue;uint32_t key=ha_texture_hash(r->name);
  h->textures[i]=lookup(h,key,pack,pack+1);
  if(!h->textures[i].id.id)h->textures[i]=lookup(h,key,0,7);
  if(!h->textures[i].id.id)h->textures[i]=lookup(h,key,18,19);
  if(!h->textures[i].id.id)diagnostic(h,"unresolved package image");
 }
 h->speed=node_named(h,"3rdPersonSpeed");h->gear=node_named(h,"3rdPersonGear");h->needle=node_named(h,"3rdPersonNeedle");
 h->unit=node_key(h,0xc3383b63,HA_KIND_TEXT);
 h->gauge_root=node_key(h,0x5164d4ea,HA_KIND_GROUP);
 h->border=node_named(h,"TrackMapTargetRing");h->player=node_named(h,"PLAYERCARINDICATOR_1");
 h->mapnode=node_resource(h,"Track4000_map.tga");h->masknode=node_resource(h,"Minimap_Mask.tga");h->north=node_resource(h,"Minimap_North_indicator.tga");
 if(mode!=2&&h->needle>=0&&!ha_needle_calibrate(h->package.nodes+h->needle)){
  h->package.nodes[h->needle].appearance=0;diagnostic(h,"unsupported needle quaternion decomposition; needle omitted");}
 resolve_amendment_targets(h);
 /* Drag's NOS targets belong to a separate group. */
 if(mode==2){h->shift_icon=node_key(h,0x25cf2f74,HA_KIND_IMAGE);h->nos_backing=node_resource(h,"Drag_NOS_Backing.tga");h->nos_alpha=node_key(h,0x11495cd6,HA_KIND_IMAGE);}
 if(mode==3){
  h->drift_angle_left=node_resource(h,"Drift_Angle_Backing1.tga");
  h->drift_angle_right=node_resource(h,"Drift_Angle_Backing2.tga");
  h->drift_angle_compact=node_resource(h,"Drift_Angle_Backing3.tga");
  h->drift_turbo_fill=node_resource(h,"3rdperson_Turbofill.tga");
  h->drift_turbo_lines=node_resource(h,"3rdperson_Turbolines.tga");
  h->drift_turbo_red[0]=node_key(h,0x1a33cbb5,HA_KIND_IMAGE);
  h->drift_turbo_red[1]=node_key(h,0x1a33cbb6,HA_KIND_IMAGE);
 }
 for(int k=0;k<HR_FIELD_COUNT;k++)h->race_nodes[k]=mode?node_key(h,hud_race_field_key(kind,k),HA_KIND_TEXT):-1;
 fit_classify(h);h->layout_ready=1;return 1;
}
Hud *hud_init(const char*root){
 Hud*h=calloc(1,sizeof *h);if(!h)return NULL;hud_default_settings(&h->settings);h->layout_mode=-1;
 if(!root)root=".";h->auxiliary=hud_baseline_init(root);
 h->data_root=strdup(root);
 const char*paths[]={"GLOBAL/InGameFreeRoam.bun","LANGUAGES/English.bin","LANGUAGES/LanguageTextures.bin","GLOBAL/GLOBALA.BUN","GLOBAL/HUD_CustomTextures_ALL.bin","GLOBAL/GLOBALB.BUN","TRACKS/TRACKMAP4000.BIN"};
 const char*events[]={"GLOBAL/InGameCommon.bun","GLOBAL/InGameRace.bun","GLOBAL/InGameDrag.bun","GLOBAL/InGameDrift.bun"};
 for(int i=0;i<PACK_COUNT;i++){char skin[128];const char*path;
  if(i<7)path=paths[i];else if(i>=18)path=events[i-18];else{snprintf(skin,sizeof skin,"GLOBAL/HUD_CustomTextures_%02d.bin",i-7);path=skin;}
  h->packs[i].data=readfile(root,path,&h->packs[i].len);
  if(h->packs[i].data){h->packs[i].tpk=n2_tpk_open(h->packs[i].data,(long)h->packs[i].len);catalog(h->packs+i,h->packs[i].data,h->packs[i].len,0);}else diagnostic(h,path);
 }
 for(int i=0;i<7;i++)if(h->packs[i].data)metadata(h,h->packs[i].data,h->packs[i].len,0);
 h->track_ok=track_map_origin(&h->packs[5],4000,h->origin);
 if(!h->track_ok)diagnostic(h,"city track metadata missing or invalid");
 load_layout(h,0);init_shader(h);return h;
}
void hud_set_map_source(Hud*h,const N2Scene*scene,const float*nav,int nnav,const int*start,const int*list,int size){
 if(!h)return;
 h->want_city=0;map_job_drop(&h->pending);map_job_drop(&h->city_pending);
 if(h->job_busy)map_job_finish(h);
 map_bake_layers_free(&h->layers);
 if(h->city_map.id.id)rhi_texture_destroy(h->city_map.id);
 h->city_map=(Texture){0};
 h->bake_scene=scene;h->bake_nav=nav;h->bake_nnav=nnav;
 h->bake_start=start;h->bake_list=list;h->bake_size=size;
 h->race_map_event=-1;   /* a live race picks the new size up on its next frame */
}
void hud_request_city_map(Hud*h){
 if(!h||h->bake_size<=0||!h->bake_scene||!h->track_ok)return;
 size_t len=0;
 unsigned char*data=readfile(h->data_root,"TRACKS/TRACKMAP4000.BIN",&len);
 HaPixels pixels;
 if(!data||ha_texture_find(data,len,ha_hash("TRACKMAP4000"),&pixels)!=1){
  free(data);diagnostic(h,"TRACKMAP4000 unavailable; retail map kept");return;
 }
 free(data);
 MapJob job;memset(&job,0,sizeof job);
 if(map_job_locate(h,&job,"CITY",&pixels))free(pixels.rgba);
 else if(map_layers_ready(h,h->origin)){job.src=pixels.rgba;job.w=(int)pixels.width;job.h=(int)pixels.height;}
 else{free(pixels.rgba);return;}
 map_job_drop(&h->city_pending);
 h->city_pending=job;h->want_city=1;
}
void hud_set_map_sync(Hud*h,int on){if(h)h->map_sync=on;}
void hud_set_map_cache(Hud*h,int on){if(h)h->cache_off=!on;}
void hud_free(Hud**hp){
 if(!hp||!*hp)return;Hud*h=*hp;
 h->want_city=0;map_job_drop(&h->pending);map_job_drop(&h->city_pending);
 if(h->job_busy)map_job_finish(h);
 map_bake_layers_free(&h->layers);
 release_layout(h);
 rhi_texture_destroy(h->race_map.id);rhi_texture_destroy(h->city_map.id);free(h->data_root);
 if(h->centre_texture.id.id)rhi_texture_destroy(h->centre_texture.id);hud_baseline_free(&h->auxiliary);
 for(size_t i=0;i<h->font_count;i++){free(h->fonts[i].metrics.glyphs);free(h->fonts[i].cells);if(h->fonts[i].atlas.id.id)rhi_texture_destroy(h->fonts[i].atlas.id);}
 for(int i=0;i<PACK_COUNT;i++){free(h->packs[i].slots);free(h->packs[i].tpk.blk);free(h->packs[i].data);}
 rhi_shader_destroy(h->hud_shader);rhi_buffer_destroy(h->vbo);rhi_buffer_destroy(h->ibo);rhi_buffer_destroy(h->line_ibo);free(h);*hp=NULL;
}
void hud_set_preview(Hud*h,int on){if(h){h->preview=!!on;hud_baseline_set_preview(h->auxiliary,on);}}
int hud_preview(const Hud*h){return h&&h->preview;}
const char *hud_missing(const Hud*h){return h?h->missing:"HUD allocation failed";}
static Texture node_texture(Hud*h,int i){Texture t={0};if(i<0)return t;HaNode*n=h->package.nodes+i;if(n->resource<h->package.resource_count)t=h->textures[n->resource];return t;}
static int skin_resource(const char*r){
 if(!r)return 0;if(!strcmp(r,"Tach_Fill.tga"))return 1;
 const char*prefix="3rdperson_";if(strncmp(r,prefix,strlen(prefix)))return 0;r+=strlen(prefix);
 if(*r<'0'||*r>'9')return 0;while(*r>='0'&&*r<='9')r++;return !strcmp(r,"lines.tga");
}
static Texture image_texture(Hud*h,int i,int skin,int scale,int*owned){
 *owned=0;
 const char *resource=resource_name(h,h->package.nodes+i);
 const char *family=NULL;
 char rpm_lines[32];
 if(i==h->drift_turbo_fill)family="3RDPERSON_TURBOFILL";
 else if(i==h->drift_turbo_lines)family="3RDPERSON_TURBOLINES";
 else if(h->layout_mode!=2&&descendant(h,i,h->gauge_root)&&(i==h->needle||skin_resource(resource))){
  if(i==h->needle)family="3RDPERSON_RPMDIAL";
  else if(!strcmp(resource,"Tach_Fill.tga"))family="TACH_FILL";
  else{
   snprintf(rpm_lines,sizeof rpm_lines,"3RDPERSON_%dLINES",scale);
   family=rpm_lines;
  }
 }
 if(!family)return node_texture(h,i);
 if(family!=rpm_lines)scale=0;
 for(size_t c=0;c<h->skin_cache_count;c++)
  if(h->skin_cache[c].node==i&&h->skin_cache[c].skin==skin&&h->skin_cache[c].scale==scale)
   return h->skin_cache[c].texture;

 char key[64];
 snprintf(key,sizeof key,"%s_CUSTOM_%02d",family,skin);
 Texture t=named(h,key,4,5);
 if(!t.id.id)t=named(h,key,7+skin,8+skin);
 if(!t.id.id&&skin){
  diagnostic(h,"skin resource fallback to 00");
  snprintf(key,sizeof key,"%s_CUSTOM_00",family);
  t=named(h,key,4,5);
  if(!t.id.id)t=named(h,key,7,8);
 }
 if(!t.id.id){
  diagnostic(h,"skin resource fallback to authored image");
  return node_texture(h,i);
 }
 if(h->skin_cache_count<sizeof h->skin_cache/sizeof h->skin_cache[0]){
  size_t c=h->skin_cache_count++;
  h->skin_cache[c].node=i;h->skin_cache[c].skin=skin;
  h->skin_cache[c].scale=scale;h->skin_cache[c].texture=t;
 }else *owned=1;
 return t;
}
/* Per-feature acceptance bits: needle/dial/scale/fill/font/road/mask/north/player/border. */
static unsigned feature(Hud*h,int i){const char*r=resource_name(h,h->package.nodes+i);if(i==h->needle)return 1u<<0;if(i==h->bank_backing)return 1u<<10;if(i==h->widget_backing||i==h->widget_dpad)return 1u<<11;if(i==h->widget_centre)return 1u<<12;if(i==h->widget_mail)return 1u<<13;if(i==h->widget_highlight)return 1u<<14;if(i==h->nos_backing)return 1u<<15;if(i==h->nos_alpha)return 1u<<16;if(i==h->shift_icon)return 1u<<17;if(i==h->glow_node)return 1u<<18;if(i==h->mapnode)return 1u<<5;if(i==h->masknode)return 1u<<6;if(i==h->north)return 1u<<7;if(i==h->player)return 1u<<8;if(descendant(h,i,h->border))return 1u<<9;if(r&&!strcmp(r,"3rdperson_redline.tga"))return 1u<<1;if(r&&!strcmp(r,"3rdperson_10500lines.tga"))return 1u<<2;if(r&&!strcmp(r,"Tach_Fill.tga"))return 1u<<3;if(h->package.nodes[i].kind==2)return 1u<<4;return 0;}
static void quad(Hud*h,int i,const float rect[4],const float uv[4],Texture t,float angle,int width,int height,int recenter,int roads,const double mapuv[3],double heading){if(!t.id.id)return;h->resolved|=feature(h,i);HaNode*n=h->package.nodes+i;float v[24],scale=fminf(width/640.0f,height/480.0f);float center[3]={0},old[3]={0};if(recenter&&h->mapnode>=0){ha_point(&h->package,(size_t)h->mapnode,old,center,0);ha_point(&h->package,(size_t)i,old,old,0);}
 /* wide policy: translation of this node's group; a recentred indicator follows the map's group */
 double sh=fit_shift(h,recenter&&h->mapnode>=0?h->mapnode:i,width,height),shm=fit_shift(h,h->masknode,width,height);
 for(int j=0;j<4;j++){float local[3]={rect[(j==1||j==2)?2:0],rect[j>=2?3:1],0},p[3];ha_point(&h->package,(size_t)i,local,p,angle);if(recenter){p[0]+=center[0]-old[0];p[1]+=center[1]-old[1];}if(n->kind==HA_KIND_TEXT){p[0]+=h->text_offset[0];p[1]+=h->text_offset[1];}v[j*6]=(float)((p[0]+sh)*scale*2/width);v[j*6+1]=-p[1]*scale*2/height;float a=(j==1||j==2)?1:0,b=j>=2?1:0;v[j*6+2]=uv[0]+a*(uv[2]-uv[0]);v[j*6+3]=uv[1]+b*(uv[3]-uv[1]);v[j*6+4]=a;v[j*6+5]=b;if(roads){HaNode*mn=h->package.nodes+h->masknode;float origin[3],bx[3],by[3],z[3]={0},xx[3]={1,0,0},yy[3]={0,1,0};ha_point(&h->package,(size_t)h->masknode,z,origin,0);ha_point(&h->package,(size_t)h->masknode,xx,bx,0);ha_point(&h->package,(size_t)h->masknode,yy,by,0);float ax=bx[0]-origin[0],ay=bx[1]-origin[1],cx=by[0]-origin[0],cy=by[1]-origin[1],det=ax*cy-ay*cx;float dx=(float)(p[0]+sh-(origin[0]+shm)),dy=p[1]-origin[1];if(fabsf(det)>1e-8f&&mn->size[0]!=0&&mn->size[1]!=0){float mx=(cy*dx-cx*dy)/det/mn->size[0]+.5f,my=(-ay*dx+ax*dy)/det/mn->size[1]+.5f;v[j*6+4]=mn->uv[0]+mx*(mn->uv[2]-mn->uv[0]);v[j*6+5]=mn->uv[1]+my*(mn->uv[3]-mn->uv[1]);}double x=(a-.5)*mapuv[2],y=(b-.5)*mapuv[2],c=cos(heading),s=sin(heading);v[j*6+2]=(float)(mapuv[0]+c*x-s*y);v[j*6+3]=(float)(mapuv[1]+s*x+c*y);}}
 const unsigned char*c=h->tint_on?h->tint_rgba:n->color;
 h->hud_params->roads=(float)roads;
 h->hud_params->tint[0]=c[0]/255.0f;h->hud_params->tint[1]=c[1]/255.0f;h->hud_params->tint[2]=c[2]/255.0f;h->hud_params->tint[3]=c[3]/255.0f;
 h->hud_params->dialClip[0]=h->clip[0];h->hud_params->dialClip[1]=h->clip[1];h->hud_params->dialClip[2]=h->clip[2];h->hud_params->dialClip[3]=h->clip[3];
 /* STREAM contract: fresh storage per draw. Updating storage that earlier HUD
    draws still read made the GL driver wait for the GPU before every glyph. */
 rhi_buffer_replace(h->vbo,sizeof v,v);
 rhi_cmd_bind_pipeline(h->tri_pipe);rhi_cmd_bind_vertex_buffer(0,h->vbo,0);rhi_cmd_bind_index_buffer(h->ibo);
 rhi_cmd_bind_texture(0,t.id);
 rhi_cmd_draw_indexed(6,0);
 h->drawn|=feature(h,i);if(roads){h->resolved|=1u<<6;h->drawn|=1u<<6;}}
static Font *font_for(Hud*h,HaNode*n){const char*name=resource_name(h,n);if(name)for(size_t i=0;i<h->font_count;i++)if(equalfont(name,h->fonts[i].metrics.name)&&h->fonts[i].atlas.id.id)return h->fonts+i;diagnostic(h,"requested original font missing; trying original 21i");for(size_t i=0;i<h->font_count;i++)if(equalfont("conduitmditc_tt21i",h->fonts[i].metrics.name)&&h->fonts[i].atlas.id.id)return h->fonts+i;diagnostic(h,"no original font available; text omitted");return NULL;}
static HaGlyph *glyph(Font*f,unsigned char c){HaGlyph*question=NULL;for(size_t i=0;i<f->metrics.count;i++){HaGlyph*g=f->metrics.glyphs+i;if(g->code==c)return g;if(g->code=='?')question=g;}return question;}
static void text(Hud*h,int i,const char*s,int w,int height){if(i<0||!*s)return;HaNode*n=h->package.nodes+i;if(!n->valid||!n->appearance)return;if(n->resource>=h->package.resource_count||h->package.resources[n->resource].kind!=2||!resource_name(h,n)){diagnostic(h,"unresolved text resource; text omitted");return;}Font*f=font_for(h,n);if(!f)return;float advance=0;for(const char*q=s;*q;q++){HaGlyph*g=glyph(f,(unsigned char)*q);advance+=g?g->advance:f->metrics.line;}float x=ha_text_start(0,n->size[0],n->justification,advance);for(const char*q=s;*q;q++){HaGlyph*g=glyph(f,(unsigned char)*q);if(g){HaGlyph cell=*g;int inside;if(f->cells){const HaGlyphCell*c=f->cells+(g-f->metrics.glyphs);inside=c->ok;cell.x=c->x;cell.y=c->y;}else inside=(unsigned)g->x+g->width<=(unsigned)f->atlas.w&&(unsigned)g->y+g->height<=(unsigned)f->atlas.h;if(!inside){diagnostic(h,"glyph outside resolved atlas");}else{/* G3: whole cell, unclipped, independent of the advance; drawn in string order (provisional overlap order). G6: edge to edge, no inset unless the compatibility setting is on. */HaGlyphQuad gq;if(ha_glyph_quad(&cell,f->inv_w,f->inv_h,x,0,n->size[0],n->size[1],h->settings.glyph_half_texel_inset,&gq)){float rect[]={(float)gq.x0,(float)gq.y0,(float)gq.x1,(float)gq.y1},uv[]={(float)gq.u0,(float)gq.v0,(float)gq.u1,(float)gq.v1};quad(h,i,rect,uv,f->atlas,0,w,height,0,0,NULL,0);}}x+=g->advance*n->size[0];}else{x+=f->metrics.line*n->size[0];diagnostic(h,"missing font character; advance only");}}}
/* Runtime event placement is project policy: the static rank/lap poses overlap.
 * Keep data anchors, combine the rank, and fit it into the supplied corner art. */
static void race_text(Hud*h,const HudState*s,int w,int height,int compact_angle){
 char buf[64];
 for(int k=0;k<HR_FIELD_COUNT;k++){
  int i=h->race_nodes[k];if(i<0||k==HR_ORDINAL||k==HR_CARS||!hud_race_text(s,k,buf))continue;
  if(k==HR_DRIFT_ANGLE&&s->hide_gauge)continue;
  HaNode*n=h->package.nodes+i;unsigned justification=n->justification;
  if(k==HR_POSITION){
   char ordinal[64],cars[64];hud_race_text(s,HR_ORDINAL,ordinal);hud_race_text(s,HR_CARS,cars);
   snprintf(buf,sizeof buf,"%d%.2s %.16s",s->position,ordinal,cars);
   int corner=node_resource(h,h->layout_mode==2?"Drag_Position_Backing_01.tga":"3rdperson_BoxCorner.tga");
   int right=h->race_nodes[HR_TIME]>=0?h->race_nodes[HR_TIME]:h->race_nodes[HR_LAPS];
   if(corner>=0&&right>=0){float z[3]={0},p[3],c[3],r[3];ha_point(&h->package,i,z,p,0);ha_point(&h->package,corner,z,c,0);ha_point(&h->package,right,z,r,0);
    h->text_offset[0]=r[0]-p[0];h->text_offset[1]=c[1]-h->package.nodes[corner].size[1]*.25f-p[1];n->justification=2;}
  }
  if(k==HR_DRIFT_ANGLE&&compact_angle&&h->drift_angle_left>=0){
   float zero[3]={0},split[3],compact[3];
   ha_point(&h->package,(size_t)h->drift_angle_left,zero,split,0);
   ha_point(&h->package,(size_t)h->drift_angle_compact,zero,compact,0);
   h->text_offset[0]=compact[0]-split[0]+(float)(fit_shift(h,h->drift_angle_compact,w,height)-fit_shift(h,h->drift_angle_left,w,height));
   h->text_offset[1]=compact[1]-split[1];
  }
  if(k==HR_BANNER){memset(h->tint_rgba,255,4);h->tint_on=1;}
  text(h,i,buf,w,height);h->tint_on=0;n->justification=justification;h->text_offset[0]=h->text_offset[1]=0;
 }
}
static int map_vertex(Hud*h,double x,double y,float*v){
 HaMapFrame*f=&h->pick_frame;double p[2];if(!hud_race_map_point(f,x,y,p))return 0;
 float z[3]={0},xx[3]={1,0,0},yy[3]={0,1,0},o[3],bx[3],by[3];
 ha_point(&h->package,h->masknode,z,o,0);ha_point(&h->package,h->masknode,xx,bx,0);ha_point(&h->package,h->masknode,yy,by,0);
 double a=bx[0]-o[0],b=bx[1]-o[1],c=by[0]-o[0],d=by[1]-o[1],det=a*d-b*c;
 HaNode*n=h->package.nodes+h->masknode;if(fabs(det)<1e-12||!n->size[0]||!n->size[1])return 0;
 double dx=p[0]-o[0]-fit_shift(h,h->masknode,f->width,f->height),dy=p[1]-o[1],scale=fmin(f->width/640.0,f->height/480.0);
 double u=(d*dx-c*dy)/det/n->size[0]+.5,vv=(-b*dx+a*dy)/det/n->size[1]+.5;
 v[0]=(float)(p[0]*scale*2/f->width);v[1]=(float)(-p[1]*scale*2/f->height);v[2]=v[3]=0;
 v[4]=(float)(n->uv[0]+u*(n->uv[2]-n->uv[0]));v[5]=(float)(n->uv[1]+vv*(n->uv[3]-n->uv[1]));return 1;
}
/* lines != 0: a line strip of `count` points (map route, drawn at width 2).
   Otherwise a TRIANGLE_FAN of `count` (<= 4) points (opponent markers). */
static void map_vertices(Hud*h,const float*v,int count,int lines,float r,float g,float b){
 if(count<2||(lines&&count>HA_LIMIT+1))return;   /* a closed HA_LIMIT-point route is HA_LIMIT+1 verts */
 h->hud_params->roads=2;
 h->hud_params->tint[0]=r;h->hud_params->tint[1]=g;h->hud_params->tint[2]=b;h->hud_params->tint[3]=1;
 h->hud_params->dialClip[0]=h->hud_params->dialClip[1]=h->hud_params->dialClip[2]=h->hud_params->dialClip[3]=0;
 rhi_buffer_replace(h->vbo,(size_t)count*6*sizeof(float),v);
 rhi_cmd_bind_vertex_buffer(0,h->vbo,0);
 if(lines){rhi_cmd_bind_pipeline(h->line_pipe);rhi_cmd_bind_index_buffer(h->line_ibo);rhi_cmd_draw_indexed((uint32_t)(count-1)*2,0);}
 else{rhi_cmd_bind_pipeline(h->tri_pipe);rhi_cmd_bind_index_buffer(h->ibo);rhi_cmd_draw_indexed(6,0);}
}
static void map_overlay(Hud*h,const HudState*s,int show_route){
 if(!h->pick_frame.enabled)return;
 float vertices[(HA_LIMIT+1)*6];
 /* shortcut: overlays accept up to HA_LIMIT points; stream larger decoded courses when needed. */
 if(show_route&&s->racing&&s->route_xy&&s->route_n>1&&s->route_n<=HA_LIMIT){
  int ok=1;for(int i=0;i<s->route_n;i++)if(!map_vertex(h,s->route_xy[i*2],s->route_xy[i*2+1],vertices+i*6)){ok=0;break;}
  if(ok){int n=s->route_n;if(s->route_loop){memcpy(vertices+n*6,vertices,6*sizeof(float));n++;}
   map_vertices(h,vertices,n,1,.2f,.8f,1);}
 }
 for(int i=0;i<s->opponent_n&&i<64&&s->opponent_xy;i++){
  float center[6];if(!map_vertex(h,s->opponent_xy[i*2],s->opponent_xy[i*2+1],center))continue;
  /* Marker size is a display policy in world metres, following the same mask. */
  double d=h->pick_frame.window*.025;
  /* Race opponents are diamonds; free-roam rivals orange triangles. */
  const double diamond[4][2]={{-1,0},{0,-1},{1,0},{0,1}},triangle[4][2]={{-1,-.8},{1,-.8},{0,1.2},{0,1.2}};
  const double (*offsets)[2]=s->racing?diamond:triangle;int ok=1;
  for(int j=0;j<4;j++)if(!map_vertex(h,s->opponent_xy[i*2]+offsets[j][0]*d,s->opponent_xy[i*2+1]+offsets[j][1]*d,vertices+j*6))ok=0;
  if(ok)map_vertices(h,vertices,4,0,1,s->racing?.3f:.55f,s->racing?.15f:.05f);
 }
}
/* Text with its role's tint (C1-3/E1 gear colour; others authored). */
static void role_text(Hud*h,int i,int role,int gear,int change,double speed_mps,const char*s,int w,int height){if(i<0)return;HaNode*n=h->package.nodes+i;ha_text_tint(role,gear,change,speed_mps,h->settings.gear_standstill_mps,n->color,h->settings.gear_colour,h->tint_rgba);h->tint_on=1;text(h,i,s,w,height);h->tint_on=0;}
void hud_draw(Hud*h,const RProg*rp,GpuMesh*unused,const HudState*s,int w,int height){if(!h)return;
 h->pick_frame.enabled=0;if(!s||w<=0||height<=0)return;h->pick_frame.width=w;h->pick_frame.height=height;
 if(!load_layout(h,layout_mode(s)))return;
 {HaWidget shown;ha_widget(s->career_context,s->message_waiting!=0,h->widget_ok,1,&shown);
  /* Set 4 D4: while the authored widget is drawn, inherited message visuals are suppressed. */
  if(rp&&unused&&(!s->racing||s->nav_active)){HudState base=*s;
   base.racing=0;
   if(s->racing)base.notice=NULL; /* The authored race banner owns this notice. */
   if(s->racing||!ha_inherited_message_visuals(0,shown.visible,s->message_waiting!=0))base.message_waiting=0;
   hud_baseline_set_preview(h->auxiliary,h->preview&&!s->racing);
   hud_baseline_draw(h->auxiliary,rp,unused,&base,w,height);}}
 if(!h->hud_shader.id||!h->hud_params)return;
 h->resolved=h->drawn=0;
 /* Save the caller's raster state and bindings; the pipelines set everything
    the draws need (no depth/cull, source-over alpha). Restored at the end. */
 RhiStateSnapshot hud_saved; rhi_cmd_save_state(&hud_saved);
 int city_map=h->city_map.id.id!=0;
 load_race_map(h,s->racing&&h->mapnode>=0?s->race_event_id:0,s);
 map_jobs_pump(h,h->map_sync);
 int authored_map=h->race_map.id.id!=0;
 const double *origin=authored_map?h->race_map_origin:h->origin;
 double heading=s->map_heading_explicit?s->map_heading_clockwise:M_PI*.5-s->car_heading;
 double map_window=s->map_window_world>0?s->map_window_world:500,mapuv[3];
 int mapon=!s->hide_map&&(authored_map||h->track_ok)&&
    ha_map(origin[0],origin[1],origin[2],s->car_x,s->car_y,heading,map_window,mapuv);
 if(!s->hide_map&&!mapon)diagnostic(h,"map suppressed: invalid input or metadata");
 int scale=ha_scale(s->rpm_maximum),skin=ha_skin(s->skin_index);
 Texture turbo_fill={0},turbo_lines={0};
 int own_fill=0,own_lines=0;
 if(!s->hide_gauge&&s->have_turbo&&h->drift_turbo_fill>=0&&h->drift_turbo_lines>=0){
  turbo_fill=image_texture(h,h->drift_turbo_fill,skin,scale,&own_fill);
  turbo_lines=image_texture(h,h->drift_turbo_lines,skin,scale,&own_lines);
 }
 int turbo_visible=turbo_fill.id.id&&turbo_lines.id.id;
 int compact_angle=!turbo_visible&&node_texture(h,h->drift_angle_compact).id.id;
 /* Capture amendments: per-frame element decisions from pure contracts. */
 const HudSettings*cfg=&h->settings;float dial[2]={0,0};int dial_ok=0;
 if(h->needle>=0&&h->package.nodes[h->needle].valid){float p[3];ha_point(&h->package,(size_t)h->needle,h->package.nodes[h->needle].pivot,p,0);float fit=fminf(w/640.0f,height/480.0f);dial[0]=(float)((p[0]+fit_shift(h,h->needle,w,height))*fit*2/w);dial[1]=-p[1]*fit*2/height;dial_ok=1;}
 double redline_start=0;int redline=s->hide_gauge?0:ha_redline(cfg->inferred_redline,s->have_redline_rpm,s->rpm_redline,scale,&redline_start);
 if(redline<0)diagnostic(h,"inferred redline hidden: nonfinite redline RPM");
 if(redline>0&&!dial_ok){diagnostic(h,"inferred redline hidden: needle rotation centre unavailable");redline=0;}
 double nos_limit=0;int nos=s->hide_gauge?0:ha_nos_level(cfg->inferred_nos_level,s->have_nos,s->have_nitro_tank,s->nitro_tank,cfg->nos_a0_degrees,cfg->nos_a1_degrees,&nos_limit);
 if(nos<0)diagnostic(h,"inferred NOS level hidden: nonfinite level or span");
 if(nos>0&&(h->nos_backing<0||h->nos_alpha<0)){diagnostic(h,"NOS level targets unresolved; NOS level hidden");nos=0;}
 if(!s->hide_gauge&&h->layout_mode==2&&s->have_nos){nos=s->have_nitro_tank&&isfinite(s->nitro_tank)&&s->nitro_tank>0?2:1;}
 if(nos==2&&!dial_ok){diagnostic(h,"NOS level arc hidden: needle rotation centre unavailable");nos=1;}
 int shift=s->hide_gauge?0:ha_shift_mode(cfg->shift_indicator,s->have_shift_rpm,s->shift_rpm,s->gear,s->rpm,s->rpm_maximum,s->transmission,cfg->shift_icon_automatic);
 if(shift<0){diagnostic(h,"shift indicator hidden: nonfinite or nonpositive shift RPM");shift=0;}
 if(shift&&h->shift_icon<0){diagnostic(h,"shift icon target unresolved; shift indicator hidden");shift=0;}
 /* H3 item 5: flash phase restarts whenever the icon becomes visible. */
 int shift_bad_clock=0;double shift_env=ha_shift_flash(cfg->shift_flash,ha_shift_elapsed(&h->shift_phase,shift>0,s->clock_ms),cfg->glow_ms_per_unit,&shift_bad_clock);
 if(shift&&shift_bad_clock)diagnostic(h,"shift icon drawn steadily lit: invalid clock or time unit");
 HaGlow glow;memset(&glow,0,sizeof glow);int glow_on=0;
 if(cfg->nos_glow&&h->glow_active){
  double gt=ha_glow_units(s->clock_ms-h->glow_start,cfg->glow_ms_per_unit);
  if(isnan(gt)){diagnostic(h,"N2O glow hidden: invalid clock or time unit");}
  else if(gt<0||gt>=HA_GLOW_END)h->glow_active=0; /* finished (or the clock restarted) */
  else if(!s->hide_gauge){glow_on=ha_glow(gt,&glow);if(glow_on&&h->glow_node<0){diagnostic(h,"N2O glow target unresolved; glow hidden");glow_on=0;}}
 }
 char bankbuf[64];int bank=ha_bank(s->career_context,s->have_money,s->suppress_cash,(int64_t)s->money,',',h->bank_ok,bankbuf);
 if(bank<0)diagnostic(h,h->bank_ok?"negative balance: bank suppressed":"bank targets unresolved; bank hidden");
 HaWidget wid;ha_widget(s->career_context,s->message_waiting!=0,h->widget_ok,h->centre_texture.id.id!=0,&wid);
 if(wid.diagnostic)diagnostic(h,h->widget_ok?"widget centre figure texture missing; figure omitted":"widget targets unresolved; widget hidden");
 h->hud_params->halfSize[0]=w*.5f;h->hud_params->halfSize[1]=height*.5f;h->tint_on=0;h->clip[3]=0;
 /* Stable insertion sort by transformed depth. Higher z first. */
 size_t count=h->package.count;size_t order[HA_LIMIT];float depthvalues[HA_LIMIT];for(size_t i=0;i<count;i++){float zero[3]={0},p[3];ha_point(&h->package,i,zero,p,0);order[i]=i;depthvalues[i]=p[2];size_t j=i;while(j&&depthvalues[order[j-1]]<depthvalues[i]){order[j]=order[j-1];j--;}order[j]=i;}
 for(size_t k=0;k<count;k++){int i=(int)order[k];HaNode*n=h->package.nodes+i;if(!n->valid||!n->appearance||n->kind!=1)continue;const char*r=resource_name(h,n);int gauge=!s->hide_gauge&&(drag_gauge(h,i)||i==h->needle||(descendant(h,i,h->gauge_root)&&r&&(skin_resource(r)||!strcmp(r,"3rdperson_rpmdial.tga")||(redline>0&&!strcmp(r,"3rdperson_redline.tga")))));int map=mapon&&(i==h->mapnode||i==h->north||i==h->player||descendant(h,i,h->border));
 int heat=!s->hide_gauge&&h->layout_mode==2&&s->have_rpm&&isfinite(s->engine_heat)&&r&&!strcmp(r,"Drag_HEAT_Fill.tga");
 int drift_angle=!s->hide_gauge&&(compact_angle?i==h->drift_angle_compact:
                 (i==h->drift_angle_left||i==h->drift_angle_right));
 int drift_turbo=turbo_visible&&(i==h->drift_turbo_fill||i==h->drift_turbo_lines||
                  i==h->drift_turbo_red[0]||i==h->drift_turbo_red[1]);
 int extra=heat||drift_angle||drift_turbo||(bank>0&&i==h->bank_backing)||(wid.backing&&i==h->widget_backing)||(wid.dpad&&i==h->widget_dpad)||(wid.centre&&i==h->widget_centre)||(wid.mail&&i==h->widget_mail)||(wid.highlight_right&&i==h->widget_highlight)||(nos>=1&&i==h->nos_backing)||(nos==2&&i==h->nos_alpha)||(shift&&i==h->shift_icon)||(glow_on&&i==h->glow_node);
 if(!gauge&&!map&&!extra&&!(s->racing&&race_image(h,i)))continue;
 h->tint_on=0;h->clip[3]=0;
 if(i==h->widget_highlight){ha_setting_rgba(cfg->highlight_colour,h->tint_rgba);h->tint_on=1;}
 if(i==h->shift_icon){ha_setting_tint(n->color,cfg->shift_colour,h->tint_rgba);h->tint_rgba[3]=(unsigned char)lround(n->color[3]*shift_env);h->tint_on=1;} /* H3 item 6: alpha = authored x envelope */
 if(i==h->nos_alpha&&h->layout_mode!=2){h->clip[0]=dial[0];h->clip[1]=dial[1];h->clip[2]=(float)(nos_limit*M_PI/180);h->clip[3]=1;}
 if(gauge&&r&&!strcmp(r,"3rdperson_redline.tga")){h->clip[0]=dial[0];h->clip[1]=dial[1];h->clip[2]=(float)(redline_start*M_PI/180);h->clip[3]=2;}
 if(i==h->glow_node){/* E3: authored centre and RGB; size, rotation (counter-clockwise) and alpha follow the curve; source-over. */
  int owned_g=0;Texture gt=image_texture(h,i,skin,scale,&owned_g);float pv[3];memcpy(pv,n->pivot,sizeof pv);memset(n->pivot,0,sizeof n->pivot);
  h->tint_rgba[0]=n->color[0];h->tint_rgba[1]=n->color[1];h->tint_rgba[2]=n->color[2];h->tint_rgba[3]=glow.alpha_byte;h->tint_on=1;
  float grect[]={(float)(-glow.width/2),(float)(-glow.height/2),(float)(glow.width/2),(float)(glow.height/2)};quad(h,i,grect,n->uv,gt,(float)(-glow.rotation_ccw_deg*M_PI/180),w,height,0,0,NULL,0);
  memcpy(n->pivot,pv,sizeof pv);if(owned_g)rhi_texture_destroy(gt.id);continue;}
 if(i==h->widget_centre){float full[4]={0,0,1,1},rect[]={-n->size[0]/2,-n->size[1]/2,n->size[0]/2,n->size[1]/2};quad(h,i,rect,full,h->centre_texture,0,w,height,0,0,NULL,0);continue;}
 int roads=i==h->mapnode,owned=0;
 Texture t;
 if(i==h->drift_turbo_fill)t=turbo_fill;
 else if(i==h->drift_turbo_lines)t=turbo_lines;
 else if(roads&&authored_map)t=h->race_map;
 else if(roads&&city_map)t=h->city_map;
 else t=image_texture(h,i,skin,scale,&owned);
 float angle=0;
 if(i==h->needle)
  angle=h->layout_mode==2?hud_race_drag_angle(n,s->rpm):(float)(ha_rpm_angle(s->rpm,s->rpm_maximum,0)*M_PI/180);
 if(i==h->north)angle=(float)-heading;
 if(roads){
  Texture mask=node_texture(h,h->masknode);
  if(!mask.id.id){
   diagnostic(h,"map mask missing; roads omitted");
   if(owned)rhi_texture_destroy(t.id);
   continue;
  }
  rhi_cmd_bind_texture(1,mask.id);
  if(t.id.id&&h->mask_alpha){
   HaMapFrame*f=&h->pick_frame;
   float zero[3]={0},bx[3]={1,0,0},by[3]={0,1,0},o[3],x[3],y[3];
   ha_point(&h->package,(size_t)i,zero,o,0);
   ha_point(&h->package,(size_t)i,bx,x,0);
   ha_point(&h->package,(size_t)i,by,y,0);
   f->center[0]=o[0]+fit_shift(h,i,w,height);f->center[1]=o[1];
   f->basis[0]=x[0]-o[0];f->basis[1]=x[1]-o[1];
   f->basis[2]=y[0]-o[0];f->basis[3]=y[1]-o[1];
   f->size[0]=n->size[0];f->size[1]=n->size[1];
   f->world[0]=s->car_x;f->world[1]=s->car_y;
   f->heading=heading;f->window=map_window;f->enabled=1;
  }
 }
 float rect[]={-n->size[0]/2,-n->size[1]/2,n->size[0]/2,n->size[1]/2};
 float uv[4];memcpy(uv,n->uv,sizeof uv);
 if(heat||(h->layout_mode==2&&i==h->nos_alpha)){float fraction=heat?s->engine_heat:s->nitro_tank;fraction=fminf(1,fmaxf(0,fraction));rect[1]=rect[3]-(rect[3]-rect[1])*fraction;uv[1]=uv[3]-(uv[3]-uv[1])*fraction;}
 quad(h,i,rect,uv,t,angle,w,height,i==h->player,roads,mapuv,heading);if(owned)rhi_texture_destroy(t.id);if(roads)map_overlay(h,s,!authored_map);}
 h->tint_on=0;h->clip[3]=0;
 if(!s->hide_gauge){char buf[32];if(ha_speed(s->speed_kmh/3.6,!s->imperial_units,buf))text(h,h->speed,buf,w,height);else diagnostic(h,"speed suppressed: invalid or outside 0..INT_MAX");if(!ha_gear(s->gear,buf))diagnostic(h,"unsupported gear state");role_text(h,h->gear,HA_TEXT_GEAR,s->gear,s->gear_change_in_progress!=0,s->speed_kmh/3.6,buf,w,height);text(h,h->unit,s->imperial_units?"MPH":"KM/H",w,height);}
 if(s->racing)race_text(h,s,w,height,compact_angle);
 if(own_fill)rhi_texture_destroy(turbo_fill.id);
 if(own_lines)rhi_texture_destroy(turbo_lines.id);
 if(bank>0){role_text(h,h->bank_label,HA_TEXT_BANK_LABEL,0,0,0,ha_bank_label,w,height);role_text(h,h->bank_value,HA_TEXT_BANK_VALUE,0,0,0,bankbuf,w,height);}
 rhi_cmd_restore_state(&hud_saved);
}
float hud_angle_delta(float from, float to) {
    float d = to - from;
    /* fmod alone leaves (-2pi, 2pi); fold once more to land in (-pi, pi]. */
    d = (float)fmod((double)d, 2.0 * M_PI);
    if (d <= -(float)M_PI) d += 2.0f * (float)M_PI;
    if (d >   (float)M_PI) d -= 2.0f * (float)M_PI;
    return d;
}

int hud_speed_value(float speed_kmh, int imperial) {
    if (!isfinite(speed_kmh)) return 0;
    double magnitude = fabs((double)speed_kmh);
    if (imperial) magnitude /= 1.609344;
    if (magnitude >= INT_MAX) return INT_MAX;
    return (int)magnitude;
}

float hud_tach_fraction(const HudState *s, float dial_scale) {
    if (!s || !s->have_rpm || !isfinite(s->rpm) ||
        !isfinite(dial_scale) || dial_scale <= 0) return 0;
    if (!isfinite(s->rpm_maximum) || s->rpm_maximum < 0) return 0;
    float maximum = s->rpm_maximum > 0 ? s->rpm_maximum : s->rpm_redline;
    if (!isfinite(maximum) || maximum <= 0) return 0;
    float reading = fminf(maximum, fmaxf(0, s->rpm));
    return fminf(1, reading / dial_scale);
}

void hud_map_project(float wx, float wy, float cx, float cy,
                     float radius_m, float rot, float *mx, float *my) {
    if (radius_m < 1e-3f) radius_m = 1e-3f;
    float dx = (wx - cx) / radius_m, dy = (wy - cy) / radius_m;
    float c = cosf(rot), s = sinf(rot);
    *mx = dx * c - dy * s;
    *my = dx * s + dy * c;
}

void hud_status_text(const HudState *s, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!s || !s->racing) return;
    if(s->race_kind==N2_RACE_DRIFT) {
        snprintf(out,(size_t)cap,"SCORE %.0f  +%.0f",s->drift_score,s->drift_chain);
    } else if(s->race_kind==N2_RACE_DRAG) {
        snprintf(out,(size_t)cap,s->engine_failed?"ENGINE BLOWN":"HEAT %d PCT%s",(int)(s->engine_heat*100),s->shift_ready?" SHIFT":"");
    } else if (s->circuit) {
        int lap = s->lap < 1 ? 1 : s->lap, laps = s->laps < 1 ? 1 : s->laps;
        if (lap > laps) lap = laps;
        snprintf(out, (size_t)cap, "LAP %d/%d", lap, laps);
    } else {
        float p = s->sprint_progress;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        snprintf(out, (size_t)cap, "%d PCT", (int)(p * 100.0f + 0.5f));
    }
}

int hud_shows_nos(const HudState *s) { return s && s->have_nos; }

int hud_shows_money(const HudState *s) {
    return s && !s->racing && s->have_money;
}

float hud_art_run_fraction(const unsigned char *rgb, int w, int h) {
    if (!rgb || w < 2 || h < 1) return 1.0f;      /* nothing to judge */
    long runs = 0, cmp = 0;
    for (int y = 0; y < h; y++)
        for (int x = 1; x < w; x++) {
            const unsigned char *a = rgb + (((long)y * w) + x - 1) * 3;
            const unsigned char *b = a + 3;
            /* 5 bits per channel: ignores dithering, keeps real edges */
            if ((a[0] >> 3) == (b[0] >> 3) && (a[1] >> 3) == (b[1] >> 3) &&
                (a[2] >> 3) == (b[2] >> 3)) runs++;
            cmp++;
        }
    return cmp ? (float)runs / (float)cmp : 1.0f;
}

void hud_place(float aspect, float anchor_x, float anchor_y,
               float off_x, float off_y, float *ndc_x, float *ndc_y) {
    if (aspect < 1e-3f) aspect = 1e-3f;
    *ndc_x = anchor_x + off_x / aspect;
    *ndc_y = anchor_y + off_y;
}


void hud_audit(const Hud*h,HudAudit*out){memset(out,0,sizeof *out);if(!h)return;out->nodes=(unsigned)h->package.count;out->resources=(unsigned)h->package.resource_count;for(size_t i=0;i<h->package.count;i++){switch(h->package.nodes[i].kind){case 1:out->images++;break;case 2:out->texts++;break;case 5:out->groups++;break;}}out->fonts=(unsigned)h->font_count;for(size_t i=0;i<h->font_count;i++)if(h->fonts[i].atlas.id.id)out->original_atlases++;out->gauge_targets=h->speed>=0&&h->gear>=0&&h->needle>=0&&h->unit>=0;out->map_targets=h->mapnode>=0&&h->masknode>=0&&h->north>=0&&h->player>=0&&h->border>=0;out->track_metadata=h->track_ok;out->resource_resolved=h->resolved;out->resource_drawn=h->drawn;}

int hud_map_pick(float x,float y,int w,int h,const HudState*s,float*wx,float*wy){return hud_baseline_map_pick(x,y,w,h,s,wx,wy);}

int hud_catalog_has(const unsigned char*data,size_t len,uint32_t key){Pack p={0};catalog(&p,data,len,0);int found=0;for(size_t i=0;i<p.slot_count;i++)if(slot_present(p.slots+i,key)){found=1;break;}free(p.slots);return found;}

int hud_pick(Hud*h,float nx,float ny,float*wx,float*wy){
 if(!h||!wx||!wy)return 0;

 HaMapFrame*f=&h->pick_frame;double screenx=(nx+1)*f->width*.5,screeny=(1-ny)*f->height*.5,result[2];if(!ha_map_pick(f,screenx,screeny,1,result)||!h->mask_alpha||h->masknode<0)return 0;
 float zero[3]={0},one[3]={1,0,0},two[3]={0,1,0},o[3],x[3],y[3];ha_point(&h->package,(size_t)h->masknode,zero,o,0);ha_point(&h->package,(size_t)h->masknode,one,x,0);ha_point(&h->package,(size_t)h->masknode,two,y,0);double scale=fmin(f->width/640.0,f->height/480.0),dx=(screenx-f->width*.5)/scale-(o[0]+fit_shift(h,h->masknode,f->width,f->height)),dy=(screeny-f->height*.5)/scale-o[1],a=x[0]-o[0],b=x[1]-o[1],c=y[0]-o[0],d=y[1]-o[1],det=a*d-b*c;
 HaNode*n=h->package.nodes+h->masknode;if(fabs(det)<1e-12||!n->size[0]||!n->size[1])return 0;double mx=(d*dx-c*dy)/det/n->size[0]+.5,my=(-b*dx+a*dy)/det/n->size[1]+.5,u=n->uv[0]+mx*(n->uv[2]-n->uv[0]),v=n->uv[1]+my*(n->uv[3]-n->uv[1]);double px=fmax(0,fmin(h->mask_width-1,u*h->mask_width-.5)),py=fmax(0,fmin(h->mask_height-1,v*h->mask_height-.5));int ix=(int)px,iy=(int)py,jx=ix+1<h->mask_width?ix+1:ix,jy=iy+1<h->mask_height?iy+1:iy;double tx=px-ix,ty=py-iy;double coverage=(1-ty)*((1-tx)*h->mask_alpha[iy*h->mask_width+ix]+tx*h->mask_alpha[iy*h->mask_width+jx])+ty*((1-tx)*h->mask_alpha[jy*h->mask_width+ix]+tx*h->mask_alpha[jy*h->mask_width+jx]);if(coverage<=0)return 0;*wx=(float)result[0];*wy=(float)result[1];return 1;
}

#ifdef HUD_TEST_HOOKS
/* Test only (compiled with -DHUD_TEST_HOOKS, see hud.h): a HUD whose authored package is a single white image node shown as the career
 * widget backing, so the real hud_draw can be exercised with a GL context and no game data. */
Hud *hud_test_white_widget(void){
 Hud*h=calloc(1,sizeof *h);if(!h)return NULL;hud_default_settings(&h->settings);
 release_layout(h);   /* clears the package and sets every node index to -1 */
 HaNode*n=calloc(1,sizeof *n);HaResource*r=calloc(1,sizeof *r);Texture*t=calloc(1,sizeof *t);
 char*name=strdup("white.tga");
 if(!n||!r||!t||!name){free(n);free(r);free(t);free(name);free(h);return NULL;}
 n->kind=HA_KIND_IMAGE;n->valid=1;n->appearance=1;n->parent=-1;n->resource=0;
 n->size[0]=n->size[1]=64;n->uv[2]=n->uv[3]=1;memset(n->color,255,sizeof n->color);n->quaternion[3]=1;
 r->kind=1;r->name=name;
 unsigned char white[4]={255,255,255,255};
 t[0].id=rhi_texture_create(&(RhiTextureDesc){RHI_TEXTURE_2D,RHI_FORMAT_RGBA8,1,1,RHI_TEXTURE_SAMPLED});
 rhi_texture_upload(t[0].id,0,0,white,sizeof white);t[0].w=t[0].h=1;
 h->package.nodes=n;h->package.count=1;h->package.resources=r;h->package.resource_count=1;h->textures=t;
 h->layout_mode=0;h->layout_ready=1;h->widget_ok=1;
 h->bank_value=h->bank_label=h->bank_backing=-1;h->widget_dpad=h->widget_highlight=h->widget_centre=h->widget_mail=-1;
 h->nos_backing=h->nos_alpha=h->shift_icon=h->glow_node=-1;h->widget_backing=0;
 init_shader(h);
 return h;
}
#endif
