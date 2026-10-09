#ifndef OPENUG2_MAP_EDITOR_COLLISION_H
#define OPENUG2_MAP_EDITOR_COLLISION_H
#include "map_editor_gizmo.h"
#include <functional>
#include <map>
#include <set>

namespace mapedit { namespace collision {
using namespace gizmo;
enum Code { RoadCrossing, Gap, Buried, Height, ZeroLength, CodeCount };
inline const char *code_name(Code code) {
    static const char *names[] = {"ROAD_CROSSING", "BARRIER_GAP", "BURIED", "HEIGHT", "ZERO_LENGTH"};
    return code >= RoadCrossing && code < CodeCount ? names[code] : "UNKNOWN";
}
struct Surface { int kind = 0; float z = 0, normal_z = 1; }; // 1 road, 2 terrain, 0 unsupported
using Ground = std::function<Surface(Point)>;
struct Segment {
    Point a,b;
    float height = 0;
    uint64_t item = 0,source = 0;
    int part = 0;
    bool triangle = false;
    Point face[3]{};
};
struct Road { Point a,b; int id = -1; };
struct Finding {
    Code code = Height;
    Point a,b;
    uint64_t item = 0,source = 0,other_item = 0,other_source = 0;
    int part = 0,other_part = -1,road = -1,mesh = -1;
    unsigned count = 1;
    float value = 0;
    std::string detail;
};
struct Results {
    std::vector<Finding> findings;
    size_t segments = 0,generated_segments = 0,source_faces = 0,road_tests = 0,endpoint_tests = 0,omitted = 0;
    bool limited = false;
    std::map<std::pair<uint64_t,int>,size_t> source_groups;
    void add(Finding f) {
        auto key = std::make_pair(f.source,int(f.code));
        if (!f.item && f.source) {
            auto it = source_groups.find(key);
            if (it != source_groups.end()) {
                if (it->second<findings.size()) findings[it->second].count++;
                return;
            }
        }
        if (findings.size() >= 5000) {
            omitted++; limited = true;
            if (!f.item && f.source) source_groups[key]=size_t(-1);
            return;
        }
        if (!f.item && f.source) source_groups[key] = findings.size();
        findings.push_back(std::move(f));
    }
};
inline Point interpolate(Point a,Point b,float t) { return add(a,mul(sub(b,a),t)); }
inline float cross_xy(Point a,Point b) { return a.x*b.y-a.y*b.x; }
inline Point direction(Point a,Point b) { Point d=sub(b,a); d.z=0; return normalized(d); }
inline bool crossing(const Segment &s,const Road &r,float &t,float &u) {
    Point d=sub(s.b,s.a),e=sub(r.b,r.a),offset=sub(r.a,s.a);
    float denominator=cross_xy(d,e),length=std::hypot(d.x,d.y),road_length=std::hypot(e.x,e.y);
    if (length<.1f || road_length<.1f || std::fabs(denominator)<.34f*length*road_length) return false;
    t=cross_xy(offset,e)/denominator; u=cross_xy(offset,d)/denominator;
    return t*length>.05f && (1-t)*length>.05f && u>=0 && u<=1;
}
inline bool buried(const Point *samples,int count,float top,const Ground &ground) {
    for (int n=0;n<count;n++) {
        Point p=samples[n]; p.z=top+.25f;
        Surface g=ground(p);
        if (!g.kind || g.normal_z<.7f || top>=g.z-.15f || g.z-top>2) return false;
    }
    return true;
}
inline void inspect(const Segment &s,const Ground &ground,Results &out) {
    if (s.item) out.segments++; else out.generated_segments++;
    Finding f; f.item=s.item; f.source=s.source; f.part=s.part; f.a=s.a; f.b=s.b;
    float length=std::hypot(s.b.x-s.a.x,s.b.y-s.a.y);
    if (length<.1f) { f.code=ZeroLength; f.value=length; f.detail="Wall has less than 0.1 m horizontal length"; out.add(f); }
    if (s.height<.3f || s.height>30) {
        f.code=Height; f.value=s.height; f.detail="Wall height outside review range 0.3..30 m"; out.add(f);
    }
    bool below=true;
    for (float t : {.15f,.5f,.85f}) {
        Point p=interpolate(s.a,s.b,t); float top=p.z+s.height;
        if (!buried(&p,1,top,ground)) below=false;
    }
    if (below) { f.code=Buried; f.value=s.height; f.detail="Wall top below nearby support at three samples"; out.add(f); }
    Point mid=interpolate(s.a,s.b,.5f);
    Surface g=ground(mid);
    if (g.kind && g.normal_z>=.7f && mid.z-g.z>1 && mid.z-g.z<30) {
        f.code=Height; f.value=mid.z-g.z; f.detail="Wall base more than 1 m above nearby support"; out.add(f);
    }
}
struct RoadIndex {
    std::vector<Road> roads;
    std::map<std::pair<int,int>,std::vector<int>> cells;
    std::vector<int> wide;
    void add(Road road) {
        int id=int(roads.size()); roads.push_back(road);
        int x0=int(std::floor(std::min(road.a.x,road.b.x)/32)),x1=int(std::floor(std::max(road.a.x,road.b.x)/32));
        int y0=int(std::floor(std::min(road.a.y,road.b.y)/32)),y1=int(std::floor(std::max(road.a.y,road.b.y)/32));
        if (int64_t(x1-x0+1)*(y1-y0+1)>4096) { wide.push_back(id); return; }
        for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) cells[{x,y}].push_back(id);
    }
    void check(const Segment &s,const Ground &ground,Results &out) const {
        if (out.road_tests>2000000) { out.limited=true; return; }
        std::set<int> seen;
        bool found=false;
        auto test=[&](int id) {
            if (found || !seen.insert(id).second) return;
            if (++out.road_tests>2000000) { out.limited=true; return; }
            const auto &r=roads[id]; float t,u;
            if (!crossing(s,r,t,u)) return;
            Point p=interpolate(s.a,s.b,t);
            float low=p.z,high=p.z+s.height;
            if (s.triangle) {
                low=INFINITY; high=-INFINITY;
                Point d=sub(s.b,s.a); d.z=0; float length2=dot(d,d);
                for (int k=0;k<3;k++) {
                    Point a=s.face[k],b=s.face[(k+1)%3];
                    float u0=dot(sub(a,s.a),d)/length2,u1=dot(sub(b,s.a),d)/length2;
                    if (t<std::min(u0,u1)-.00001f || t>std::max(u0,u1)+.00001f) continue;
                    if (std::fabs(u1-u0)<.00001f) { low=std::min(low,std::min(a.z,b.z)); high=std::max(high,std::max(a.z,b.z)); }
                    else { float z=a.z+(b.z-a.z)*(t-u0)/(u1-u0); low=std::min(low,z); high=std::max(high,z); }
                }
            }
            if (!std::isfinite(low) || !std::isfinite(high)) return;
            Point at=p; at.z=low+.25f;
            Surface g=ground(at);
            if (g.kind!=1 || g.normal_z<.7f || low>g.z+1.5f || high<g.z+.15f) return;
            float road_length=std::hypot(r.b.x-r.a.x,r.b.y-r.a.y);
            float delta=std::min({2.f,road_length*u,road_length*(1-u)});
            // At a graph node, check both complete edge endpoints instead of zero-distance samples.
            Point a=delta>.05f ? gizmo::add(p,mul(direction(r.a,r.b),-delta)) : r.a;
            Point b=delta>.05f ? gizmo::add(p,mul(direction(r.a,r.b),delta)) : r.b;
            a.z=b.z=g.z;
            Surface ga=ground(a),gb=ground(b);
            if (ga.kind!=1 || gb.kind!=1 || ga.normal_z<.7f || gb.normal_z<.7f ||
                std::fabs(ga.z-g.z)>.2f+.55f*std::hypot(a.x-p.x,a.y-p.y) ||
                std::fabs(gb.z-g.z)>.2f+.55f*std::hypot(b.x-p.x,b.y-p.y)) return;
            Finding f; f.code=RoadCrossing; f.item=s.item; f.source=s.source; f.part=s.part;
            f.a=s.a; f.b=s.b; f.road=r.id; f.value=g.z;
            f.detail="Wall overlaps car-height band across a supported source road-graph link";
            out.add(f); found=true;
        };
        int x0=int(std::floor(std::min(s.a.x,s.b.x)/32)),x1=int(std::floor(std::max(s.a.x,s.b.x)/32));
        int y0=int(std::floor(std::min(s.a.y,s.b.y)/32)),y1=int(std::floor(std::max(s.a.y,s.b.y)/32));
        if (int64_t(x1-x0+1)*(y1-y0+1)>4096) {
            for (size_t n=0;n<roads.size() && !found && out.road_tests<=2000000;n++) test(int(n));
        } else {
            for (int y=y0;y<=y1 && !found && out.road_tests<=2000000;y++)
                for (int x=x0;x<=x1 && !found && out.road_tests<=2000000;x++) {
                    auto cell=cells.find({x,y});
                    if (cell!=cells.end()) for (int id:cell->second) { test(id); if(found || out.road_tests>2000000) break; }
                }
            for (int id:wide) { test(id); if(found || out.road_tests>2000000) break; }
        }
    }
};
inline void gaps(const std::vector<Segment> &segments,Results &out) {
    struct End { Point pos,dir; int segment; };
    std::vector<End> ends;
    RoadIndex boundaries;
    for (size_t n=0;n<segments.size();n++) boundaries.add({segments[n].a,segments[n].b,int(n)});
    std::map<std::pair<int,int>,std::vector<int>> cells;
    for (size_t n=0;n<segments.size();n++) {
        const auto &s=segments[n];
        if (std::hypot(s.a.x-s.b.x,s.a.y-s.b.y)<.1f) continue;
        for (int k=0;k<2;k++) {
            Point pos=k?s.b:s.a,other=k?s.a:s.b;
            int id=int(ends.size()); ends.push_back({pos,direction(other,pos),int(n)});
            cells[{int(std::floor(pos.x/2)),int(std::floor(pos.y/2))}].push_back(id);
        }
    }
    std::vector<bool> connected(ends.size(),false),checked(ends.size(),false);
    std::vector<int> nearest(ends.size(),-1);
    for (size_t n=0;n<ends.size();n++) {
        auto e=ends[n]; float best=2.01f;
        int cx=int(std::floor(e.pos.x/2)),cy=int(std::floor(e.pos.y/2));
        for (int y=cy-1;y<=cy+1;y++) for (int x=cx-1;x<=cx+1;x++) {
            auto cell=cells.find({x,y}); if (cell==cells.end()) continue;
            for (int index:cell->second) {
                if (++out.endpoint_tests>2000000) { out.limited=true; goto emit_completed; }
                auto q=ends[index]; if (q.segment==e.segment || std::fabs(q.pos.z-e.pos.z)>.5f) continue;
                float distance=std::hypot(q.pos.x-e.pos.x,q.pos.y-e.pos.y);
                if (distance<=.15f && dot(e.dir,q.dir)<.9f) connected[n]=true;
                Point toward=direction(e.pos,q.pos);
                if (distance>.15f && distance<=2 && distance<best &&
                    dot(e.dir,toward)>.95f && dot(q.dir,toward)<-.95f) { best=distance; nearest[n]=index; }
            }
        }
        if (nearest[n]>=0 && !connected[n]) {
            Point p=e.pos;
            int bx=int(std::floor(p.x/32)),by=int(std::floor(p.y/32));
            auto test=[&](int id) {
                if (++out.endpoint_tests>2000000) { out.limited=true; return; }
                if (id==e.segment) return;
                const auto &s=segments[id]; Point d=sub(s.b,s.a); d.z=0;
                float length2=dot(d,d);
                if (length2<.01f) return;
                float t=dot(sub(p,s.a),d)/length2;
                Point q=interpolate(s.a,s.b,t);
                if (t>0 && t<1 && std::fabs(q.z-p.z)<=.5f && std::hypot(q.x-p.x,q.y-p.y)<=.15f)
                    connected[n]=true;
            };
            std::set<int> seen;
            for (int y=by-1;y<=by+1;y++) for (int x=bx-1;x<=bx+1;x++) {
                auto cell=boundaries.cells.find({x,y}); if(cell==boundaries.cells.end()) continue;
                for (int id:cell->second) {
                    if (seen.insert(id).second) test(id);
                    if (out.endpoint_tests>2000000) goto emit_completed;
                }
            }
            for (int id:boundaries.wide) { test(id); if(out.endpoint_tests>2000000) goto emit_completed; }
        }
        checked[n]=true;
    }
emit_completed:
    for (size_t n=0;n<ends.size();n++) {
        int index=nearest[n];
        if (index<0 || size_t(index)<=n || !checked[n] || !checked[index] ||
            connected[n] || connected[index] || nearest[index]!=int(n)) continue;
        const auto &a=segments[ends[n].segment],&b=segments[ends[index].segment];
        Finding f; f.code=Gap; f.a=ends[n].pos; f.b=ends[index].pos;
        f.item=a.item; f.source=a.source; f.part=a.part; f.other_item=b.item; f.other_source=b.source; f.other_part=b.part;
        f.value=std::hypot(f.a.x-f.b.x,f.a.y-f.b.y);
        f.detail="Aligned open barrier ends 0.15..2 m apart; may be an intentional opening"; out.add(f);
    }
}
inline std::string text(const Results &r) {
    std::ostringstream out; out << std::setprecision(9)
        << "\nCOLLISION_VALIDATION 1\nFindings are review hints, not confirmed solver contacts. No edits were applied.\n"
        << "Authored segments: " << r.segments << "\nGenerated source boundary segments: " << r.generated_segments << "\nSource candidate faces: " << r.source_faces
        << "\nRoad link tests: " << r.road_tests << "\nEndpoint tests: " << r.endpoint_tests
        << "\nPartial/limited: " << r.limited << "\nOmitted findings: " << r.omitted
        << "\nLimits: 100000 boundary segments; 2000000 road tests; 2000000 endpoint tests; 5000 findings."
        << "\nThresholds: height 0.3..30 m; floating base >1 m; gap 0.15..2 m; gap Z <=0.5 m; "
           "burial >0.15 m and <=2 m to support; normal Z >=0.7; road car band +0.15..1.5 m.\n"
        << "Findings: " << r.findings.size() << '\n';
    for (size_t n=0;n<r.findings.size();n++) {
        const auto &f=r.findings[n];
        out << "\nFinding " << n+1 << ' ' << code_name(f.code) << " item " << f.item << " source 0x"
            << std::hex << std::setw(16) << std::setfill('0') << f.source << std::dec << std::setfill(' ')
            << " part " << f.part << " preview_mesh " << f.mesh << " other_item " << f.other_item << " other_source 0x"
            << std::hex << f.other_source << std::dec << " other_part " << f.other_part
            << " road_link " << f.road << " candidates " << f.count << " value " << f.value
            << "\nXYZ: " << f.a.x << ' ' << f.a.y << ' ' << f.a.z << " -> "
            << f.b.x << ' ' << f.b.y << ' ' << f.b.z << '\n' << f.detail << '\n';
    }
    return out.str();
}
} }
#endif
