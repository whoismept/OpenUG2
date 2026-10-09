#ifndef OPENUG2_RACE_LOG_H
#define OPENUG2_RACE_LOG_H
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    long tick; float pos[3], speed; int event, gate;
    char asset[64], note[128];
} RaceLogMark;
typedef struct {
    FILE *file;
    char path[1024], status[160];
    long tick, samples, marks;
    RaceLogMark recent[64];
} RaceLog;

static int race_log_flush(RaceLog *log) {
    if(!log || !log->file)return 0;
    if(!ferror(log->file) && fflush(log->file)==0)return 1;
    fclose(log->file);log->file=NULL;
    snprintf(log->status,sizeof log->status,"Recording stopped: file write failed");
    return 0;
}
static void race_log_stop(RaceLog *log) {
    if(!log || !log->file)return;
    fprintf(log->file,"END,%ld,%ld,%ld\n",log->tick,log->samples,log->marks);
    if(!race_log_flush(log))return;
    int ok=fclose(log->file)==0;log->file=NULL;
    snprintf(log->status,sizeof log->status,ok?"Saved %ld samples and %ld problem marks":"File close failed",log->samples,log->marks);
}
static int race_log_start(RaceLog *log,const char *path) {
    if(!log || !path)return 0;
    if(strlen(path)>=sizeof log->path){snprintf(log->status,sizeof log->status,"Log path is too long");return 0;}
    race_log_stop(log);
    FILE *f=fopen(path,"ab"); /* A repeated filename must never erase a report. */
    if(!f){snprintf(log->status,sizeof log->status,"Cannot open log file");return 0;}
    memset(log,0,sizeof *log);log->file=f;
    snprintf(log->path,sizeof log->path,"%s",path);
    snprintf(log->status,sizeof log->status,"Recording");
    return 1;
}
static void race_log_write(RaceLog *log,const char *format,...) {
    if(!log || !log->file)return;
    va_list args;va_start(args,format);int ok=vfprintf(log->file,format,args)>=0;va_end(args);
    if(!ok || ferror(log->file))race_log_flush(log);
}
static void race_log_text(RaceLog *log,const char *s) {
    if(!log || !log->file)return;
    fputc('"',log->file);
    for(;s && *s;s++) {
        if(*s=='"')fputc('"',log->file);
        fputc(*s=='\n' || *s=='\r'?' ':*s,log->file);
    }
    fputc('"',log->file);
    if(ferror(log->file))race_log_flush(log);
}
static void race_log_mark(RaceLog *log,const RaceLogMark *mark) {
    if(!log || !log->file || !mark)return;
    RaceLogMark m=*mark;m.tick=log->tick;
    m.asset[sizeof m.asset-1]=0;m.note[sizeof m.note-1]=0;
    race_log_write(log,"MARK,%ld,%.3f,%d,%d,%.3f,%.3f,%.3f,%.2f,",m.tick,m.tick/60.0,m.event,m.gate,m.pos[0],m.pos[1],m.pos[2],m.speed);
    race_log_text(log,m.asset);race_log_write(log,",");race_log_text(log,m.note);race_log_write(log,"\n");
    if(race_log_flush(log)) {
        log->recent[log->marks%64]=m;log->marks++;
        snprintf(log->status,sizeof log->status,"Problem #%ld marked and saved",log->marks);
    }
}
#endif
