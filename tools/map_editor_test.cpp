#include "map_editor_gizmo.h"
#include <cassert>
#include <iostream>
using namespace mapedit;
int main() {
    Document d;
    d.scenery = 4001;
    auto &wall = d.add(Wall, "High wall");
    wall.points = {{0, 0, 2}, {20, 0, 2}};
    wall.values[0] = 8;
    auto &obj = d.add(Object, "Whole object");
    obj.source = 0x1234567890123456ull;
    obj.replacement = 42;
    obj.pos = {4, 2, 1};
    obj.rotation.z = 90;
    auto &path = d.add(Path, "AI street");
    path.points = {{1, 2, 3}, {4, 5, 6}};
    path.source = 43;
    path.values[0] = 72.5f;
    path.values[1] = 1;
    auto path_points = path.points;
    auto &race = d.add(Race, "Sprint \"one\"");
    race.points = path_points;
    race.source = 4001;
    race.values[0] = 2;
    race.values[1] = 1;
    race.values[2] = 3;
    auto &light = d.add(Light, "Lamp");
    light.values[0] = 2;
    light.values[1] = 20;
    light.values[2] = .75f;
    light.color = {.25f, .5f, 1};
    light.source = 0xfedcba9876543210ull;
    d.add(Shop, "Safe house").values[0] = 4;
    auto &region = d.add(Region, "City core");
    region.points = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    region.values[0] = 2;
    std::string error;
    assert(validate(d, error));
    std::string bytes = encode(d);
    Document parsed;
    std::istringstream input(bytes);
    assert(decode(input, parsed, error));
    assert(encode(parsed) == bytes);
    assert(parsed.next == 8);
    assert(parsed.scenery == 4001);
    assert(parsed.items[1].source == 0x1234567890123456ull);
    Document handoff = d;
    for (int k = 0; k < 3; k++) {
        Item extra = handoff.items[0];
        extra.id = handoff.next++;
        extra.source = k < 2 ? 0x1234567890123456ull + k : 0;
        extra.enabled = k == 0;
        handoff.items.push_back(extra);
    }
    std::string report = implementation_report(handoff, Wall, "Synthetic source context\n");
    assert(report.find("Collision records: 4\n") != std::string::npos);
    for (const char *action : {"ADD_WALL", "REPLACE_SOURCE_COLLISION", "DISABLE_SOURCE_COLLISION", "INACTIVE_WALL"})
        assert(report.find(std::string("Action: ") + action + '\n') != std::string::npos);
    assert(report.find("0x1234567890123456") != std::string::npos);
    assert(report.find("Height: 8\n") != std::string::npos);
    assert(report.find("Points: 2\n  0: 0 0 2\n  1: 20 0 2\n") != std::string::npos);
    assert(report.find("Scenery selection: 4001") != std::string::npos);
    const std::string marker = "\nBEGIN_UG2MAP\n", payload = encode(handoff);
    assert(report.find(marker) != std::string::npos);
    size_t start = report.find(marker) + marker.size();
    assert(report.substr(start, payload.size()) == payload);
    std::istringstream embedded(report.substr(start, payload.size()));
    Document imported;
    assert(decode(embedded, imported, error) && encode(imported) == payload);
    std::string project_name = "/tmp/openug2-handoff-" + std::to_string(getpid()) + ".ug2map";
    std::string report_name = report_path(project_name, Wall);
    assert(!report_name.empty() && report_path("map.bin", Wall).empty());
    assert(save_report(report_name, handoff, Wall, "Synthetic source context\n", error));
    Document invalid_report = handoff;
    invalid_report.items[0].values[0] = 0;
    assert(!save_report(report_name, invalid_report, Wall, "", error));
    std::ifstream kept(report_name);
    std::ostringstream stored;
    stored << kept.rdbuf();
    assert(stored.str() == report);
    assert(!save_report(report_name + ".bin", handoff, Wall, "", error));
    assert(!save_report("/no-such-openug2-dir/test.collision.txt", handoff, Wall, "", error));
    unlink(report_name.c_str());
    const char *suffixes[] = {".collision.txt", ".objects.txt", ".ai.txt", ".races.txt",
                             ".lights.txt", ".shops.txt", ".districts.txt", ".edits.txt"};
    const char *actions[] = {"ADD_WALL", "REPLACE_OBJECT", "AUTHOR_AI_PATH", "EDIT_RACE",
                            "EDIT_LIGHT", "PLACE_SHOP", "AUTHOR_DISTRICT"};
    for (int k = Wall; k <= KindCount; k++) {
        Kind category = Kind(k);
        std::string text = implementation_report(handoff, category, "Context\n");
        std::string filename = report_path(project_name, category);
        assert(filename == project_name.substr(0, project_name.size() - 7) + suffixes[k]);
        assert(save_report(filename, handoff, category, "Context\n", error));
        size_t begin = text.find(marker);
        assert(begin != std::string::npos && text.substr(begin + marker.size(), payload.size()) == payload);
        std::string summary = text.substr(0, begin);
        for (int kind = Wall; kind < KindCount; kind++)
            assert((summary.find(std::string("Action: ") + actions[kind] + '\n') != std::string::npos)
                   == (k == KindCount || k == kind));
        std::ifstream saved(filename);
        std::ostringstream contents;contents << saved.rdbuf();
        assert(contents.str() == text);
        assert(!save_report(filename, invalid_report, category, "", error));
        std::ifstream still(filename);std::ostringstream unchanged;unchanged << still.rdbuf();
        assert(unchanged.str() == text);
        unlink(filename.c_str());
    }
    std::string all = implementation_report(handoff, KindCount, "Context\n");
    for (const char *field : {"Replacement source ID: 0x000000000000002a", "Rotation: 0 0 90",
                             "Uniform scale: 1", "Target speed km/h: 72.5\nLoop: 1\nSource start node: 42",
                             "Race type: Sprint\nLaps: 1\nCareer stage: 3", "RGB: 0.25 0.5 1\nPower (0..1): 0.75",
                             "Inner radius: 2\nOuter radius: 20", "Shop category: Purple - safe house",
                             "Unlock stage: 2\nPolygon closes last point to first.", "0xfedcba9876543210"})
        assert(all.find(field) != std::string::npos);
    Document inactive = d;
    for (auto &i : inactive.items) i.enabled = false;
    all = implementation_report(inactive, KindCount, "Context\n");
    for (const char *action : {"HIDE_OBJECT", "INACTIVE_AI_PATH", "INACTIVE_RACE", "DISABLE_SOURCE_LIGHT",
                              "INACTIVE_SHOP", "INACTIVE_DISTRICT"})
        assert(all.find(std::string("Action: ") + action + '\n') != std::string::npos);
    assert(report_path(project_name, -1).empty());
    assert(!save_report(project_name, handoff, -1, "", error));
    assert(!save_report(report_path(project_name, Path), handoff, Race, "", error));
    std::string name = "/tmp/openug2-editor-test-" + std::to_string(getpid()) + ".ug2map";
    assert(save(name, d, error));
    assert(load(name, parsed, error));
    assert(encode(parsed) == bytes);
    for (std::string bad : std::vector<std::string>{bytes.substr(0, bytes.size() - 10),
                                                    bytes + "BAD", "OPENUG2_MAP 9 \"STREAML4RA\" 0",
                                                    "OPENUG2_MAP 1 \"../TRACKS\" -1 0",
                                                    "OPENUG2_MAP 1 \"STREAML4RA\" -2 0"}) {
        Document keep = parsed;
        std::istringstream broken(bad);
        assert(!decode(broken, parsed, error));
        assert(encode(keep) == encode(parsed));
    }
    Document bad = d;
    bad.items[0].values[0] = 0;
    assert(!save(name, bad, error));
    assert(load(name,parsed,error));
    assert(encode(parsed)==bytes);
    unlink(name.c_str());
    bad = d;
    bad.items[1].id = bad.items[0].id;
    assert(!validate(bad, error));
    bad=d;Item duplicate=bad.items[1];duplicate.id=100;bad.items.push_back(duplicate);
    assert(!validate(bad,error));
    Point p = transform({1, 0, 0}, {0, 0, 0}, d.items[1]);
    assert(std::fabs(p.x - 4) < 1e-5 && std::fabs(p.y - 3) < 1e-5 && std::fabs(p.z - 1) < 1e-5);
    using namespace mapedit::gizmo;
    Point hit;
    float t;
    assert(axis_parameter({0,-10,3}, normalized({5,10,-3}), {}, axis(0), t));
    assert(std::fabs(t-5)<1e-4f);
    assert(!axis_parameter({0,-10,3}, axis(0), {}, axis(0), t));
    assert(plane_hit({0,-10,3},normalized({5,10,-3}),{},axis(2),hit));
    assert(std::fabs(hit.x-5)<1e-4f && std::fabs(hit.z)<1e-4f);
    assert(!plane_hit({0,0,3},axis(0),{},axis(2),hit));
    assert(!plane_hit({0,0,3},axis(2),{},axis(2),hit));
    assert(snap(1.6f,1)==2 && snap(-1.6f,1)==-2);
    assert(snap(1.6f,0)==1.6f);
    assert(segment_distance(5,3,0,0,10,0)==3);
    assert(segment_distance(3,4,0,0,0,0)==5);
    for (Point r : {Point{0,0,0}, Point{32,-24,71}, Point{17,90,44}, Point{17,-90,44}})
        for (int a=0;a<3;a++) {
            Item before,after; before.rotation=r;
            after.rotation=rotate_world(r,a,.7f);
            Point expected=rotate_vector(transform({2,3,5},{},before),axis(a),.7f);
            Point actual=transform({2,3,5},{},after);
            assert(dot(sub(actual,expected),sub(actual,expected))<1e-7f);
        }
    Item singular;
    singular.rotation=rotate_world({},1,1.57079632679f);
    Point at=transform({1,0,0},{},singular);
    assert(std::fabs(at.x)<.0001f && std::fabs(at.z+1)<.0001f);
    std::cout << "map editor: all categories, source IDs, atomic save/load, invalid/truncated "
                 "preservation, transforms, gizmo rays/rotation/snapping and all-category handoff PASS\n";
}
