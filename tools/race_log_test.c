#include "race_log.h"
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    char path[]="/tmp/openug-race-log-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    RaceLog log={0};assert(race_log_start(&log,path));
    RaceLogMark mark={.pos={123,-45,6},.speed=101,.event=4001,.gate=2};
    strcpy(mark.asset,"road\"asset");strcpy(mark.note,"wall, contact\nagain");
    for(int k=0;k<70;k++){log.tick=k*6;race_log_mark(&log,&mark);}
    assert(log.marks==70 && log.recent[69%64].tick==414);
    race_log_stop(&log);assert(!log.file && strstr(log.status,"70 problem marks"));
    assert(race_log_start(&log,path));race_log_write(&log,"SECOND SESSION\n");race_log_stop(&log);
    FILE *f=fopen(path,"rb");assert(f);char line[512];int marks=0,second=0;
    while(fgets(line,sizeof line,f)) {
        if(strncmp(line,"MARK,",5)==0){marks++;assert(strstr(line,"\"road\"\"asset\",\"wall, contact again\""));}
        if(strstr(line,"SECOND SESSION"))second++;
    }
    fclose(f);assert(marks==70 && second==1);unlink(path);
    assert(!race_log_start(&log,"/no/such/directory/race.csv") && !log.file);
    /* Buffered writes can fail only when flushed; never report them as saved. */
    log.file=fopen("/dev/null","rb");assert(log.file);
    race_log_mark(&log,&mark);assert(!log.file && !log.marks && strstr(log.status,"write failed"));
    puts("race log: all markers, bounded display, CSV escaping, append safety and write failure PASS");
}
