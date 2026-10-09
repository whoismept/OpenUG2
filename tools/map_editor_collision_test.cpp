#include "map_editor_collision.h"
#include <cassert>
#include <iostream>
using namespace mapedit;
using namespace mapedit::collision;
static bool has(const Results &r,Code code) {
    return std::any_of(r.findings.begin(),r.findings.end(),[&](const Finding &f) { return f.code==code; });
}
int main() {
    Ground road=[](Point) { return Surface{1,0,1}; };
    Ground none=[](Point) { return Surface{}; };
    Ground upper=[](Point) { return Surface{1,10,1}; };
    RoadIndex roads; roads.add({{-10,0,0},{10,0,0},73});
    Segment wall{{0,-4,0},{0,4,0},8,12,0,0};
    Results crossed; roads.check(wall,road,crossed);
    assert(has(crossed,RoadCrossing));
    assert(crossed.findings[0].road==73 && crossed.findings[0].item==12);
    Results r; roads.check(wall,upper,r); assert(!has(r,RoadCrossing));
    roads.check(wall,none,r); assert(r.findings.empty());
    Ground terrain=[](Point) { return Surface{2,0,1}; };
    roads.check(wall,terrain,r); assert(r.findings.empty());
    Segment overhead=wall; overhead.a.z=overhead.b.z=3;
    roads.check(overhead,road,r); assert(r.findings.empty());
    Segment along{{-5,0,0},{5,0,0},8,13,0,0};
    roads.check(along,road,r); assert(r.findings.empty());
    Segment endpoint{{0,0,0},{0,4,0},8,14,0,0};
    roads.check(endpoint,road,r); assert(r.findings.empty());
    // Negative cell coordinates and graph-node crossings must be indexed too.
    RoadIndex node; node.add({{-40,-40,0},{-32,-40,0},74}); node.add({{-32,-40,0},{-24,-40,0},75});
    Segment negative{{-32,-45,0},{-32,-35,0},8,15,0,0};
    node.check(negative,road,r); assert(has(r,RoadCrossing));
    // Source triangles need their vertical range at the crossing, not their global Z bounds.
    Segment triangle{{0,-4,0},{0,4,10},10,0,90,2}; triangle.triangle=true;
    triangle.face[0]={0,-4,0}; triangle.face[1]={0,4,10}; triangle.face[2]={0,-4,1};
    r={}; roads.check(triangle,road,r); assert(!has(r,RoadCrossing));
    triangle.face[1].z=0; triangle.b.z=0;
    roads.check(triangle,road,r); assert(has(r,RoadCrossing));
    Segment buried_wall{{0,-1,-1},{0,1,-1},.5f,16,0,0};
    r={}; inspect(buried_wall,road,r); assert(has(r,Buried));
    inspect(wall,road,r); assert(r.findings.size()==1);
    r={}; inspect(buried_wall,none,r); assert(!has(r,Buried));
    inspect(buried_wall,upper,r); assert(!has(r,Buried));
    Ground slope=[](Point) { return Surface{1,0,.5f}; };
    inspect(buried_wall,slope,r); assert(!has(r,Buried));
    Segment floating=wall; floating.a.z=floating.b.z=2;
    inspect(floating,road,r); assert(has(r,Height));
    Segment small=wall; small.height=.1f;
    r={}; inspect(small,road,r); assert(has(r,Height));
    small.height=40; inspect(small,road,r); assert(r.findings.size()==2);
    small.height=8; small.b=small.a; inspect(small,road,r); assert(has(r,ZeroLength));
    std::vector<Segment> barriers={{{0,0,0},{10,0,0},8,20,0,0},{{11,0,0},{20,0,0},8,21,0,0}};
    r={}; gaps(barriers,r); assert(r.findings.size()==1 && has(r,Gap));
    assert(r.findings[0].other_item==21 && r.findings[0].value==1);
    auto closed=barriers; closed[1].a.x=10;
    r={}; gaps(closed,r); assert(r.findings.empty());
    auto deck=barriers; deck[1].a.z=deck[1].b.z=10;
    gaps(deck,r); assert(r.findings.empty());
    auto corner=barriers; corner[1].b={11,10,0};
    gaps(corner,r); assert(r.findings.empty());
    auto tee=barriers; tee.push_back({{10,-5,0},{10,5,0},8,22,0,0});
    gaps(tee,r); assert(r.findings.empty());
    auto loop=std::vector<Segment>{{{0,0,0},{10,0,0},8,23,0,0},{{10,0,0},{10,10,0},8,23,0,1},
                                  {{10,10,0},{0,10,0},8,23,0,2},{{0,10,0},{0,0,0},8,23,0,3}};
    gaps(loop,r); assert(r.findings.empty());
    // Completed authored findings survive a later dense-source work limit.
    auto dense=barriers;
    for (int n=0;n<2000;n++) dense.push_back({{100,100,0},{110,100,0},8,0,uint64_t(n+100),0});
    r={}; gaps(dense,r); assert(r.limited && has(r,Gap));
    Finding source; source.source=0xfedcba9876543210ull; source.code=Buried; source.detail="Synthetic";
    r={}; r.add(source); r.add(source); assert(r.findings.size()==1 && r.findings[0].count==2);
    std::string text=collision::text(r);
    assert(text.find("0xfedcba9876543210")!=std::string::npos && text.find("candidates 2")!=std::string::npos);
    assert(text.find("Partial/limited: 0")!=std::string::npos);
    assert(text.find("2000000 endpoint tests; 5000 findings")!=std::string::npos);
    Results limited;
    for (int n=0;n<5001;n++) { Finding f; f.item=n+1; limited.add(f); }
    assert(limited.limited && limited.omitted==1 && limited.findings.size()==5000);
    assert(collision::text(limited).find("Partial/limited: 1")!=std::string::npos);
    source.source=99;
    limited.add(source); limited.add(source);
    assert(limited.omitted==2);
    limited={}; limited.road_tests=2000001; roads.check(wall,road,limited);
    assert(limited.limited && limited.findings.empty());
    // Long-coordinate inputs use the bounded fallback, not millions of grid cells.
    RoadIndex wide; wide.add({{-99999,-99999,0},{99999,99999,0},80});
    assert(wide.cells.empty() && wide.wide.size()==1);
    std::cout << "collision validation: crossing/layers, triangle Z clipping, burial, heights, "
                 "aligned gaps/closed joins/T joins, source grouping and work limits PASS\n";
}
