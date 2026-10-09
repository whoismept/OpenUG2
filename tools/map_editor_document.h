#ifndef OPENUG2_MAP_EDITOR_DOCUMENT_H
#define OPENUG2_MAP_EDITOR_DOCUMENT_H
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace mapedit {
struct Point {
    float x = 0, y = 0, z = 0;
};
enum Kind { Wall, Object, Path, Race, Light, Shop, Region, KindCount };
struct Item {
    Kind kind = Wall;
    uint64_t id = 0, source = 0, replacement = 0;
    std::string name;
    bool enabled = true;
    Point pos, rotation, color{1, 1, 1};
    float scale = 1, values[4] = {0, 0, 0, 0};
    std::vector<Point> points;
};
struct Document {
    std::string map = "STREAML4RA";
    int scenery = -1;
    std::vector<Item> items;
    uint64_t next = 1;
    Item &add(Kind k, const std::string &name) {
        Item i;
        i.kind = k;
        i.id = next++;
        i.name = name;
        items.push_back(i);
        return items.back();
    }
};
inline const char *kind_name(Kind k) {
    static const char *names[] = {"Collision", "Objects", "AI paths", "Races & icons",
                                  "Lights",    "Shops",   "Districts"};
    return k >= Wall && k < KindCount ? names[k] : "?";
}
inline bool finite(Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::fabs(p.x) < 100000 && std::fabs(p.y) < 100000 && std::fabs(p.z) < 100000;
}
inline bool validate(const Document &d, std::string &error) {
    if (d.scenery < -1 || d.scenery > 65535) {
        error = "Invalid scenery event";
        return false;
    }
    if (d.map.empty() || d.map.size() > 63 ||
        d.map.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
            std::string::npos) {
        error = "Invalid map name";
        return false;
    }
    if (d.items.size() > 10000) {
        error = "Too many editor items";
        return false;
    }
    std::vector<uint64_t> ids;
    std::vector<std::pair<int, uint64_t>> sources;
    size_t points = 0;
    for (const auto &i : d.items) {
        if (!i.id || i.kind < Wall || i.kind >= KindCount || i.name.size() > 255 ||
            !finite(i.pos) || !finite(i.rotation) || !finite(i.color) || !std::isfinite(i.scale) ||
            i.scale <= 0 || i.scale > 100) {
            error = "Invalid item fields";
            return false;
        }
        for (float v : i.values)
            if (!std::isfinite(v) || std::fabs(v) > 100000) {
                error = "Invalid item parameters";
                return false;
            }
        for (Point p : i.points)
            if (!finite(p)) {
                error = "Invalid point";
                return false;
            }
        points += i.points.size();
        if (points > 1000000) {
            error = "Too many points";
            return false;
        }
        if (i.kind == Wall && (i.points.size() < 2 || i.values[0] <= 0 || i.values[0] > 1000)) {
            error = "Walls need endpoints and positive height";
            return false;
        }
        if (i.kind == Object && !i.source) {
            error = "Object has no source identity";
            return false;
        }
        if ((i.kind == Path || i.kind == Race) && i.points.size() < 2) {
            error = "Routes need at least two points";
            return false;
        }
        if (i.kind == Race &&
            (i.values[0] < 1 || i.values[0] > 6 || i.values[1] < 1 || i.values[1] > 99 ||
             i.values[0] != std::floor(i.values[0]) || i.values[1] != std::floor(i.values[1]))) {
            error = "Invalid race type or laps";
            return false;
        }
        if (i.kind == Light && (i.values[0] < 0 || i.values[1] < i.values[0] ||
                                i.values[1] > 1000 || i.values[2] < 0 || i.values[2] > 1)) {
            error = "Invalid light radius";
            return false;
        }
        if (i.kind == Shop &&
            (i.values[0] < 0 || i.values[0] > 4 || i.values[0] != std::floor(i.values[0]))) {
            error = "Invalid shop category";
            return false;
        }
        if (i.kind == Region && i.points.size() < 3) {
            error = "Districts need at least three corners";
            return false;
        }
        ids.push_back(i.id);
        if (i.source && (i.kind == Object || i.kind == Light || i.kind == Wall))
            sources.push_back({i.kind, i.source});
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        error = "Duplicate item identity";
        return false;
    }
    std::sort(sources.begin(), sources.end());
    if (std::adjacent_find(sources.begin(), sources.end()) != sources.end()) {
        error = "Conflicting edits to one source object";
        return false;
    }
    error.clear();
    return true;
}
inline std::string encode(const Document &d) {
    std::ostringstream out;
    out << std::setprecision(9) << "OPENUG2_MAP 1 " << std::quoted(d.map) << ' ' << d.scenery << ' '
        << d.items.size() << '\n';
    for (const auto &i : d.items) {
        out << "ITEM " << int(i.kind) << ' ' << i.id << ' ' << i.source << ' ' << i.replacement
            << ' ' << int(i.enabled) << ' ' << std::quoted(i.name) << '\n';
        out << i.pos.x << ' ' << i.pos.y << ' ' << i.pos.z << ' ' << i.rotation.x << ' '
            << i.rotation.y << ' ' << i.rotation.z << ' ' << i.scale << ' ' << i.color.x << ' '
            << i.color.y << ' ' << i.color.z;
        for (float v : i.values)
            out << ' ' << v;
        out << ' ' << i.points.size() << '\n';
        for (Point p : i.points)
            out << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    return out.str();
}
inline bool decode(std::istream &in, Document &out, std::string &error) {
    Document d;
    std::string tag;
    int version = 0;
    long long count = 0;
    if (!(in >> tag >> version >> std::quoted(d.map) >> d.scenery >> count) ||
        tag != "OPENUG2_MAP" || version != 1 || count < 0 || count > 10000) {
        error = "Unsupported or malformed editor project";
        return false;
    }
    size_t total = 0;
    for (long long n = 0; n < count; n++) {
        Item i;
        int k = -1, on = -1;
        long long np = -1;
        if (!(in >> tag >> k >> i.id >> i.source >> i.replacement >> on >> std::quoted(i.name)) ||
            tag != "ITEM" || k < 0 || k >= KindCount || (on != 0 && on != 1)) {
            error = "Malformed item header";
            return false;
        }
        i.kind = Kind(k);
        i.enabled = on;
        if (!(in >> i.pos.x >> i.pos.y >> i.pos.z >> i.rotation.x >> i.rotation.y >> i.rotation.z >>
              i.scale >> i.color.x >> i.color.y >> i.color.z >> i.values[0] >> i.values[1] >>
              i.values[2] >> i.values[3] >> np) ||
            np < 0 || np > 1000000 || total + size_t(np) > 1000000) {
            error = "Malformed item values";
            return false;
        }
        total += size_t(np);
        i.points.resize(size_t(np));
        for (auto &p : i.points)
            if (!(in >> p.x >> p.y >> p.z)) {
                error = "Truncated point list";
                return false;
            }
        if (i.id == UINT64_MAX) {
            error = "Item identity exhausted";
            return false;
        }
        d.next = std::max(d.next, i.id + 1);
        d.items.push_back(i);
    }
    in >> std::ws;
    if (!in.eof()) {
        error = "Unexpected trailing data";
        return false;
    }
    if (!validate(d, error))
        return false;
    out = std::move(d);
    return true;
}
inline bool load(const std::string &path, Document &d, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "Cannot read project: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    auto bytes = in.tellg();
    in.seekg(0);
    if (bytes < 0 || bytes > 64 * 1024 * 1024) {
        error = "Project exceeds 64 MB";
        return false;
    }
    return decode(in, d, error);
}
inline bool write_atomic(const std::string &path, const std::string &bytes, std::string &error) {
    std::string tmp = path + ".tmp.XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0) {
        error = "Cannot create file: " + std::string(strerror(errno));
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        written += size_t(n);
    }
    if (ok && fsync(fd))
        ok = false;
    if (close(fd))
        ok = false;
    if (ok && rename(name.data(), path.c_str()))
        ok = false;
    if (!ok) {
        unlink(name.data());
        error = "File write failed: " + std::string(strerror(errno));
        return false;
    }
    error.clear();
    return true;
}
inline bool save(const std::string &path, const Document &d, std::string &error) {
    if (!validate(d, error))
        return false;
    if (path.size() < 7 || path.substr(path.size() - 7) != ".ug2map") {
        error = "Use a .ug2map filename";
        return false;
    }
    return write_atomic(path, encode(d), error);
}
static const char *const race_names[] = {"Circuit", "Sprint", "Drag", "Drift", "Street X", "URL"};
static const char *const shop_names[] = {"Green - body parts", "Yellow - accessories", "Red - paint / vinyl",
                                       "Blue - performance", "Purple - safe house"};
inline const char *report_suffix(int category) {
    static const char *const suffixes[] = {".collision.txt", ".objects.txt", ".ai.txt", ".races.txt",
                                          ".lights.txt", ".shops.txt", ".districts.txt", ".edits.txt"};
    return category >= Wall && category <= KindCount ? suffixes[category] : "";
}
inline std::string report_path(const std::string &project, int category) {
    return project.size() >= 7 && project.substr(project.size() - 7) == ".ug2map"
               && category >= Wall && category <= KindCount
               ? project.substr(0, project.size() - 7) + report_suffix(category)
               : "";
}
inline std::string implementation_report(const Document &d, int category, const std::string &sources) {
    std::ostringstream out;
    std::string project = encode(d);
    out << std::setprecision(9) << "OPENUG2_EDITOR_REPORT 1\nCategory: "
        << (category == KindCount ? "All edits" : category >= Wall && category < KindCount ? kind_name(Kind(category)) : "?")
        << "\nMap: " << d.map
        << "\nScenery selection: " << d.scenery
        << " (-1 free roam, 0 unfiltered, positive source event)\n"
           "Coordinates: world XYZ in metres, Z up. Rotations are degrees.\n"
           "Snapshot: complete editor project, not a list of changes since the previous export.\n"
           "Status: editor preview only; implementation and validation in the game are still required.\n"
           "Scope: verify map, scenery selection and original source fingerprints before implementation.\n"
           "Review focus: category above; other categories remain in the full project for context.\n"
           "AI source IDs identify a copied start node, not an entire shared road chain.\n"
           "Inactive AI/race/district edits do not themselves disable the shared source graph/event/district.\n"
           "Source boundary overrides keep the visible model and suppress original body-wall collision; driving ground support is retained.\n"
           "Separate object overrides are included in the project payload and must also be reviewed.\n";
    for (int k = Wall; k < KindCount; k++)
        if (category == KindCount || category == k)
            out << kind_name(Kind(k)) << " records: "
                << std::count_if(d.items.begin(), d.items.end(),
                                 [k](const Item &i) { return i.kind == k; }) << '\n';
    for (const auto &i : d.items)
        if (category == KindCount || i.kind == category) {
            const char *action = "";
            switch (i.kind) {
            case Wall: action = i.source ? (i.enabled ? "REPLACE_SOURCE_COLLISION" : "DISABLE_SOURCE_COLLISION")
                                        : (i.enabled ? "ADD_WALL" : "INACTIVE_WALL"); break;
            case Object: action = !i.enabled ? "HIDE_OBJECT" : i.replacement ? "REPLACE_OBJECT" : "TRANSFORM_OBJECT"; break;
            case Path: action = i.enabled ? "AUTHOR_AI_PATH" : "INACTIVE_AI_PATH"; break;
            case Race: action = !i.enabled ? "INACTIVE_RACE" : i.source ? "EDIT_RACE" : "ADD_RACE"; break;
            case Light: action = i.source ? (i.enabled ? "EDIT_LIGHT" : "DISABLE_SOURCE_LIGHT")
                                         : (i.enabled ? "ADD_LIGHT" : "INACTIVE_LIGHT"); break;
            case Shop: action = i.enabled ? "PLACE_SHOP" : "INACTIVE_SHOP"; break;
            case Region: action = i.enabled ? "AUTHOR_DISTRICT" : "INACTIVE_DISTRICT"; break;
            default: break;
            }
            out << "\n" << kind_name(i.kind) << " item " << i.id << ' ' << std::quoted(i.name) << "\nAction: " << action
                << "\nEnabled: " << int(i.enabled) << "\nSource ID: " << i.source << " (0x"
                << std::hex << std::setw(16) << std::setfill('0') << i.source << std::dec
                << std::setfill(' ') << ")\n";
            if (i.kind != Wall && i.kind != Path)
                out << "Position: " << i.pos.x << ' ' << i.pos.y << ' ' << i.pos.z << '\n';
            switch (i.kind) {
            case Wall: out << "Height: " << i.values[0] << "\nBase points; height extends upward.\n"; break;
            case Object:
                out << "Replacement source ID: 0x" << std::hex << std::setw(16) << std::setfill('0')
                    << i.replacement << std::dec << std::setfill(' ')
                    << "\nRotation: " << i.rotation.x << ' ' << i.rotation.y << ' ' << i.rotation.z
                    << "\nUniform scale: " << i.scale << '\n'; break;
            case Path:
                out << "Target speed km/h: " << i.values[0] << "\nLoop: " << int(i.values[1] > .5f)
                    << "\nSource start node: " << (i.source ? std::to_string(i.source - 1) : "custom path") << '\n'; break;
            case Race:
                out << "Race type: " << race_names[int(i.values[0]) - 1] << "\nLaps: " << i.values[1]
                    << "\nCareer stage: " << i.values[2] << "\nPosition is the map icon/start location.\n"; break;
            case Light:
                out << "RGB: " << i.color.x << ' ' << i.color.y << ' ' << i.color.z
                    << "\nPower (0..1): " << i.values[2] << "\nInner radius: " << i.values[0]
                    << "\nOuter radius: " << i.values[1] << '\n'; break;
            case Shop: out << "Shop category: " << shop_names[int(i.values[0])] << '\n'; break;
            case Region: out << "Unlock stage: " << i.values[0] << "\nPolygon closes last point to first.\n"; break;
            default: break;
            }
            out << "Points: " << i.points.size() << '\n';
            for (size_t n = 0; n < i.points.size(); n++) {
                Point p = i.points[n];
                out << "  " << n << ": " << p.x << ' ' << p.y << ' ' << p.z << '\n';
            }
        }
    out << "\nOriginal source context:\n" << sources
        << "\nComplete project bytes: " << project.size()
        << "\nBEGIN_UG2MAP\n" << project << "END_UG2MAP\n";
    return out.str();
}
inline bool save_report(const std::string &path, const Document &d, int category,
                        const std::string &sources, std::string &error) {
    if (!validate(d, error))
        return false;
    const std::string suffix = report_suffix(category);
    if (suffix.empty() || path.size() < suffix.size() || path.substr(path.size() - suffix.size()) != suffix) {
        error = "Use the report filename for the selected category";
        return false;
    }
    return write_atomic(path, implementation_report(d, category, sources), error);
}
inline Point transform(Point p, Point origin, const Item &i) {
    float a = i.rotation.x * .01745329252f, b = i.rotation.y * .01745329252f,
          c = i.rotation.z * .01745329252f;
    p.x = (p.x - origin.x) * i.scale;
    p.y = (p.y - origin.y) * i.scale;
    p.z = (p.z - origin.z) * i.scale;
    float y = p.y * std::cos(a) - p.z * std::sin(a), z = p.y * std::sin(a) + p.z * std::cos(a);
    p.y = y;
    p.z = z;
    float x = p.x * std::cos(b) + p.z * std::sin(b);
    z = -p.x * std::sin(b) + p.z * std::cos(b);
    p.x = x;
    p.z = z;
    x = p.x * std::cos(c) - p.y * std::sin(c);
    y = p.x * std::sin(c) + p.y * std::cos(c);
    return {x + i.pos.x, y + i.pos.y, p.z + i.pos.z};
}
} // namespace mapedit
#endif
