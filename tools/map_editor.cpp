/* Standalone authoring tool. Game archives are read-only; edits are .ug2map projects. */
#include "map_editor_collision.h"
extern "C" {
#include "ai.h"
#include "resource.h"
#include "world.h"
}
#include "backends/imgui_impl_opengl2.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include <limits>
#include <memory>
#include <unordered_map>

using namespace mapedit;
static Point point(const float *v) { return {v[0], v[1], v[2]}; }
using namespace mapedit::gizmo;
static float ray_triangle(Point origin, Point ray, Point a, Point b, Point c) {
    Point e1 = sub(b, a), e2 = sub(c, a), h = cross(ray, e2);
    float det = dot(e1, h);
    if (std::fabs(det) < 1e-7f) return INFINITY;
    Point s = sub(origin, a), q = cross(s, e1);
    float v = dot(s, h) / det, w = dot(ray, q) / det;
    if (v < 0 || w < 0 || v + w > 1) return INFINITY;
    float t = dot(e2, q) / det;
    return t > .05f ? t : INFINITY;
}
static uint64_t hash_bytes(uint64_t h, const void *ptr, size_t n) {
    const auto *b = (const unsigned char *)ptr;
    while (n--) {
        h ^= *b++;
        h *= 1099511628211ull;
    }
    return h;
}
static uint64_t mesh_id(const N2Mesh &m) {
    // shortcut: placement IDs target one archive set; add version tags before cross-edition sharing.
    return n2_world_source_id(&m);
}
static uint64_t light_id(const N2LightSrc &l) {
    return hash_bytes(1469598103934665603ull, &l, sizeof l);
}
struct SourceObject {
    uint64_t id;
    std::string name;
    Point lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    std::vector<int> meshes;
    Point center() const { return mul(add(lo, hi), .5f); }
};
struct Editor {
    Document doc;
    std::vector<Document> undo, redo;
    bool dirty = false, preview_dirty = false;
    std::string error, status = "Ready";
    std::unique_ptr<World> world;
    std::vector<SourceObject> objects, preview_objects;
    std::unordered_map<uint64_t, int> object_index;
    std::vector<N2Mesh> meshes;
    std::vector<uint64_t> mesh_sources;
    std::vector<std::vector<float>> verts;
    std::vector<float> bounds;
    std::vector<GLuint> textures, source_textures;
    std::vector<unsigned char> modes, source_modes;
    std::vector<N2LightSrc> lights;
    std::vector<Point> navpoints;
    N2Batch *batches = nullptr, *glows = nullptr, *sky = nullptr;
    int nbatch = 0, nglow = 0, nsky = 0;
    RProg shader{};
    GLuint wallvbo = 0, flare = 0;
    GpuMesh quad{};
    char root[1024] = "../TRACKS", file[1024] = "map-edits.ug2map", filter[128] = "";
    char tracks[128][64]{};
    int ntracks = 0, track = 0, scene_event = -1;
    Point camera{-90, 64, 10};
    float yaw = 1.1f, pitch = -.14f, speed = 30, range = 350;
    float mvp[16]{}, fov = 65;
    int width = 1440, height = 900;
    static constexpr float toolbar_height = 56, status_height = 44;
    float panel_width() const { return std::min(350.f, width * .26f); }
    ImVec2 view_min() const { return {panel_width(), toolbar_height}; }
    ImVec2 view_max() const { return {width - panel_width(), height - status_height}; }
    ImVec2 view_size() const {
        return {std::max(1.f, view_max().x - view_min().x),
                std::max(1.f, view_max().y - view_min().y)};
    }
    bool in_view(ImVec2 p) const {
        return p.x >= view_min().x && p.x < view_max().x &&
               p.y >= view_min().y && p.y < view_max().y;
    }
    bool walls = true, through = true, paths = true, source_lights = false, show_regions = true;
    bool pick_replacement = false;
    collision::Results collision_results;
    collision::RoadIndex collision_roads;
    std::vector<collision::Segment> collision_segments;
    std::set<uint64_t> collision_replaced;
    WGroundGrid collision_grid{};
    N2Scene collision_scene{};
    bool collision_stale = true, collision_running = false, collision_whole = false, show_findings = true;
    float collision_radius = 80;
    Point collision_center;
    size_t collision_mesh = 0, collision_segment = 0;
    bool collision_gaps = false;
    int collision_triangle = 0, selected_finding = -1, pending_report = -1;
    double collision_ms = 0;
    int gizmo_mode = 0;
    bool grid_snap = false, ground_snap = false, gizmo_point = true;
    float grid_step = 1;
    struct Drag {
        bool active = false, dirty = false;
        int mode = 0, axis = 0, point = -1;
        uint64_t selected = 0;
        float start = 0, length = 1, angle = 0;
        Point pivot, hit, radial;
        ImVec2 mouse;
        Document before;
        Item original;
    } drag;
    Kind tab = Wall;
    uint64_t selected = 0, picked_object = 0;
    int selected_point = 0, event_index = 0, navnode = -1, light_index = 0, region_index = 0;
    int pending = 0, pending_track = 0, pending_event = -1;
    bool ask = false, pending_discard = false;
    void checkpoint() { checkpoint(doc); }
    void checkpoint(const Document &before) {
        invalidate_collision();
        undo.push_back(before);
        if (undo.size() > 32)
            undo.erase(undo.begin());
        redo.clear();
        dirty = true;
    }
    Item *item() {
        for (auto &i : doc.items)
            if (i.id == selected)
                return &i;
        return nullptr;
    }
    void changed() {
        invalidate_collision();
        preview_dirty = true;
        dirty = true;
    }
    void release() {
        invalidate_collision();
        render_batch_array_free(&batches, &nbatch);
        render_batch_array_free(&glows, &nglow);
        render_batch_array_free(&sky, &nsky);
        if (world) {
            world_ground_grid_activate(nullptr);
            world_neighborhood_free(&world->neighborhood);
            world_city_free(&world->city);
            world.reset();
        }
        meshes.clear();
        verts.clear();
        objects.clear();
        object_index.clear();
        world_texture_cache_clear();
    }
    void sources() {
        objects.clear();
        object_index.clear();
        const auto &s = world->neighborhood.scene;
        for (int k = 0; k < s.count; k++) {
            const auto &m = s.meshes[k];
            if (!m.verts || !m.idx || m.cat == N2_SKY)
                continue;
            uint64_t id = mesh_id(m);
            auto found = object_index.find(id);
            int at;
            if (found == object_index.end()) {
                at = int(objects.size());
                SourceObject o;
                o.id = id;
                o.name = m.aname[0] ? m.aname : m.sname;
                if (o.name.empty())
                    o.name = "Unnamed mesh";
                objects.push_back(o);
                object_index[id] = at;
            } else
                at = found->second;
            auto &o = objects[at];
            o.meshes.push_back(k);
            for (int v = 0; v < m.nverts; v++) {
                Point p = point(m.verts + 5 * v);
                o.lo = {std::min(o.lo.x, p.x), std::min(o.lo.y, p.y), std::min(o.lo.z, p.z)};
                o.hi = {std::max(o.hi.x, p.x), std::max(o.hi.y, p.y), std::max(o.hi.z, p.z)};
            }
        }
    }
    bool load_map(const std::string &map, int event = -1) {
        auto next = std::unique_ptr<World>(new World{});
        WLoadOptions opt{1, camera.x, camera.y, 12000, event};
        if (world_load_ex(next.get(), root, map.c_str(), &opt) <= 0) {
            world_neighborhood_free(&next->neighborhood);
            world_city_free(&next->city);
            if (world)
                world_ground_grid_activate(&world->neighborhood.grid);
            error = "Map could not be loaded; current project retained";
            return false;
        }
        release();
        world = std::move(next);
        world_ground_grid_activate(&world->neighborhood.grid);
        navpoints.clear();
        for (int n = 0; n < world->city.nnav; n++)
            navpoints.push_back(
                ground({world->city.nav[2 * n], world->city.nav[2 * n + 1], camera.z}));
        scene_event = event;
        sources();
        uint32_t keys[4096];
        GLuint tex[4096];
        unsigned char mode[4096];
        int count = world_bind_textures(world.get(), keys, tex, mode, 4096);
        if (count < 0) {
            error = "Texture upload failed; project is still retained";
            return false;
        }
        source_textures.assign(world->neighborhood.scene.count, 0);
        source_modes.assign(source_textures.size(), 0);
        flare = 0;
        std::unordered_map<uint32_t, int> binding;
        for (int k = 0; k < count; k++) {
            binding[keys[k]] = k;
            if (keys[k] == N2_TEX_SFX_FLARE_GLOWA)
                flare = tex[k];
        }
        for (int k = 0; k < world->neighborhood.scene.count; k++) {
            const auto &m = world->neighborhood.scene.meshes[k];
            auto it = binding.find(m.texkey);
            if (it != binding.end()) {
                source_textures[k] = tex[it->second];
                source_modes[k] = (unsigned char)n2_world_draw_mode(&m, mode[it->second]);
            }
        }
        status = "Loaded " + map + " (" + std::to_string(objects.size()) + " objects)";
        picked_object = 0;
        navnode = -1;
        event_index = 0;
        light_index = 0;
        region_index = 0;
        preview_dirty = true;
        return rebuild();
    }
    bool rebuild() {
        invalidate_collision();
        if (!world)
            return false;
        if (!validate(doc, error))
            return false;
        const auto &src = world->neighborhood.scene;
        meshes.assign(src.meshes, src.meshes + src.count);
        mesh_sources.resize(src.count);
        for (const auto &o : objects)
            for (int k : o.meshes)
                mesh_sources[k] = o.id;
        verts.clear();
        verts.resize(src.count);
        textures = source_textures;
        modes = source_modes;
        preview_objects = objects;
        int missing = 0;
        for (const auto &i : doc.items)
            if (i.kind == Object) {
                auto at = object_index.find(i.source);
                if (at == object_index.end()) {
                    missing++;
                    continue;
                }
                const auto &target = objects[at->second];
                auto &preview = preview_objects[at->second];
                preview.meshes.clear();
                preview.lo = {INFINITY, INFINITY, INFINITY};
                preview.hi = {-INFINITY, -INFINITY, -INFINITY};
                for (int k : target.meshes)
                    meshes[k].nidx = 0;
                if (!i.enabled)
                    continue;
                const SourceObject *donor = &target;
                if (i.replacement) {
                    auto it = object_index.find(i.replacement);
                    if (it == object_index.end()) {
                        missing++;
                        continue;
                    }
                    donor = &objects[it->second];
                }
                for (size_t slice = 0; slice < donor->meshes.size(); slice++) {
                    int original = donor->meshes[slice], destination;
                    if (slice < target.meshes.size())
                        destination = target.meshes[slice];
                    else {
                        destination = int(meshes.size());
                        meshes.push_back({});
                        verts.emplace_back();
                        textures.push_back(0);
                        modes.push_back(0);
                        mesh_sources.push_back(i.source);
                    }
                    N2Mesh m = src.meshes[original];
                    auto &buffer = verts[destination];
                    buffer.assign(m.verts, m.verts + 5 * m.nverts);
                    for (int v = 0; v < m.nverts; v++) {
                        Point p = transform(point(m.verts + 5 * v), donor->center(), i);
                        buffer[5 * v] = p.x;
                        buffer[5 * v + 1] = p.y;
                        buffer[5 * v + 2] = p.z;
                    }
                    m.verts = buffer.data();
                    m.authored_normals = 0;
                    m.prop_id = 1;
                    m.prop_revision = 0;
                    m.wall_verts = nullptr;
                    m.wall_idx = nullptr;
                    m.wall_nverts = m.wall_nidx = 0;
                    meshes[destination] = m;
                    mesh_sources[destination] = i.source;
                    textures[destination] = source_textures[original];
                    modes[destination] = source_modes[original];
                    preview.meshes.push_back(destination);
                    for (int v = 0; v < m.nverts; v++) {
                        Point p = point(m.verts + 5 * v);
                        preview.lo = {std::min(preview.lo.x, p.x), std::min(preview.lo.y, p.y),
                                      std::min(preview.lo.z, p.z)};
                        preview.hi = {std::max(preview.hi.x, p.x), std::max(preview.hi.y, p.y),
                                      std::max(preview.hi.z, p.z)};
                    }
                }
            }
        missing += rebuild_lights();
        N2Scene view{};
        view.meshes = meshes.data();
        view.count = int(meshes.size());
        bounds.resize(meshes.size() * 4);
        for (size_t k = 0; k < meshes.size(); k++) {
            const auto &m = meshes[k];
            float *b = bounds.data() + 4 * k;
            b[0] = b[2] = INFINITY;
            b[1] = b[3] = -INFINITY;
            for (int v = 0; v < m.nverts; v++) {
                const float *p = m.verts + 5 * v;
                b[0] = std::min(b[0], p[0]);
                b[1] = std::max(b[1], p[0]);
                b[2] = std::min(b[2], p[1]);
                b[3] = std::max(b[3], p[1]);
            }
        }
        N2Batch *next = nullptr;
        int count = upload_world_batches(&view, (const float (*)[4])bounds.data(), textures.data(),
                                         0, &next, nullptr, nullptr, modes.data());
        if (count < 0) {
            error = "Could not rebuild map preview";
            return false;
        }
        render_batch_array_free(&batches, &nbatch);
        batches = next;
        nbatch = count;
        render_batch_array_free(&glows, &nglow);
        nglow = upload_cat_batches(&view, N2_GLOW, textures.data(), &glows, nullptr);
        render_batch_array_free(&sky, &nsky);
        nsky = upload_cat_batches(&view, N2_SKY, textures.data(), &sky, nullptr);
        preview_dirty = false;
        if (missing)
            error = std::to_string(missing) + " source references missing in this map/scenery";
        else
            error.clear();
        return true;
    }
    void focus(Point p) {
        camera = add(p, {-18, -18, 12});
        yaw = .7854f;
        pitch = -.35f;
    }
    bool project_load() {
        Document next;
        if (!mapedit::load(file, next, error))
            return false;
        bool reload = next.map != doc.map || next.scenery != scene_event || !world;
        Document previous = doc;
        doc = std::move(next);
        if (reload && !load_map(doc.map, doc.scenery)) {
            doc = std::move(previous);
            return false;
        }
        if (!reload && !rebuild()) {
            doc = std::move(previous);
            return false;
        }
        selected = 0;
        undo.clear();
        redo.clear();
        dirty = false;
        preview_dirty = true;
        status = "Project loaded";
        return true;
    }
    bool project_save() {
        if (!mapedit::save(file, doc, error))
            return false;
        dirty = false;
        status = "Project saved";
        return true;
    }
    bool report_export(Kind category) {
        std::string path = report_path(file, category);
        if (path.empty()) {
            error = "Set a .ug2map project filename before exporting";
            return false;
        }
        if (!validate(doc, error))
            return false;
        if ((category == Wall || category == KindCount) && collision_stale) {
            if (!start_collision_scan()) return false;
            while (collision_running) collision_step();
        }
        std::vector<uint64_t> ids;
        for (const auto &i : doc.items)
            if ((category == KindCount || category == i.kind) && (i.kind == Wall || i.kind == Object)) {
                if (i.source) ids.push_back(i.source);
                if (i.kind == Object && i.replacement) ids.push_back(i.replacement);
            }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::ostringstream sources;
        sources << std::setprecision(9)
                << "Fingerprint: FNV1a64 of original slices in order: texture key, vertex/index counts, vertices, indices.\n";
        int missing = 0;
        for (uint64_t id : ids) {
            sources << "\nSource 0x" << std::hex << std::setw(16) << std::setfill('0') << id
                    << std::dec << std::setfill(' ');
            auto at = object_index.find(id);
            if (at == object_index.end()) {
                sources << " MISSING in current map/scenery; resolve before implementation\n";
                missing++;
                continue;
            }
            const auto &o = objects[at->second];
            sources << ' ' << std::quoted(o.name) << "\nOriginal bounds: "
                    << o.lo.x << ' ' << o.lo.y << ' ' << o.lo.z << " -> "
                    << o.hi.x << ' ' << o.hi.y << ' ' << o.hi.z
                    << "\nMaterial slices: " << o.meshes.size() << '\n';
            uint64_t hash = 14695981039346656037ull;
            for (int k : o.meshes) {
                const auto &m = world->neighborhood.scene.meshes[k];
                sources << "  " << std::quoted(m.sname) << " texture " << m.texkey
                        << " vertices " << m.nverts << " triangles " << m.nidx / 3 << '\n';
                hash = hash_bytes(hash, &m.texkey, sizeof m.texkey);
                hash = hash_bytes(hash, &m.nverts, sizeof m.nverts);
                hash = hash_bytes(hash, &m.nidx, sizeof m.nidx);
                hash = hash_bytes(hash, m.verts, size_t(m.nverts) * 5 * sizeof(float));
                hash = hash_bytes(hash, m.idx, size_t(m.nidx) * sizeof(uint16_t));
            }
            sources << "Fingerprint: 0x" << std::hex << std::setw(16) << std::setfill('0')
                    << hash << std::dec << std::setfill(' ') << '\n';
        }
        bool graph = world && std::any_of(doc.items.begin(), doc.items.end(), [category](const Item &i) {
            return (category == KindCount || category == Path) && i.kind == Path && i.source;
        });
        if (graph) {
            const auto &c = world->city;
            uint64_t hash = 14695981039346656037ull;
            hash = hash_bytes(hash, &c.nnav, sizeof c.nnav);
            hash = hash_bytes(hash, c.nav, size_t(c.nnav) * 2 * sizeof(float));
            hash = hash_bytes(hash, &c.nnavedge, sizeof c.nnavedge);
            hash = hash_bytes(hash, c.navedge, size_t(c.nnavedge) * 2 * sizeof(int));
            sources << "\nSource road graph: " << c.nnav << " nodes, " << c.nnavedge << " edges"
                    << "\nGraph fingerprint FNV1a64 (counts, XY and edge pairs in order): 0x"
                    << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec << std::setfill(' ')
                    << "\nNode IDs are local to this loaded graph; source Z is projected, not stored in the XY records.\n";
        }
        for (const auto &i : doc.items) {
            if (!i.source || (category != KindCount && category != i.kind) || i.kind == Wall || i.kind == Object)
                continue;
            sources << "\n" << kind_name(i.kind) << " source for item " << i.id << ": " << i.source << '\n';
            bool found = false;
            if (world && i.kind == Path && i.source <= uint64_t(world->city.nnav)) {
                size_t n = size_t(i.source - 1);
                sources << "Original start node: " << n << "\nOriginal XY: "
                        << world->city.nav[2*n] << ' ' << world->city.nav[2*n+1] << '\n';
                found = true;
            }
            if (world && i.kind == Race)
                for (int n = 0; n < world->city.nev; n++) {
                    const auto &e = world->city.ev[n];
                    if (uint64_t(e.id) != i.source) continue;
                    uint64_t hash = hash_bytes(14695981039346656037ull, &e.info.kind, sizeof e.info.kind);
                    hash = hash_bytes(hash, &e.npoly, sizeof e.npoly);
                    hash = hash_bytes(hash, e.poly, size_t(e.npoly) * 2 * sizeof(float));
                    sources << "Original event: " << e.id << " region " << e.reg << " type " << n2_race_name(e.info.kind)
                            << " name " << std::quoted(e.info.name) << "\nOutline points: " << e.npoly
                            << "\nEvent fingerprint FNV1a64 (kind, outline count, XY): 0x" << std::hex
                            << std::setw(16) << std::setfill('0') << hash << std::dec << std::setfill(' ') << '\n';
                    found = true;
                    break;
                }
            if (world && i.kind == Light)
                for (int n = 0; n < world->neighborhood.nlights; n++) {
                    const auto &l = world->neighborhood.lights[n];
                    if (light_id(l) != i.source) continue;
                    sources << "Original lamp index: " << n << "\nOriginal position: "
                            << l.pos[0] << ' ' << l.pos[1] << ' ' << l.pos[2]
                            << "\nOriginal inner/outer radius: " << l.r_in << ' ' << l.r_out
                            << "\nOriginal RGBA: 0x" << std::hex << l.rgba << std::dec << '\n';
                    found = true;
                    break;
                }
            if (world && i.kind == Region && i.source <= uint64_t(world->city.ndist)) {
                const auto &r = world->city.dist[i.source - 1];
                sources << "Original district: " << r.tok << "\nOriginal bounds XY: "
                        << r.bb[0] << ' ' << r.bb[1] << ' ' << r.bb[2] << ' ' << r.bb[3]
                        << "\nOriginal centroid/median Z: " << r.cx << ' ' << r.cy << ' ' << r.medz << '\n';
                found = true;
            }
            if (!found) {
                sources << "MISSING in current map/scenery; resolve before implementation\n";
                missing++;
            }
        }
        if (category == Wall || category == KindCount) {
            sources << "\nCollision validation source scope: "
                    << (collision_whole ? "entire loaded map/scenery" : "near captured camera")
                    << "\nScan centre: " << collision_center.x << ' ' << collision_center.y << ' ' << collision_center.z
                    << "\nSource road links available: " << collision_roads.roads.size()
                    << "\nSource radius metres: " << (collision_whole ? -1.f : collision_radius)
                    << "\nAll enabled authored walls are checked regardless of source radius.\n"
                    << "Gap hints cover authored walls and generated source boundaries; raw triangle seams are not guessed.\n"
                    << collision::text(collision_results);
            std::set<uint64_t> attributed;
            for (const auto &f : collision_results.findings)
                for (uint64_t source : {f.source,f.other_source}) {
                    if (!attributed.insert(source).second) continue;
                    auto at=object_index.find(source);
                    if (at!=object_index.end()) sources << "Validation source 0x" << std::hex << source << std::dec
                                                       << ' ' << std::quoted(objects[at->second].name) << '\n';
                }
        }
        if (!save_report(path, doc, category, sources.str(), error))
            return false;
        status = "Exported " + path + "; send this file for implementation";
        if (missing)
            status += " (" + std::to_string(missing) + " missing source references flagged)";
        return true;
    }
    int rebuild_lights() {
        int missing = 0;
        lights.clear();
        if (world->neighborhood.nlights)
            lights.assign(world->neighborhood.lights,
                          world->neighborhood.lights + world->neighborhood.nlights);
        for (const auto &i : doc.items)
            if (i.kind == Light) {
                N2LightSrc l{};
                l.pos[0] = i.pos.x;
                l.pos[1] = i.pos.y;
                l.pos[2] = i.pos.z;
                l.r_in = i.values[0];
                l.r_out = i.values[1];
                l.rgba = uint32_t(std::max(0.f, std::min(1.f, i.values[2])) * 255) << 24 |
                         uint32_t(std::max(0.f, std::min(1.f, i.color.x)) * 255) |
                         uint32_t(std::max(0.f, std::min(1.f, i.color.y)) * 255) << 8 |
                         uint32_t(std::max(0.f, std::min(1.f, i.color.z)) * 255) << 16;
                auto it = std::find_if(lights.begin(), lights.end(), [&](const N2LightSrc &x) {
                    return light_id(x) == i.source;
                });
                if (it != lights.end()) {
                    if (i.enabled)
                        *it = l;
                    else
                        lights.erase(it);
                } else if (!i.source && i.enabled)
                    lights.push_back(l);
                else if (i.source)
                    missing++;
            }
        return missing;
    }
    Point ground(Point p) const {
        if (world) {
            float z;
            if (world_ground_at(&world->neighborhood.scene, p.x, p.y, p.z, &z) != WSURF_NONE)
                p.z = z;
        }
        return p;
    }
    Point mouse_ray(ImVec2 mouse) const {
        float look[3], tmp[3] = {camera.x, camera.y, camera.z}, zero[3] = {0, 0, 0};
        render_free_camera(tmp, yaw, pitch, zero, 0, look);
        Point f = point(look), r = normalized(cross(f, {0, 0, 1})), u = cross(r, f);
        ImVec2 size = view_size(), lo = view_min();
        float tanf = std::tan(fov * .00872664626f), aspect = size.x / size.y;
        return normalized(add(f, add(mul(r, ((mouse.x - lo.x) / size.x * 2 - 1) * aspect * tanf),
                                    mul(u, (1 - (mouse.y - lo.y) / size.y * 2) * tanf))));
    }
    Point cursor_ground(ImVec2 mouse) const {
        Point ray = mouse_ray(mouse);
        float t =
            std::fabs(ray.z) > .0001f ? std::max(1.f, std::min(1000.f, -camera.z / ray.z)) : 30;
        Point p = add(camera, mul(ray, t));
        for (int n = 0; n < 4; n++) {
            Point g = ground(p);
            if (std::fabs(ray.z) < .0001f)
                break;
            t = std::max(1.f, std::min(1000.f, (g.z - camera.z) / ray.z));
            p = add(camera, mul(ray, t));
        }
        return ground(p);
    }
    bool project(Point p, ImVec2 &out) const {
        float x = mvp[0] * p.x + mvp[4] * p.y + mvp[8] * p.z + mvp[12],
              y = mvp[1] * p.x + mvp[5] * p.y + mvp[9] * p.z + mvp[13],
              w = mvp[3] * p.x + mvp[7] * p.y + mvp[11] * p.z + mvp[15];
        if (!finite(p) || !std::isfinite(w) || w <= .05f)
            return false;
        ImVec2 lo = view_min(), size = view_size();
        out = {lo.x + size.x * (x / w * .5f + .5f), lo.y + size.y * (.5f - y / w * .5f)};
        return true;
    }
    void line(Point a, Point b, ImU32 color, float thickness = 2) const {
        ImVec2 aa, bb;
        if (project(a, aa) && project(b, bb))
            ImGui::GetBackgroundDrawList()->AddLine(aa, bb, color, thickness);
    }
    void marker(Point p, ImU32 color, const char *label, bool strong = false) const {
        ImVec2 at;
        if (project(p, at)) {
            auto *draw = ImGui::GetBackgroundDrawList();
            draw->AddCircleFilled(at, strong ? 7 : 4, color);
            if (label)
                draw->AddText({at.x + 9, at.y - 8}, color, label);
        }
    }
    void box(Point lo, Point hi, ImU32 color) const {
        Point p[8];
        for (int k = 0; k < 8; k++)
            p[k] = {k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
        for (int k = 0; k < 8; k++)
            for (int b = 1; b < 8; b *= 2)
                if (!(k & b))
                    line(p[k], p[k | b], color);
    }
    void pick(ImVec2 mouse) {
        selected_point = 0;
        float best = 144;
        uint64_t id = 0;
        bool point_pick = true;
        for (const auto &i : doc.items)
            if (i.kind == tab && i.enabled) {
                if (i.points.empty() || i.kind == Race) {
                    ImVec2 p;
                    if (project(i.pos, p)) {
                        float d =
                            (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
                        if (d < best) {
                            best = d;
                            id = i.id;
                            point_pick = false;
                        }
                    }
                }
                if (!i.points.empty())
                    for (size_t n = 0; n < i.points.size(); n++) {
                        ImVec2 p;
                        if (project(i.points[n], p)) {
                            float d = (p.x - mouse.x) * (p.x - mouse.x) +
                                      (p.y - mouse.y) * (p.y - mouse.y);
                            if (d < best) {
                                best = d;
                                id = i.id;
                                selected_point = int(n);
                                point_pick = true;
                            }
                        }
                    }
            }
        if (id) {
            selected = id;
            if (tab == Wall || tab == Object) picked_object = item()->source;
            gizmo_point = point_pick;
            return;
        }
        if (tab == Path && world) {
            best = 100;
            for (int n = 0; n < world->city.nnav; n++) {
                Point p = navpoints[n];
                if (std::hypot(p.x - camera.x, p.y - camera.y) > range)
                    continue;
                ImVec2 xy;
                if (project(p, xy)) {
                    float d =
                        (xy.x - mouse.x) * (xy.x - mouse.x) + (xy.y - mouse.y) * (xy.y - mouse.y);
                    if (d < best) {
                        best = d;
                        navnode = n;
                    }
                }
            }
            return;
        }
        if ((tab != Object && tab != Wall) || !world)
            return;
        Point ray = mouse_ray(mouse);
        float closest = range;
        uint64_t hit = 0;
        if (tab == Wall) {
            for (const auto &i : doc.items) {
                if (i.kind != Wall || !i.enabled) continue;
                for (size_t n = 1; n < i.points.size(); n++) {
                    Point a = i.points[n - 1], b = i.points[n],
                          c = add(b, {0, 0, i.values[0]}), d = add(a, {0, 0, i.values[0]});
                    float t = std::min(ray_triangle(camera, ray, a, b, c), ray_triangle(camera, ray, a, c, d));
                    if (t < closest) {
                        closest = t; id = i.id; selected_point = int(n - 1); hit = i.source;
                    }
                }
            }
            int faces[3] = {};
            std::set<uint64_t> replaced;
            for (const auto &i : doc.items) if (i.kind == Wall && i.source) replaced.insert(i.source);
            if (walls) for (size_t k = 0; k < meshes.size(); k++) {
                const auto &m = meshes[k];
                if (!m.nidx || m.cat == N2_SKY || m.cat == N2_GLOW || replaced.count(mesh_sources[k])) continue;
                const float *b = bounds.data() + 4 * k;
                float dx = std::max({0.f, b[0] - camera.x, camera.x - b[1]}),
                      dy = std::max({0.f, b[2] - camera.y, camera.y - b[3]});
                if (dx * dx + dy * dy > 80 * 80) continue;
                int group = m.wall_nidx ? 2 : (m.cat == N2_ROAD || m.cat == N2_TERRAIN) ? 1 : 0;
                for (int n = 0; n < (m.wall_nidx ? m.wall_nidx : m.nidx) / 3 && faces[group] < WALL_DEBUG_MAX_FACES; n++) {
                    float face[9];
                    if (!phys_wall_debug_face(&m, n, .3f, face)) continue;
                    faces[group]++;
                    float t = ray_triangle(camera, ray, point(face), point(face + 3), point(face + 6));
                    if (t < closest) { closest = t; hit = mesh_sources[k]; id = 0; }
                }
            }
            if ((id || hit) && through) {
                selected = id; picked_object = hit;
                return;
            }
        }
        for (const auto &o : preview_objects) {
            if (o.meshes.empty())
                continue;
            float near = 0, far = closest;
            for (int a = 0; a < 3 && far >= near; a++) {
                float origin = (&camera.x)[a], dir = (&ray.x)[a], lo = (&o.lo.x)[a],
                      hi = (&o.hi.x)[a];
                if (std::fabs(dir) < 1e-7f) {
                    if (origin < lo || origin > hi)
                        far = -1;
                } else {
                    float p = (lo - origin) / dir, q = (hi - origin) / dir;
                    if (p > q)
                        std::swap(p, q);
                    near = std::max(near, p);
                    far = std::min(far, q);
                }
            }
            if (far < near)
                continue;
            for (int k : o.meshes) {
                const auto &m = meshes[k];
                for (int t = 0; t + 2 < m.nidx; t += 3) {
                    Point a = point(m.verts + 5 * m.idx[t]), b = point(m.verts + 5 * m.idx[t + 1]),
                          c = point(m.verts + 5 * m.idx[t + 2]);
                    float dist = ray_triangle(camera, ray, a, b, c);
                    if (dist < closest) {
                        closest = dist;
                        hit = o.id;
                        id = 0;
                    }
                }
            }
        }
        if (id) {
            selected = id;
            picked_object = hit;
            return;
        }
        if (hit) {
            picked_object = hit;
            if (pick_replacement && item() && item()->kind == Object) {
                checkpoint();
                item()->replacement = hit;
                pick_replacement = false;
                changed();
                return;
            }
            selected = 0;
            for (const auto &i : doc.items)
                if (i.kind == tab && i.source == hit)
                    selected = i.id;
        }
    }
    Item &new_item(Kind k) {
        checkpoint();
        auto &i = doc.add(k, kind_name(k));
        i.pos = ground(add(camera, {15, 15, -camera.z}));
        selected = i.id;
        selected_point = 0;
        if (k == Wall) {
            i.points = {i.pos, add(i.pos, {15, 0, 0})};
            i.values[0] = 8;
        }
        if (k == Path || k == Race) {
            i.points = {i.pos, add(i.pos, {30, 0, 0})};
            i.values[0] = k == Race ? 2 : 50;
            i.values[1] = 1;
        }
        if (k == Region)
            i.points = {i.pos, add(i.pos, {50, 0, 0}), add(i.pos, {50, 50, 0}),
                        add(i.pos, {0, 50, 0})};
        if (k == Light) {
            i.pos.z += 5;
            i.values[0] = 2;
            i.values[1] = 25;
            i.values[2] = 1;
        }
        changed();
        return i;
    }
    void collision_edit(bool remove = false) {
        auto found = object_index.find(picked_object);
        if (found == object_index.end())
            return;
        for (auto &i : doc.items)
            if (i.kind == Wall && i.source == picked_object) {
                selected = i.id;
                if (i.enabled == remove) {
                    checkpoint();
                    i.enabled = !remove;
                    changed();
                }
                status = remove ? "Collision removed; visible source retained" : "Edit boundary points and height in Selection";
                return;
            }
        const auto &o = preview_objects[found->second];
        if (o.meshes.empty())
            return;
        Point lo = o.lo, hi = o.hi;
        if (!finite(lo) || !finite(hi) || (!remove && hi.z - lo.z > 1000)) {
            error = "Choose local scenery for a boundary; selected geometry exceeds wall limits";
            return;
        }
        auto &i = new_item(Wall);
        i.name = "Boundary: " + o.name;
        if (i.name.size() > 255) i.name.resize(255);
        i.source = picked_object;
        i.enabled = !remove;
        i.values[0] = std::min(1000.f, std::max(8.f, hi.z - lo.z));
        i.pos = o.center();
        i.points = {{lo.x, lo.y, lo.z},
                    {hi.x, lo.y, lo.z},
                    {hi.x, hi.y, lo.z},
                    {lo.x, hi.y, lo.z},
                    {lo.x, lo.y, lo.z}};
        status = remove ? "Collision removed; visible source retained" : "Replacement starts at source bounds; adjust its points and height";
    }
    void collision_restore() {
        auto found = std::find_if(doc.items.begin(), doc.items.end(), [&](const Item &i) {
            return i.kind == Wall && i.source == picked_object;
        });
        if (found == doc.items.end() || !picked_object) return;
        checkpoint();
        doc.items.erase(found);
        selected = 0;
        changed();
        status = "Original source collision restored";
    }
    void object_edit() {
        auto it = object_index.find(picked_object);
        if (it == object_index.end())
            return;
        for (const auto &i : doc.items)
            if (i.kind == Object && i.source == picked_object) {
                selected = i.id;
                return;
            }
        const auto &o = objects[it->second];
        checkpoint();
        auto &i = doc.add(Object, o.name);
        i.source = o.id;
        i.pos = o.center();
        selected = i.id;
        changed();
    }
    void import_race() {
        if (!world || event_index < 0 || event_index >= world->city.nev)
            return;
        const auto &e = world->city.ev[event_index];
        N2Path path{};
        if (!ai_race_course(root, &e, &path) || path.n < 2) {
            free(path.xy);
            error = "This source event has no usable route";
            return;
        }
        checkpoint();
        auto &i = doc.add(Race, "Event " + std::to_string(e.id));
        i.source = uint64_t(e.id);
        i.values[0] = e.info.kind;
        i.values[1] = e.circuit ? 3 : 1;
        float z = camera.z;
        for (int n = 0; n < path.n; n++) {
            Point p = ground({path.xy[2 * n], path.xy[2 * n + 1], z});
            z = p.z;
            i.points.push_back(p);
        }
        free(path.xy);
        i.pos = i.points.front();
        selected = i.id;
        selected_point = 0;
        changed();
        focus(i.pos);
    }
    void import_path() {
        if (!world || navnode < 0 || navnode >= world->city.nnav) {
            error = "Pick a navigation node in the viewport first";
            return;
        }
        checkpoint();
        auto &i = doc.add(Path, "Road from node " + std::to_string(navnode));
        i.source = uint64_t(navnode) + 1;
        i.values[0] = 50;
        std::vector<int> route;
        int at = navnode, prev = -1;
        for (int n = 0; n < 500; n++) {
            if (std::find(route.begin(), route.end(), at) != route.end())
                break;
            route.push_back(at);
            int next = -1, choices = 0;
            for (int e = 0; e < world->city.nnavedge; e++) {
                int a = world->city.navedge[2 * e], b = world->city.navedge[2 * e + 1];
                int other = a == at ? b : b == at ? a : -1;
                if (other >= 0 && other != prev) {
                    next = other;
                    choices++;
                }
            }
            if (next < 0 || (choices > 1 && n > 0))
                break;
            prev = at;
            at = next;
        }
        for (int n : route)
            i.points.push_back(
                ground({world->city.nav[2 * n], world->city.nav[2 * n + 1], camera.z}));
        if (i.points.size() < 2) {
            doc.items.pop_back();
            error = "Selected node is isolated; create a path instead";
            return;
        }
        i.pos = i.points.front();
        selected = i.id;
        selected_point = 0;
        changed();
    }
    void overlays() {
        if (!world)
            return;
        auto close = [&](Point p) { return std::hypot(p.x - camera.x, p.y - camera.y) < range; };
        if (paths && tab == Path) {
            for (int e = 0; e < world->city.nnavedge; e++) {
                int a = world->city.navedge[2 * e], b = world->city.navedge[2 * e + 1];
                Point p = navpoints[a];
                if (!close(p))
                    continue;
                Point q = navpoints[b];
                p.z += .2f;
                q.z += .2f;
                line(p, q, IM_COL32(55, 140, 225, 120), 1);
            }
            if (navnode >= 0)
                marker(navpoints[navnode], IM_COL32(255, 240, 90, 255), "Source node", true);
        }
        if (source_lights && tab == Light)
            for (int n = 0; n < world->neighborhood.nlights; n++) {
                const auto &l = world->neighborhood.lights[n];
                if (close(point(l.pos)))
                    marker(point(l.pos), IM_COL32(255, 200, 80, 200),
                           n == light_index ? "Source light" : nullptr, n == light_index);
            }
        if (tab == Race)
            for (int n = 0; n < world->city.nev; n++) {
                const auto &e = world->city.ev[n];
                if (e.npoly) {
                    Point p = ground({e.poly[0][0], e.poly[0][1], camera.z});
                    if (close(p))
                        marker(p, IM_COL32(110, 170, 255, 200),
                               n == event_index ? "Source event" : nullptr, n == event_index);
                }
            }
        if (tab == Region && show_regions)
            for (int n = 0; n < world->city.ndist; n++) {
                const auto &d = world->city.dist[n];
                Point a{d.bb[0], d.bb[2], d.medz}, b{d.bb[1], d.bb[3], d.medz + 2};
                box(a, b, IM_COL32(150, 100, 255, 100));
                marker({d.cx, d.cy, d.medz + 2}, IM_COL32(170, 130, 255, 255), d.tok);
            }
        const ImU32 colors[] = {IM_COL32(255, 100, 210, 255), IM_COL32(255, 230, 80, 255),
                                IM_COL32(0, 225, 240, 255),   IM_COL32(110, 170, 255, 255),
                                IM_COL32(255, 220, 60, 255),  IM_COL32(70, 235, 100, 255),
                                IM_COL32(180, 110, 255, 255)};
        for (const auto &i : doc.items)
            if (i.enabled) {
                ImU32 col = colors[i.kind];
                if (i.kind == Shop) {
                    const ImU32 shops[] = {IM_COL32(50, 245, 80, 255), IM_COL32(245, 215, 50, 255),
                                           IM_COL32(255, 70, 60, 255), IM_COL32(50, 135, 255, 255),
                                           IM_COL32(185, 80, 255, 255)};
                    col = shops[int(i.values[0]) % 5];
                }
                bool active = i.id == selected;
                if (i.kind != Object && (close(i.pos) || active))
                    marker(i.pos, col, i.name.c_str(), active);
                if (i.kind == Wall)
                    continue;
                for (size_t p = 1; p < i.points.size(); p++)
                    line(i.points[p - 1], i.points[p], col, active ? 3 : 2);
                if ((i.kind == Region || (i.kind == Path && i.values[1] > .5f) ||
                     (i.kind == Race &&
                      (i.values[0] == 1 || i.values[0] == 5 || i.values[0] == 6))) &&
                    i.points.size() > 2)
                    line(i.points.back(), i.points.front(), col);
                if (active)
                    for (size_t n = 0; n < i.points.size(); n++)
                        marker(i.points[n],
                               n == size_t(selected_point) ? IM_COL32(255, 255, 255, 255) : col,
                               std::to_string(n).c_str(), true);
                if (i.kind == Light && active) {
                    for (int plane = 0; plane < 3; plane++)
                        for (int n = 0; n < 48; n++) {
                            float a = n * 6.2831853f / 48, b = (n + 1) * 6.2831853f / 48;
                            Point p = i.pos, q = i.pos;
                            int x = plane, y = (plane + 1) % 3;
                            (&p.x)[x] += std::cos(a) * i.values[1];
                            (&p.x)[y] += std::sin(a) * i.values[1];
                            (&q.x)[x] += std::cos(b) * i.values[1];
                            (&q.x)[y] += std::sin(b) * i.values[1];
                            line(p, q, col, 1);
                        }
                }
            }
        auto at = object_index.find(picked_object);
        if ((tab == Object || tab == Wall) && at != object_index.end()) {
            const auto &o = preview_objects[at->second];
            if (!o.meshes.empty())
                box(o.lo, o.hi, IM_COL32(255, 225, 50, 255));
            marker(o.center(), IM_COL32(255, 225, 50, 255), o.name.c_str(), true);
        }
    }
    void draw_walls() {
        if (!world)
            return;
        const float palette[3][3] = {{.05f, .85f, 1}, {1, .55f, .12f}, {1, .2f, .65f}};
        std::vector<float> faces[3];
        std::vector<uint64_t> replaced;
        for (const auto &i : doc.items)
            if (i.kind == Wall && i.source)
                replaced.push_back(i.source);
        std::sort(replaced.begin(), replaced.end());
        if (walls)
            for (size_t mesh = 0; mesh < meshes.size(); mesh++) {
                const auto &m = meshes[mesh];
                if (std::binary_search(replaced.begin(), replaced.end(), mesh_sources[mesh]))
                    continue;
                if (m.cat == N2_SKY || m.cat == N2_GLOW || !m.nidx)
                    continue;
                const float *b = bounds.data() + 4 * mesh;
                float dx = std::max({0.f, b[0] - camera.x, camera.x - b[1]}),
                      dy = std::max({0.f, b[2] - camera.y, camera.y - b[3]});
                if (dx * dx + dy * dy > 80 * 80)
                    continue;
                int group = m.wall_nidx ? 2 : (m.cat == N2_ROAD || m.cat == N2_TERRAIN) ? 1 : 0;
                int triangles = (m.wall_nidx ? m.wall_nidx : m.nidx) / 3;
                for (int t = 0; t < triangles && faces[group].size() < WALL_DEBUG_MAX_FACES * 9;
                     t++) {
                    float face[9];
                    if (phys_wall_debug_face(&m, t, .3f, face))
                        faces[group].insert(faces[group].end(), face, face + 9);
                }
            }
        for (const auto &i : doc.items)
            if (i.kind == Wall && i.enabled) {
                for (size_t n = 1; n < i.points.size(); n++) {
                    Point a = i.points[n - 1], b = i.points[n], c = add(b, {0, 0, i.values[0]}),
                          d = add(a, {0, 0, i.values[0]});
                    for (Point p : {a, b, c, a, c, d}) {
                        faces[2].push_back(p.x);
                        faces[2].push_back(p.y);
                        faces[2].push_back(p.z);
                    }
                    marker(a, IM_COL32(255, 100, 210, 255), i.name.c_str(), i.id == selected);
                    if (i.id == selected) {
                        marker(b, IM_COL32(255, 255, 255, 255), std::to_string(n).c_str(), true);
                        line(a, d, IM_COL32(255, 100, 210, 255));
                    }
                }
            }
        for (int g = 0; g < 3; g++)
            for (size_t start = 0; start < faces[g].size(); start += WALL_DEBUG_MAX_FACES * 9) {
                int n =
                    int(std::min(size_t(WALL_DEBUG_MAX_FACES * 9), faces[g].size() - start) / 9);
                render_collision_walls(&shader, &wallvbo, faces[g].data() + start, n, palette[g],
                                       mvp, through);
            }
    }
    void update_view() {
        float eye[3] = {camera.x, camera.y, camera.z}, look[3], move[3] = {0, 0, 0}, view[16],
              projection[16];
        render_free_camera(eye, yaw, pitch, move, 0, look);
        mat_lookat(eye, look, view);
        ImVec2 size = view_size();
        mat_persp(fov * .01745329252f, size.x / size.y, .2f, 12000, projection);
        mat_mul(projection, view, mvp);
    }
    void render() {
        update_view();
        float eye[3] = {camera.x, camera.y, camera.z}, look[3], move[3] = {};
        render_free_camera(eye, yaw, pitch, move, 0, look);
        ImVec2 lo = view_min(), size = view_size();
        glViewport(int(lo.x), int(height - view_max().y), int(size.x), int(size.y));
        glClearColor(.025f, .035f, .065f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(shader.prog);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glUniformMatrix4fv(shader.uMVP, 1, GL_FALSE, mvp);
        render_model(&shader, nullptr);
        glUniform3f(shader.uCamPos, camera.x, camera.y, camera.z);
        glUniform3f(shader.uColor, 1, 1, 1);
        glUniform1f(shader.uVColor, 1);
        glUniform1f(shader.uAmbient, .6f);
        glUniform1f(shader.uDiffuse, .4f);
        glUniform3f(shader.uLight, N2_SUN_X, N2_SUN_Y, N2_SUN_Z);
        glUniform1f(shader.uAlpha, 1);
        glUniform1f(shader.uFogDensity, .0008f);
        glUniform3f(shader.uFogColor, .025f, .035f, .065f);
        glUniform1f(shader.uUnlit, 0);
        glUniform1f(shader.uWetness, 0);
        glUniform1f(shader.uSpec, 0);
        glActiveTexture(GL_TEXTURE0);
        if (world) {
            std::vector<std::pair<float, int>> blend;
            auto draw = [&](const N2Batch &b) {
                glUniform1f(shader.uUseTex, b.tex ? 1 : 0);
                glBindTexture(GL_TEXTURE_2D, b.tex);
                glUniform1f(shader.uAlphaTest, b.drawmode == N2_DRAW_CUTOUT);
                glUniform1f(shader.uTextureAlpha,
                            b.drawmode == N2_DRAW_BLEND || b.drawmode == N2_DRAW_ADD);
                draw_batch(&b);
            };
            glDepthMask(GL_FALSE);
            glUniform1f(shader.uUnlit, 1);
            for (int k = 0; k < nsky; k++)
                draw(sky[k]);
            glUniform1f(shader.uUnlit, 0);
            glDepthMask(GL_TRUE);
            for (int k = 0; k < nbatch; k++) {
                const auto &b = batches[k];
                Point center{(b.bbox_min[0] + b.bbox_max[0]) * .5f,
                             (b.bbox_min[1] + b.bbox_max[1]) * .5f,
                             (b.bbox_min[2] + b.bbox_max[2]) * .5f};
                float dx = std::max(0.f,
                                    std::max(b.bbox_min[0] - camera.x, camera.x - b.bbox_max[0])),
                      dy = std::max(0.f,
                                    std::max(b.bbox_min[1] - camera.y, camera.y - b.bbox_max[1]));
                if (dx * dx + dy * dy > range * range || !render_batch_in_view(&b, mvp))
                    continue;
                if (b.drawmode >= N2_DRAW_BLEND) {
                    blend.push_back({-dot(sub(center, camera), sub(center, camera)), k});
                    continue;
                }
                draw(b);
            }
            std::sort(blend.begin(), blend.end());
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            for (auto b : blend) {
                auto &batch = batches[b.second];
                glBlendFunc(GL_SRC_ALPHA,
                            batch.drawmode == N2_DRAW_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                draw(batch);
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glUniform1f(shader.uAlphaTest, 0);
            glUniform1f(shader.uTextureAlpha, 0);
            render_world_glows(&shader, glows, nglow, mvp);
            if (flare && !lights.empty())
                render_district_lights(&shader, &quad, flare, lights.data(), int(lights.size()),
                                       eye, look, mvp, range, 1, 1);
            ImGui::GetBackgroundDrawList()->PushClipRect(view_min(), view_max(), true);
            draw_walls();
            overlays();
            draw_collision_findings();
            draw_gizmo();
            ImGui::GetBackgroundDrawList()->PopClipRect();
        }
        glUseProgram(0);
    }
    bool verify_gizmos() {
        auto fail = [&](const char *why) { error = std::string("Gizmo verification: ") + why; return false; };
        Document before = doc;
        size_t steps = undo.size();
        auto found = std::find_if(doc.items.begin(),doc.items.end(),[](const Item &i) { return i.kind == Object; });
        if (found == doc.items.end()) return fail("no authored object");
        uint64_t id = found->id, source = found->source, donor = found->replacement;
        tab = Object; selected = id; picked_object = source;
        focus(found->pos); update_view();
        Point pivot = item()->pos;
        float length = gizmo_length(pivot);
        ImVec2 start,end;
        auto screen = [&](Point p) { ImVec2 s; project(p,s); return s; };
        auto gpu_matches = [&]() {
            const auto &o = preview_objects[object_index.at(source)];
            size_t matched = 0;
            for (auto array : {std::make_pair(batches,nbatch),std::make_pair(glows,nglow),std::make_pair(sky,nsky)})
                for (int b = 0; b < array.second; b++)
                    for (int n = 0; n < array.first[b].nprops; n++) {
                        auto r = array.first[b].props[n];
                        if (std::find(o.meshes.begin(),o.meshes.end(),r.mesh)==o.meshes.end()) continue;
                        BatchedVertex v{};
                        glBindBuffer(GL_ARRAY_BUFFER,array.first[b].vbo);
                        glGetBufferSubData(GL_ARRAY_BUFFER,long(r.vertex)*sizeof v,sizeof v,&v);
                        Point expected = point(meshes[r.mesh].verts), actual = point(v.pos);
                        if (dot(sub(expected,actual),sub(expected,actual))>.000001f) return false;
                        matched++;
                    }
            return matched == o.meshes.size() && glGetError()==GL_NO_ERROR;
        };
        gizmo_mode = 0; grid_snap = true; grid_step = 1; ground_snap = false;
        start = screen(add(pivot,mul(axis(0),length*.65f)));
        if (gizmo_hit(start)!=0 || !begin_gizmo(0,start)) return fail("move handle cannot be picked");
        N2Batch *live = batches; GLuint vbo = batches[0].vbo;
        Uint64 clock = SDL_GetPerformanceCounter();
        for (int n = 1; n <= 100; n++)
            update_gizmo(screen(add(pivot,mul(axis(0),length*.65f+3.2f*n/100))));
        double ms = 1000.0*(SDL_GetPerformanceCounter()-clock)/SDL_GetPerformanceFrequency();
        if (!drag.active || std::fabs(item()->pos.x-snap(pivot.x+3.2f,1))>.002f ||
            item()->pos.y!=pivot.y || item()->pos.z!=pivot.z || batches!=live || batches[0].vbo!=vbo ||
            !gpu_matches()) return fail("live/grid update or unchanged map buffers");
        finish_gizmo(false);
        Document moved = doc;
        if (undo.size()!=steps+1) return fail("drag must produce exactly one undo step");
        Document parsed; std::istringstream bytes(encode(moved));
        if (!decode(bytes,parsed,error) || encode(parsed)!=encode(moved)) return fail("drag project roundtrip");
        history(true);
        if (encode(doc)!=encode(before) || !gpu_matches()) return fail("undo did not restore geometry");
        history(false);
        if (encode(doc)!=encode(moved) || !gpu_matches()) return fail("redo did not restore geometry");
        selected = id; picked_object = source; pivot = item()->pos;
        grid_snap = false; update_view(); length = gizmo_length(pivot);
        start = screen(add(pivot,mul(axis(1),length*.65f)));
        if (!begin_gizmo(1,start)) return fail("cancel drag start");
        update_gizmo(screen(add(pivot,mul(axis(1),length*.65f+2))));
        size_t undo_before = undo.size(); bool dirty_before = drag.dirty;
        finish_gizmo(true);
        if (encode(doc)!=encode(moved) || undo.size()!=undo_before || dirty!=dirty_before || !gpu_matches())
            return fail("cancel changed data, undo history or geometry");
        start = screen(add(pivot,mul(axis(0),length*.65f)));
        if (!begin_gizmo(0,start)) return fail("no-op drag start");
        update_gizmo(start); finish_gizmo(false);
        if (encode(doc)!=encode(moved) || undo.size()!=undo_before) return fail("no-op dirtied document");
        gizmo_mode = 1;
        float angle = .6f;
        for (int n = 0; n < 64; n++) {
            angle = .12f + n * .09f;
            start = screen(ring_point(pivot,2,length,angle));
            if (gizmo_hit(start)==2) break;
        }
        if (gizmo_hit(start)!=2) return fail("rotation ring cannot be picked");
        end = screen(ring_point(pivot,2,length,angle+.6f));
        Point original_rotation = item()->rotation;
        if (!begin_gizmo(2,start)) return fail("rotation drag start");
        update_gizmo(end);
        Item expected = *item(); expected.rotation = rotate_world(original_rotation,2,.6f);
        Point a = transform({2,3,5},{},expected), b = transform({2,3,5},{},*item());
        if (dot(sub(a,b),sub(a,b))>.0001f || !gpu_matches()) return fail("world rotation / GPU preview");
        finish_gizmo(false); history(true); selected = id;
        gizmo_mode = 2;
        start = screen(add(pivot,mul(axis(0),length*.65f)));
        if (gizmo_hit(start)!=0 || !begin_gizmo(0,start)) return fail("scale handle cannot be picked");
        update_gizmo(screen(add(pivot,mul(axis(0),length*1.15f))));
        if (std::fabs(item()->scale-drag.original.scale*1.5f)>.001f || !gpu_matches()) return fail("uniform scaling");
        update_gizmo(screen(add(pivot,mul(axis(0),length*150))));
        if (item()->scale!=100 || !gpu_matches()) return fail("scale upper limit");
        update_gizmo(screen(add(pivot,mul(axis(0),-length*10))));
        if (item()->scale!=.01f || !gpu_matches()) return fail("scale lower limit");
        finish_gizmo(false); history(true); selected = id;
        gizmo_mode = 0; ground_snap = true;
        start = screen(pivot);
        if (!begin_gizmo(3,start)) return fail("XY plane handle");
        update_gizmo(screen(add(pivot,{2,1,0})));
        const auto &o = preview_objects[object_index.at(source)];
        if (std::fabs(o.lo.z-ground({item()->pos.x,item()->pos.y,o.lo.z}).z)>.002f || !gpu_matches())
            return fail("object base ground snap");
        finish_gizmo(true); ground_snap = false;
        for (Kind kind : {Wall,Path,Race,Light,Shop,Region}) {
            auto it = std::find_if(doc.items.begin(),doc.items.end(),[&](const Item &i) { return i.kind==kind; });
            if (it==doc.items.end()) return fail("missing marker/point category");
            Document keep = doc; size_t count = undo.size();
            tab = kind; selected = it->id; selected_point = 0; gizmo_point = kind!=Race;
            int p = gizmo_point_index(*it); Point at = p<0 ? it->pos : it->points[p];
            focus(at); update_view(); float len = gizmo_length(at);
            start = screen(add(at,mul(axis(0),len*.65f)));
            if (!begin_gizmo(0,start)) return fail("marker/point drag start");
            update_gizmo(screen(add(at,mul(axis(0),len*.65f+1))));
            Point actual = p<0 ? item()->pos : item()->points[p];
            if (std::fabs(actual.x-at.x-1)>.002f || actual.y!=at.y || actual.z!=at.z)
                return fail("marker/point moved on wrong axes");
            if (kind==Light && std::none_of(lights.begin(),lights.end(),[&](const N2LightSrc &l) {
                return dot(sub(point(l.pos),actual),sub(point(l.pos),actual))<.000001f;
            })) return fail("lamp glow did not move");
            finish_gizmo(false);
            if (undo.size()!=count+1) return fail("marker undo count");
            history(true);
            if (encode(doc)!=encode(keep)) return fail("marker/point undo");
        }
        // Source selection alone must not create an override, including a cancelled first drag.
        tab = Object; selected = 0; picked_object = donor;
        Item target;
        if (!gizmo_target(target) || target.id) return fail("unedited source selection");
        Document keep = doc; size_t count = undo.size(); bool was_dirty = dirty;
        focus(target.pos); update_view(); length = gizmo_length(target.pos);
        start = screen(add(target.pos,mul(axis(0),length*.65f)));
        if (!begin_gizmo(0,start)) return fail("source drag start");
        update_gizmo(screen(add(target.pos,mul(axis(0),length*.65f+2))));
        finish_gizmo(true);
        if (encode(doc)!=encode(keep) || undo.size()!=count || dirty!=was_dirty)
            return fail("cancelled source drag left an override");
        doc = before; undo.resize(steps); redo.clear(); selected = id; picked_object = source;
        tab = Object; gizmo_mode = 0; focus(item()->pos); update_view();
        if (!rebuild()) return false;
        printf("MAP EDITOR GIZMOS: live GPU slices, move/rotate/scale, grid/base snapping, 7-category "
               "movement, one-step undo/redo/cancel, source preservation PASS (100 live moves %.2f ms)\n",ms);
        return true;
    }
    bool verify_collisions(const char *path) {
        Document keep=doc; std::string original_file=file; bool was_dirty=dirty;
        Point mid; Point along; bool road=false;
        const auto &c=world->city;
        for (int n=0;n<c.nnavedge;n++) {
            int a=c.navedge[2*n],b=c.navedge[2*n+1];
            if (a<0 || b<0 || a>=c.nnav || b>=c.nnav) continue;
            Point p{c.nav[2*a],c.nav[2*a+1],camera.z},q{c.nav[2*b],c.nav[2*b+1],camera.z};
            mid=mul(add(p,q),.5f);
            if (std::hypot(mid.x-camera.x,mid.y-camera.y)>100 || std::hypot(p.x-q.x,p.y-q.y)<2) continue;
            WGroundHit h;
            if (world_ground_hit(&world->neighborhood.scene,mid.x,mid.y,mid.z,&h)!=WSURF_ROAD || h.normal[2]<.9f) continue;
            mid.z=h.z; along=collision::direction(p,q); road=true; break;
        }
        if (!road) { error="Collision verification needs a nearby source road link"; return false; }
        Point side{-along.y,along.x,0};
        auto add_wall=[&](const char *name,Point a,Point b,float height) {
            auto &i=doc.add(Wall,name); i.points={a,b}; i.values[0]=height; return i.id;
        };
        uint64_t cross=add_wall("Validation test: road crossing",add(mid,mul(side,-2)),add(mid,mul(side,2)),8);
        Point a=ground(add(mid,mul(along,-.25f))),b=ground(add(mid,mul(along,.25f)));
        a.z-=1; b.z-=1;
        uint64_t buried=add_wall("Validation test: below road",a,b,.5f);
        uint64_t high=add_wall("Validation test: excessive height",add(mid,{3,3,0}),add(mid,{3,5,0}),60);
        Point far=add(mid,{8000,0,0});
        uint64_t gap=add_wall("Validation test: open end A",far,add(far,{10,0,0}),8);
        add_wall("Validation test: open end B",add(far,{11,0,0}),add(far,{20,0,0}),8);
        uint64_t zero=add_wall("Validation test: repeated point",mid,mid,8);
        std::string before=encode(doc);
        collision_whole=true;
        if (!start_collision_scan()) return false;
        size_t slices=0; double maximum=0;
        while (collision_running) {
            Uint64 clock=SDL_GetPerformanceCounter(); collision_step(); slices++;
            maximum=std::max(maximum,1000.0*(SDL_GetPerformanceCounter()-clock)/SDL_GetPerformanceFrequency());
        }
        auto has=[&](uint64_t id,collision::Code code) {
            return std::any_of(collision_results.findings.begin(),collision_results.findings.end(),[&](const collision::Finding &f) {
                return f.item==id && f.code==code;
            });
        };
        if (!has(cross,collision::RoadCrossing) || !has(buried,collision::Buried) || !has(high,collision::Height) ||
            !has(gap,collision::Gap) || !has(zero,collision::ZeroLength) || !collision_results.source_faces ||
            encode(doc)!=before || dirty!=was_dirty || collision_stale) {
            fprintf(stderr,"Collision fixture flags: cross%d buried%d height%d gap%d zero%d source%zu stale%d\n",
                    has(cross,collision::RoadCrossing),has(buried,collision::Buried),has(high,collision::Height),
                    has(gap,collision::Gap),has(zero,collision::ZeroLength),collision_results.source_faces,collision_stale);
            error="Native collision review failed expected flags or read-only state"; return false;
        }
        std::string cases=std::string(path).substr(0,strlen(path)-7)+"-cases.ug2map";
        if (cases.size()>=sizeof file) { error="Verification filename too long"; return false; }
        snprintf(file,sizeof file,"%s",cases.c_str());
        if (!mapedit::save(file,doc,error) || !report_export(Wall)) return false;
        std::ifstream exported(report_path(file,Wall)); std::ostringstream bytes; bytes << exported.rdbuf();
        std::string report=bytes.str();
        for (const char *name:{"COLLISION_VALIDATION 1","ROAD_CROSSING","BARRIER_GAP","BURIED","HEIGHT","ZERO_LENGTH"})
            if (report.find(name)==std::string::npos) { error="Validation fields absent from exported report"; return false; }
        if (report.find(encode(doc))==std::string::npos) { error="Validation report lost the complete project"; return false; }
        printf("MAP EDITOR COLLISION: all 5 fixture flags, source candidates, read-only scan, full project/report "
               "PASS (%zu source faces, %zu findings, partial%d, CPU %.1fms, %zu slices max %.2fms)\n",
               collision_results.source_faces,collision_results.findings.size(),collision_results.limited,
               collision_ms,slices,maximum);
        if (!start_collision_scan()) return false;
        pending_report=int(Wall); invalidate_collision();
        if (collision_running || !collision_stale || pending_report!=-1 || collision_grid.start || encode(doc)!=before) {
            error="Collision scan interruption retained a stale export or changed edits"; return false;
        }
        doc=std::move(keep); dirty=was_dirty;
        snprintf(file,sizeof file,"%s",original_file.c_str());
        collision_whole=false;
        if (!rebuild() || !start_collision_scan()) return false;
        while(collision_running) collision_step();
        tab=Wall; selected_finding=collision_results.findings.empty()?-1:0;
        return true;
    }
    bool verify_editor_ui() {
        auto fail = [&](const char *why) { error = std::string("Editor UI verification: ") + why; return false; };
        Document keep = doc;
        auto saved_undo = undo, saved_redo = redo;
        bool was_dirty = dirty;
        Point saved_camera = camera;
        float saved_yaw = yaw, saved_pitch = pitch;
        int saved_width = width, saved_height = height;
        camera = {0,0,10}; yaw = pitch = 0;
        for (auto size : {ImVec2{1100,720}, ImVec2{1440,900}, ImVec2{1920,1080}}) {
            width = int(size.x); height = int(size.y); update_view();
            ImVec2 center{(view_min().x+view_max().x)*.5f,(view_min().y+view_max().y)*.5f}, actual;
            if (!in_view(center) || in_view({1,center.y}) || in_view({width-1.f,center.y}) ||
                in_view({center.x,1}) || in_view({center.x,height-1.f})) return fail("panel input leaks into viewport");
            if (!project({20,0,10},actual) || std::hypot(actual.x-center.x,actual.y-center.y)>.01f)
                return fail("viewport projection centre");
            for (Point target : {Point{20,0,10},Point{20,2,12},Point{20,-3,7}}) {
                if (!project(target,actual)) return fail("projection failed");
                Point delta = sub(mouse_ray(actual),normalized(sub(target,camera)));
                if (dot(delta,delta)>1e-7f) return fail("viewport picking ray differs from projection");
            }
        }
        width = saved_width; height = saved_height; update_view();
        auto saved_meshes = std::move(meshes);
        auto saved_sources = std::move(mesh_sources);
        auto saved_bounds = std::move(bounds);
        float v[] = {20,-5,5,0,0, 20,5,5,0,0, 20,0,15,0,0};
        uint16_t idx[] = {0,1,2};
        N2Mesh m{}; m.verts=v; m.nverts=3; m.idx=idx; m.nidx=3;
        uint64_t source = objects.front().id;
        doc.items.clear(); tab=Wall; selected=picked_object=0; walls=true;
        ImVec2 center{(view_min().x+view_max().x)*.5f,(view_min().y+view_max().y)*.5f};
        for (int group=0;group<4;group++) {
            m.cat=group==1?N2_ROAD:group==2?N2_TERRAIN:N2_OTHER;
            m.wall_verts=group==3?v:nullptr; m.wall_idx=group==3?idx:nullptr;
            m.wall_nverts=group==3?3:0; m.wall_nidx=group==3?3:0;
            meshes={m}; mesh_sources={source}; bounds={20,20,-5,5};
            picked_object=0; pick(center);
            if (picked_object!=source || selected) return fail("source collision overlay cannot be selected");
        }
        auto &wall=doc.add(Wall,"Pickable authored wall");
        wall.points={{10,-5,5},{10,5,5}}; wall.values[0]=10;
        uint64_t authored=wall.id;
        pick(center);
        if (selected!=authored || picked_object) return fail("authored wall face cannot be selected");
        meshes=std::move(saved_meshes); mesh_sources=std::move(saved_sources); bounds=std::move(saved_bounds);
        doc=keep; camera=saved_camera; yaw=saved_yaw; pitch=saved_pitch;
        for (int cat : {N2_OTHER,N2_ROAD,N2_TERRAIN}) {
            auto at=std::find_if(objects.begin(),objects.end(),[&](const SourceObject &o) {
                return !o.meshes.empty() && meshes[o.meshes[0]].cat==cat && finite(o.lo) && finite(o.hi) && o.hi.z-o.lo.z<=1000 &&
                       std::none_of(keep.items.begin(),keep.items.end(),[&](const Item &i) { return i.kind==Object && i.source==o.id; });
            });
            if (at==objects.end()) return fail("missing native source category");
            doc=keep; picked_object=at->id; collision_restore();
            size_t steps=undo.size();
            collision_edit(true);
            if (!item() || item()->enabled || item()->source!=at->id || undo.size()!=steps+1)
                return fail("direct removal did not create one undo step");
            uint64_t removal=item()->id;
            std::string removed=encode(doc);
            collision_edit(true);
            if (undo.size()!=steps+1 || encode(doc)!=removed) return fail("repeated removal changed history");
            if (!start_collision_scan()) return false;
            for (int k:at->meshes) {
                if (collision_source_visible(k) || meshes[k].nidx!=world->neighborhood.scene.meshes[k].nidx ||
                    meshes[k].verts!=world->neighborhood.scene.meshes[k].verts)
                    return fail("removal changed rendered source or retained collision candidates");
            }
            invalidate_collision();
            if (implementation_report(doc,Wall,"").find("Action: DISABLE_SOURCE_COLLISION")==std::string::npos)
                return fail("removal missing from handoff");
            Document repaired = doc;
            for (auto &i:repaired.items) if (i.id==removal) i.enabled=true;
            collision_edit();
            if (encode(doc)!=encode(repaired))
                return fail("editing removed boundary lost its geometry");
            collision_restore();
            if (item() || std::any_of(doc.items.begin(),doc.items.end(),[&](const Item &i) { return i.kind==Wall && i.source==at->id; }))
                return fail("original collision was not restored");
            history(true);
            if (!std::any_of(doc.items.begin(),doc.items.end(),[&](const Item &i) { return i.id==removal && i.enabled; }))
                return fail("restore undo did not bring back edited boundary");
            history(true);
            if (encode(doc)!=removed) return fail("removal undo/redo state differs");
            history(false); history(false);
        }
        doc=std::move(keep); undo=std::move(saved_undo); redo=std::move(saved_redo); dirty=was_dirty;
        selected=picked_object=0; update_view();
        if (!rebuild()) return false;
        puts("MAP EDITOR UI: 3 window sizes, viewport projection/rays/panel exclusion, coloured source and authored wall picking, native object/road/terrain removal/edit/restore/undo/export and unchanged source geometry PASS");
        return true;
    }
    bool verify(const char *path) {
        if (!world || objects.size() < 2) {
            error = "Verification needs map objects";
            return false;
        }
        Point original_camera = camera;
        float original_yaw = yaw, original_pitch = pitch;
        int source = -1, donor = -1;
        for (size_t n = 0; n < objects.size(); n++)
            if (objects[n].meshes.size() > 1 &&
                std::hypot(objects[n].center().x - camera.x, objects[n].center().y - camera.y) <
                    150 &&
                dot(sub(objects[n].hi, objects[n].lo), sub(objects[n].hi, objects[n].lo)) < 40000 &&
                world->neighborhood.scene.meshes[objects[n].meshes[0]].cat == N2_OTHER) {
                if (source < 0)
                    source = int(n);
                else if (objects[n].name != objects[source].name) {
                    donor = int(n);
                    break;
                }
            }
        if (source < 0 || donor < 0) {
            error = "Verification needs two complete scenery models";
            return false;
        }
        uint64_t id = objects[source].id;
        int first = objects[source].meshes[0];
        Point original = point(world->neighborhood.scene.meshes[first].verts);
        picked_object = id;
        object_edit();
        item()->pos.x += 2;
        item()->rotation.z = 30;
        item()->replacement = objects[donor].id;
        size_t slices = objects[donor].meshes.size();
        collision_edit();
        if (!item() || item()->kind != Wall)
            return false;
        auto &wall = new_item(Wall);
        wall.name = "8 m authored wall";
        wall.points = {{-98, 88, 2}, {-84, 88, 2}};
        if (world->city.nnav > 1) {
            float best = INFINITY;
            for (int n = 0; n < world->city.nnav; n++) {
                float d = std::hypot(world->city.nav[n * 2] - camera.x,
                                     world->city.nav[n * 2 + 1] - camera.y);
                if (d < best) {
                    best = d;
                    navnode = n;
                }
            }
            import_path();
        }
        if (std::none_of(doc.items.begin(), doc.items.end(),
                         [](const Item &i) { return i.kind == Path; }))
            new_item(Path);
        if (world->city.nev)
            import_race();
        else
            new_item(Race);
        new_item(Light).name = "Editable lamp";
        if (world->neighborhood.nlights) {
            const auto &l = world->neighborhood.lights[0];
            auto &i = new_item(Light);
            i.name = "Source lamp test";
            i.source = light_id(l);
            i.pos = point(l.pos);
            i.values[0] = l.r_in;
            i.values[1] = l.r_out;
            i.values[2] = .5f;
        }
        new_item(Shop).name = "Body shop";
        auto &district = new_item(Region);
        district.name = "Test district";
        if (world->city.ndist) {
            const auto &r = world->city.dist[0];
            district.source = 1;
            district.pos = {r.cx, r.cy, r.medz};
            district.points = {{r.bb[0],r.bb[2],r.medz}, {r.bb[1],r.bb[2],r.medz},
                               {r.bb[1],r.bb[3],r.medz}, {r.bb[0],r.bb[3],r.medz}};
        }
        selected = doc.items[2].id;
        tab = Wall;
        walls = true;
        if (!rebuild() || preview_objects[source].meshes.size() != slices) {
            error = "Replacement lost a material slice";
            return false;
        }
        if (dot(sub(original, point(world->neighborhood.scene.meshes[first].verts)),
                sub(original, point(world->neighborhood.scene.meshes[first].verts))) != 0) {
            error = "Source mesh was mutated";
            return false;
        }
        if (!verify_editor_ui() || !verify_gizmos() || !verify_collisions(path)) return false;
        if (!mapedit::save(path, doc, error))
            return false;
        Document loaded;
        if (!mapedit::load(path, loaded, error) || encode(loaded) != encode(doc)) {
            error = "Project roundtrip differs";
            return false;
        }
        if (!load_map(doc.map, scene_event))
            return false;
        auto found = object_index.find(id);
        if (found == object_index.end() || preview_objects[found->second].meshes.size() != slices) {
            error = "Source identity changed on reload";
            return false;
        }
        snprintf(file, sizeof file, "%s", path);
        for (int k = Wall; k <= KindCount; k++)
            if (!report_export(Kind(k)))
                return false;
        camera = original_camera;
        yaw = original_yaw;
        pitch = original_pitch;
        dirty = false;
        status = "Verified all 7 editors, source identity, replacement slices and export/save/load";
        puts("MAP EDITOR VERIFY: categories, whole-object replacement, source immutability, stable "
             "reload, project roundtrip, all-category exports PASS");
        return true;
    }
    void invalidate_collision() {
        if (collision_running) status=pending_report>=0 ? "Queued export interrupted; check again after edits"
                                                       : "Collision check interrupted; results out of date";
        collision_stale = true;
        collision_running = false;
        pending_report = -1;
        if (collision_grid.start) {
            world_ground_grid_activate(world ? &world->neighborhood.grid : nullptr);
            world_ground_grid_free(&collision_grid);
        }
    }
    collision::Surface collision_ground(Point p) const {
        WGroundHit hit{};
        int kind=world_ground_hit(&collision_scene,p.x,p.y,p.z,&hit);
        return {kind,hit.z,hit.normal[2]};
    }
    bool collision_source_visible(size_t k) const {
        const auto &m=meshes[k];
        if (!m.nidx || m.cat==N2_SKY || m.cat==N2_GLOW || collision_replaced.count(mesh_sources[k])) return false;
        if (collision_whole) return true;
        const float *b=bounds.data()+4*k;
        float dx=std::max({0.f,b[0]-collision_center.x,collision_center.x-b[1]}),
              dy=std::max({0.f,b[2]-collision_center.y,collision_center.y-b[3]});
        return dx*dx+dy*dy<=collision_radius*collision_radius;
    }
    bool start_collision_scan() {
        if (!world || drag.active || !validate(doc,error)) return false;
        if (preview_dirty && !rebuild()) return false;
        invalidate_collision();
        Uint64 begin=SDL_GetPerformanceCounter();
        collision_results={}; collision_roads={}; collision_segments.clear(); collision_replaced.clear();
        selected_finding=-1; collision_center=camera; collision_mesh=collision_segment=0; collision_triangle=0; collision_gaps=false;
        collision_scene={meshes.data(),int(meshes.size()),0};
        std::vector<float> ground_bounds(bounds.size());
        float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
        for (size_t k=0;k<meshes.size();k++) {
            const float *b=bounds.data()+4*k;
            float *g=ground_bounds.data()+4*k;
            g[0]=b[0]; g[1]=b[2]; g[2]=b[1]; g[3]=b[3];
            if (meshes[k].cat!=N2_ROAD && meshes[k].cat!=N2_TERRAIN) continue;
            x0=std::min(x0,g[0]); y0=std::min(y0,g[1]); x1=std::max(x1,g[2]); y1=std::max(y1,g[3]);
        }
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) ||
            (double(x1-x0)/64+1)*(double(y1-y0)/64+1)>1000000 ||
            !world_ground_grid_build(&collision_grid,&collision_scene,(const float (*)[4])ground_bounds.data())) {
            error="Could not index preview ground for validation (missing or excessive extent)";
            return false;
        }
        const auto &c=world->city;
        for (int n=0;n<c.nnavedge;n++) {
            int a=c.navedge[2*n],b=c.navedge[2*n+1];
            if (a<0 || b<0 || a>=c.nnav || b>=c.nnav) continue;
            Point p{c.nav[2*a],c.nav[2*a+1],0},q{c.nav[2*b],c.nav[2*b+1],0};
            if (finite(p) && finite(q)) collision_roads.add({p,q,n});
        }
        if (collision_roads.roads.empty()) collision_results.limited=true;
        for (const auto &i:doc.items) {
            if (i.kind!=Wall) continue;
            if (i.source) collision_replaced.insert(i.source);
            if (!i.enabled) continue;
            for (size_t n=1;n<i.points.size();n++) {
                if (collision_segments.size()>=100000) { collision_results.limited=true; break; }
                collision_segments.push_back({i.points[n-1],i.points[n],i.values[0],i.id,i.source,int(n-1)});
            }
        }
        for (size_t k=0;k<meshes.size();k++) {
            const auto &m=meshes[k];
            if (!collision_source_visible(k) || !m.wall_verts) continue;
            // phys_prepare_boundaries owns four vertices per generated boundary quad.
            for (int n=0;n+3<m.wall_nverts;n+=4) {
                if (collision_segments.size()>=100000) { collision_results.limited=true; break; }
                Point a=point(m.wall_verts+n*5),b=point(m.wall_verts+(n+1)*5),top=point(m.wall_verts+(n+3)*5);
                if (finite(a) && finite(b) && finite(top))
                    collision_segments.push_back({a,b,top.z-a.z,0,mesh_sources[k],n/2});
            }
        }
        collision_ms=1000.0*(SDL_GetPerformanceCounter()-begin)/SDL_GetPerformanceFrequency();
        collision_running=true;
        status="Checking source collision candidates; edits interrupt the scan";
        return true;
    }
    void collision_step() {
        if (!collision_running) return;
        Uint64 begin=SDL_GetPerformanceCounter();
        world_ground_grid_activate(&collision_grid);
        auto ground=[&](Point p) { return collision_ground(p); };
        while (collision_segment<collision_segments.size()) {
            if (1000.0*(SDL_GetPerformanceCounter()-begin)/SDL_GetPerformanceFrequency()>=4) break;
            const auto &s=collision_segments[collision_segment++];
            collision::inspect(s,ground,collision_results);
            collision_roads.check(s,ground,collision_results);
        }
        if (collision_segment==collision_segments.size() && !collision_gaps) {
            // shortcut: gap/index passes are synchronous; split them if measured editor stalls need reducing.
            collision::gaps(collision_segments,collision_results); collision_gaps=true;
        }
        while (collision_gaps && collision_mesh<meshes.size()) {
            if (1000.0*(SDL_GetPerformanceCounter()-begin)/SDL_GetPerformanceFrequency()>=4) break;
            size_t k=collision_mesh;
            const auto &m=meshes[k];
            int triangles=(m.wall_verts?m.wall_nidx:m.nidx)/3;
            if (!collision_source_visible(k) || collision_triangle>=triangles) {
                collision_mesh++; collision_triangle=0; continue;
            }
            int t=collision_triangle++;
            float face[9];
            if (!phys_wall_debug_face(&m,t,0,face)) continue;
            Point p[3]={point(face),point(face+3),point(face+6)};
            if (!finite(p[0]) || !finite(p[1]) || !finite(p[2])) { collision_results.limited=true; continue; }
            collision_results.source_faces++;
            Point mid=mul(add(add(p[0],p[1]),p[2]),1.f/3);
            float top=std::max({p[0].z,p[1].z,p[2].z});
            float height=phys_wall_face_height(face,face+3,face+6);
            collision::Finding f; f.source=mesh_sources[k]; f.part=t; f.mesh=int(k); f.a=f.b=mid;
            if ((height<.3f || height>30) && (m.wall_verts || m.scen==N2_SC_WALL)) {
                f.code=collision::Height; f.value=height;
                f.detail="Source candidate vertical thickness outside review range; low seams may already be ignored by physics";
                collision_results.add(f);
            }
            Point samples[4]={mid,mul(add(p[0],p[1]),.5f),mul(add(p[1],p[2]),.5f),mul(add(p[2],p[0]),.5f)};
            if (collision::buried(samples,4,top,ground)) {
                f.code=collision::Buried; f.value=top; f.a.z=f.b.z=top;
                f.detail="Entire candidate face below nearby supporting layer at four samples";
                collision_results.add(f);
            }
            if (!collision_results.source_groups.count({mesh_sources[k],int(collision::RoadCrossing)})) {
                collision::Segment s; s.source=mesh_sources[k]; s.part=t; s.triangle=true;
                std::copy(p,p+3,s.face);
                float longest=0;
                for (int a=0;a<3;a++) for (int b=a+1;b<3;b++) {
                    float d=std::hypot(p[a].x-p[b].x,p[a].y-p[b].y);
                    if (d>longest) { longest=d; s.a=p[a]; s.b=p[b]; }
                }
                s.height=top-std::min({p[0].z,p[1].z,p[2].z});
                collision_roads.check(s,ground,collision_results);
            }
        }
        world_ground_grid_activate(&world->neighborhood.grid);
        collision_ms+=1000.0*(SDL_GetPerformanceCounter()-begin)/SDL_GetPerformanceFrequency();
        if (!collision_gaps || collision_mesh<meshes.size()) return;
        collision_running=false; collision_stale=false;
        world_ground_grid_free(&collision_grid);
        status="Collision review: "+std::to_string(collision_results.findings.size())+" findings";
        if (collision_results.limited) status+=" (partial / work limit reached)";
        if (pending_report>=0) {
            Kind category=Kind(pending_report); pending_report=-1;
            report_export(category);
        }
    }
    void request_report(Kind category) {
        if ((category==Wall || category==KindCount) && (collision_stale || collision_running)) {
            if (!collision_running && !start_collision_scan()) return;
            pending_report=int(category);
            status="Export queued until collision review finishes";
        } else report_export(category);
    }
    void draw_collision_findings() const {
        if (!show_findings || collision_stale || tab!=Wall) return;
        for (size_t n=0;n<collision_results.findings.size();n++) {
            const auto &f=collision_results.findings[n];
            Point mid=mul(add(f.a,f.b),.5f);
            if (std::hypot(mid.x-camera.x,mid.y-camera.y)>range) continue;
            bool active=int(n)==selected_finding;
            ImU32 color=f.code==collision::Gap ? IM_COL32(255,185,60,255) : IM_COL32(255,80,80,255);
            line(f.a,f.b,color,active?5:2);
            marker(mid,color,active?collision::code_name(f.code):nullptr,active);
            if (active && f.mesh>=0 && size_t(f.mesh)<meshes.size()) {
                float face[9];
                if (phys_wall_debug_face(&meshes[f.mesh],f.part,0,face))
                    for (int p=0;p<3;p++) line(point(face+3*p),point(face+3*((p+1)%3)),color,4);
            }
        }
    }
    void collision_controls() {
        ImGui::SeparatorText("Collision review");
        if (ImGui::Checkbox("Entire source map",&collision_whole)) invalidate_collision();
        if (!collision_whole && ImGui::SliderFloat("Source radius m",&collision_radius,20,500)) invalidate_collision();
        if (ImGui::Button(collision_running?"Restart collision check":"Check collisions")) start_collision_scan();
        ImGui::SameLine();
        if (collision_running && ImGui::Button("Cancel check")) { invalidate_collision(); status="Collision check cancelled; results incomplete"; }
        if (collision_running)
            ImGui::ProgressBar(collision_gaps ? (meshes.empty()?0:float(collision_mesh)/meshes.size()) :
                                              (collision_segments.empty()?0:float(collision_segment)/collision_segments.size()),
                               {-1,0},collision_gaps?"Checking source candidates":"Checking boundaries");
        ImGui::Checkbox("Show findings in map",&show_findings);
        ImGui::TextWrapped("All enabled project walls; source checks use the selected scope. Gaps check authored and generated boundary ends. Hints require review; nothing is removed automatically.");
        if (collision_stale) ImGui::TextColored({1,.75f,.2f,1},"Results incomplete or out of date");
        ImGui::Text("%zu findings | %zu source faces | %.1f ms CPU",collision_results.findings.size(),collision_results.source_faces,collision_ms);
        if (collision_roads.roads.empty()) ImGui::TextDisabled("Road-crossing checks need a source graph");
        if (collision_results.limited) ImGui::TextColored({1,.4f,.2f,1},"PARTIAL: work / finding limit reached");
        ImGui::BeginDisabled(collision_stale);
        if (ImGui::BeginListBox("Findings",{0,110})) {
            for (size_t n=0;n<collision_results.findings.size();n++) {
                const auto &f=collision_results.findings[n];
                std::string label=std::to_string(n+1)+" "+collision::code_name(f.code)+" "+(f.item?"item "+std::to_string(f.item):"source");
                if (ImGui::Selectable(label.c_str(),selected_finding==int(n))) {
                    selected_finding=int(n); selected=f.item; selected_point=std::max(0,f.part);
                    picked_object=f.source; focus(mul(add(f.a,f.b),.5f));
                }
            }
            ImGui::EndListBox();
        }
        if (selected_finding>=0 && size_t(selected_finding)<collision_results.findings.size()) {
            const auto &f=collision_results.findings[selected_finding];
            ImGui::TextWrapped("%s",f.detail.c_str());
            ImGui::Text("Value %.3f | candidates %u",f.value,f.count);
            auto at=object_index.find(f.source);
            if (at!=object_index.end()) ImGui::TextWrapped("Asset: %s",objects[at->second].name.c_str());
        }
        ImGui::EndDisabled();
    }
    bool gizmo_target(Item &out) {
        if (auto *i = item()) {
            if (i->kind == tab && i->enabled) {
                out = *i;
                if (i->kind == Object) picked_object = i->source;
                return true;
            }
            return false;
        }
        auto at = object_index.find(picked_object);
        if (tab != Object || at == object_index.end()) return false;
        for (const auto &i : doc.items)
            if (i.kind == Object && i.source == picked_object) { out = i; return i.enabled; }
        const auto &o = objects[at->second];
        out = Item{};
        out.kind = Object; out.source = o.id; out.name = o.name; out.pos = o.center();
        return true;
    }
    int gizmo_point_index(const Item &i) const {
        if (i.points.empty() || (i.kind == Race && !gizmo_point)) return -1;
        return std::max(0, std::min(selected_point, int(i.points.size()) - 1));
    }
    float gizmo_length(Point pivot) const {
        float depth = dot(sub(pivot, camera), mouse_ray({(view_min().x + view_max().x) * .5f,
                                                      (view_min().y + view_max().y) * .5f}));
        return std::max(.1f, depth * 2 * std::tan(fov * .00872664626f) * 85 / height);
    }
    Point ring_point(Point pivot, int a, float length, float angle) const {
        return add(pivot, add(mul(axis((a + 1) % 3), length * std::cos(angle)),
                              mul(axis((a + 2) % 3), length * std::sin(angle))));
    }
    int gizmo_hit(ImVec2 mouse) {
        Item i;
        if (!gizmo_target(i)) return -1;
        int p = gizmo_point_index(i), mode = i.kind == Object ? gizmo_mode : 0;
        Point pivot = p < 0 ? i.pos : i.points[p];
        ImVec2 center;
        if (!project(pivot, center)) return -1;
        if (mode == 0 && std::hypot(mouse.x-center.x, mouse.y-center.y) < 8) return 3;
        float length = gizmo_length(pivot), best = 9;
        int hit = -1;
        for (int a = 0; a < 3; a++) {
            int segments = mode == 1 ? 64 : 1;
            for (int n = 0; n < segments; n++) {
                Point from = mode == 1 ? ring_point(pivot,a,length,n*6.283185307f/segments)
                                        : add(pivot,mul(axis(a),length*.22f));
                Point to = mode == 1 ? ring_point(pivot,a,length,(n+1)*6.283185307f/segments)
                                      : add(pivot,mul(axis(a),length));
                ImVec2 aa,bb;
                if (!project(from,aa) || !project(to,bb)) continue;
                if (mode != 1 && std::hypot(bb.x-center.x,bb.y-center.y) < 18) continue;
                float d = segment_distance(mouse.x,mouse.y,aa.x,aa.y,bb.x,bb.y);
                if (d < best) { best = d; hit = a; }
            }
        }
        return hit;
    }
    void draw_gizmo() {
        Item i;
        if (!gizmo_target(i) || pick_replacement) return;
        int p = gizmo_point_index(i), mode = i.kind == Object ? gizmo_mode : 0;
        Point pivot = p < 0 ? i.pos : i.points[p];
        ImVec2 center;
        if (!project(pivot,center)) return;
        float length = drag.active ? drag.length : gizmo_length(pivot);
        auto *draw = ImGui::GetBackgroundDrawList();
        draw->PushClipRect(view_min(), view_max(), true);
        int hover = drag.active ? drag.axis : gizmo_hit(ImGui::GetIO().MousePos);
        const ImU32 colors[] = {IM_COL32(255,85,85,255),IM_COL32(80,235,125,255),
                               IM_COL32(90,165,255,255)};
        for (int a = 0; a < 3; a++) {
            ImU32 color = a == hover ? IM_COL32(255,235,90,255) : colors[a];
            if (mode == 1) {
                for (int n = 0; n < 64; n++)
                    line(ring_point(pivot,a,length,n*6.283185307f/64),
                         ring_point(pivot,a,length,(n+1)*6.283185307f/64),color,3);
            } else {
                Point end = add(pivot,mul(axis(a),length));
                ImVec2 at;
                if (!project(end,at) || std::hypot(at.x-center.x,at.y-center.y) < 18) continue;
                line(pivot,end,IM_COL32(10,10,15,235),6);
                line(pivot,end,color,3);
                if (mode == 2) draw->AddRectFilled({at.x-5,at.y-5},{at.x+5,at.y+5},color);
                else draw->AddCircleFilled(at,5,color);
                draw->AddText({at.x+8,at.y-8},color,a==0 ? "X" : a==1 ? "Y" : "Z");
            }
        }
        if (mode == 0) {
            draw->AddCircleFilled(center,7,hover==3 ? IM_COL32(255,235,90,255) : IM_COL32_WHITE);
            draw->AddText({center.x+10,center.y+8},IM_COL32_WHITE,"XY");
        }
        draw->AddText({410,16},IM_COL32_WHITE,mode==0 ? "Move: drag an axis or XY centre"
                                             : mode==1 ? "Rotate: drag a coloured ring"
                                                       : "Uniform scale: drag an axis handle");
        draw->AddText({410,34},IM_COL32_WHITE,"1 Move   2 Rotate   3 Scale   Escape cancels drag");
        draw->PopClipRect();
    }
    bool begin_gizmo(int handle, ImVec2 mouse) {
        Item original;
        if (drag.active || !gizmo_target(original)) return false;
        Drag next;
        next.point = gizmo_point_index(original);
        next.mode = original.kind == Object ? gizmo_mode : 0;
        if (handle < 0 || handle > 3 || (handle == 3 && next.mode != 0)) return false;
        next.axis = handle;
        next.pivot = next.point < 0 ? original.pos : original.points[next.point];
        next.length = gizmo_length(next.pivot);
        Point ray = mouse_ray(mouse);
        if (next.mode == 1 || handle == 3) {
            if (!plane_hit(camera,ray,next.pivot,axis(handle==3 ? 2 : handle),next.hit)) return false;
            if (next.mode == 1) {
                next.radial = sub(next.hit,next.pivot);
                if (dot(next.radial,next.radial)<.0001f) return false;
                next.radial = normalized(next.radial);
            }
        } else if (!axis_parameter(camera,ray,next.pivot,axis(handle),next.start)) return false;
        next.before = doc; next.dirty = dirty; next.selected = selected; next.mouse = mouse;
        if (!original.id) {
            auto &i = doc.add(Object,original.name);
            uint64_t id = i.id;
            i = original; i.id = id; original = i; selected = id;
            preview_dirty = true;
        }
        selected = original.id;
        next.original = original;
        next.active = true;
        drag = std::move(next);
        if (preview_dirty && !rebuild()) {
            std::string failure = error;
            finish_gizmo(true);
            error = failure;
            return false;
        }
        return true;
    }
    void snap_object_ground(Item &i) const {
        auto at = object_index.find(i.replacement ? i.replacement : i.source);
        if (at == object_index.end()) return;
        const auto &o = objects[at->second];
        float bottom = INFINITY;
        for (int k : o.meshes) {
            const auto &m = world->neighborhood.scene.meshes[k];
            for (int v = 0; v < m.nverts; v++)
                bottom = std::min(bottom,transform(point(m.verts+5*v),o.center(),i).z);
        }
        if (std::isfinite(bottom))
            i.pos.z += ground({i.pos.x,i.pos.y,bottom}).z-bottom;
    }
    bool update_object_geometry(const Item &i) {
        auto target = object_index.find(i.source), donor = object_index.find(i.replacement ? i.replacement : i.source);
        if (target == object_index.end() || donor == object_index.end()) return false;
        auto &o = preview_objects[target->second];
        const auto &src = objects[donor->second];
        if (o.meshes.size() != src.meshes.size()) return false;
        o.lo = {INFINITY,INFINITY,INFINITY}; o.hi = {-INFINITY,-INFINITY,-INFINITY};
        for (size_t n = 0; n < o.meshes.size(); n++) {
            int k = o.meshes[n];
            auto &m = meshes[k];
            const auto &original = world->neighborhood.scene.meshes[src.meshes[n]];
            float *b = bounds.data()+4*k;
            b[0] = b[2] = INFINITY; b[1] = b[3] = -INFINITY;
            for (int v = 0; v < m.nverts; v++) {
                Point p = transform(point(original.verts+5*v),src.center(),i);
                verts[k][5*v] = p.x; verts[k][5*v+1] = p.y; verts[k][5*v+2] = p.z;
                o.lo = {std::min(o.lo.x,p.x),std::min(o.lo.y,p.y),std::min(o.lo.z,p.z)};
                o.hi = {std::max(o.hi.x,p.x),std::max(o.hi.y,p.y),std::max(o.hi.z,p.z)};
                b[0] = std::min(b[0],p.x); b[1] = std::max(b[1],p.x);
                b[2] = std::min(b[2],p.y); b[3] = std::max(b[3],p.y);
            }
            m.prop_revision++;
        }
        N2Scene scene{}; scene.meshes = meshes.data(); scene.count = int(meshes.size());
        return render_world_prop_updates(&scene,batches,nbatch,nullptr)>=0 &&
               render_world_prop_updates(&scene,glows,nglow,nullptr)>=0 &&
               render_world_prop_updates(&scene,sky,nsky,nullptr)>=0;
    }
    void update_gizmo(ImVec2 mouse) {
        Item *i = item();
        if (!drag.active || !i) return;
        if (mouse.x == drag.mouse.x && mouse.y == drag.mouse.y) return;
        drag.mouse = mouse;
        Item next = drag.original;
        Point ray = mouse_ray(mouse), hit;
        float t;
        if (drag.mode == 0) {
            Point p = drag.pivot;
            if (drag.axis == 3) {
                if (!plane_hit(camera,ray,drag.pivot,axis(2),hit)) return;
                p = add(p,sub(hit,drag.hit)); p.z = drag.pivot.z;
                if (grid_snap) { p.x = snap(p.x,grid_step); p.y = snap(p.y,grid_step); }
            } else {
                if (!axis_parameter(camera,ray,drag.pivot,axis(drag.axis),t)) return;
                Point a = axis(drag.axis);
                p = add(p,mul(a,t-drag.start));
                if (grid_snap) p = add(p,mul(a,snap(dot(p,a),grid_step)-dot(p,a)));
            }
            if (drag.point < 0) next.pos = p;
            else next.points[drag.point] = p;
        } else if (drag.mode == 1) {
            if (!plane_hit(camera,ray,drag.pivot,axis(drag.axis),hit)) return;
            Point radial = sub(hit,drag.pivot);
            if (dot(radial,radial)<.0001f) return;
            radial = normalized(radial);
            drag.angle += std::atan2(dot(axis(drag.axis),cross(drag.radial,radial)),dot(drag.radial,radial));
            drag.radial = radial;
            next.rotation = std::fabs(drag.angle) < .000001f ? drag.original.rotation
                              : rotate_world(drag.original.rotation,drag.axis,drag.angle);
        } else {
            if (!axis_parameter(camera,ray,drag.pivot,axis(drag.axis),t)) return;
            next.scale = std::max(.01f,std::min(100.f,drag.original.scale*(1+(t-drag.start)/drag.length)));
        }
        if (ground_snap && !(drag.mode == 0 && drag.axis == 2)) {
            if (next.kind == Object) snap_object_ground(next);
            else if (drag.point < 0) next.pos = ground(next.pos);
            else next.points[drag.point] = ground(next.points[drag.point]);
        }
        if (!finite(next.pos) || !finite(next.rotation) ||
            (drag.point >= 0 && !finite(next.points[drag.point]))) return;
        *i = std::move(next);
        if (i->kind == Object && !update_object_geometry(*i)) {
            finish_gizmo(true);
            error = "Live geometry update failed; drag cancelled";
            return;
        }
        if (i->kind == Light) rebuild_lights();
        invalidate_collision();
        dirty = true;
    }
    void finish_gizmo(bool cancel) {
        if (!drag.active) return;
        auto *i = item();
        bool moved = i && (dot(sub(i->pos,drag.original.pos),sub(i->pos,drag.original.pos))>1e-10f ||
                           dot(sub(i->rotation,drag.original.rotation),sub(i->rotation,drag.original.rotation))>1e-10f ||
                           i->scale!=drag.original.scale);
        if (i && drag.point >= 0)
            moved |= dot(sub(i->points[drag.point],drag.original.points[drag.point]),
                         sub(i->points[drag.point],drag.original.points[drag.point]))>1e-10f;
        if (cancel || !moved) {
            doc = std::move(drag.before); selected = drag.selected; dirty = drag.dirty;
        } else checkpoint(drag.before);
        Kind kind = drag.original.kind;
        drag.active = false;
        if (kind == Object) { preview_dirty = true; rebuild(); }
        if (kind == Light) rebuild_lights();
        status = cancel ? "Drag cancelled" : moved ? "Transform applied; one undo step" : "Selection unchanged";
    }
    void history(bool backwards) {
        auto &from = backwards ? undo : redo;
        auto &to = backwards ? redo : undo;
        if (drag.active || from.empty()) return;
        to.push_back(doc);
        doc = std::move(from.back()); from.pop_back();
        selected = picked_object = 0;
        changed();
        rebuild();
    }
    void gizmo_controls() {
        ImGui::SeparatorText("Viewport handles");
        ImGui::RadioButton("Move (1)",&gizmo_mode,0);
        ImGui::BeginDisabled(tab != Object);
        ImGui::RadioButton("Rotate (2)",&gizmo_mode,1);
        ImGui::SameLine();
        ImGui::RadioButton("Scale (3)",&gizmo_mode,2);
        ImGui::EndDisabled();
        ImGui::Checkbox("Grid snap",&grid_snap);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90);
        ImGui::InputFloat("m",&grid_step,.1f,1,"%.2f");
        if (!std::isfinite(grid_step)) grid_step = 1;
        grid_step = std::max(.01f,std::min(1000.f,grid_step));
        ImGui::Checkbox("Snap to ground",&ground_snap);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Places the object base or selected point on supporting ground");
        if (tab == Race) ImGui::Checkbox("Edit route point (off: icon/start)",&gizmo_point);
        ImGui::TextWrapped(tab == Object ? "Pick an object, then drag a handle. Scale is uniform. Escape cancels."
                                        : "Select an item or point, then drag an axis or the XY centre. Escape cancels.");
    }
    bool quit = false;
    void execute_pending() {
        if (pending == 1) {
            std::string name = tracks[pending_track];
            bool same = name == doc.map;
            Document previous = doc;
            if (!same || pending_discard) {
                doc = Document{};
                doc.map = name;
                doc.scenery = pending_event;
            }
            if (load_map(name, pending_event)) {
                track = pending_track;
                if (!same || pending_discard) {
                    selected = 0;
                    undo.clear();
                    redo.clear();
                    dirty = false;
                }
                if (doc.scenery != pending_event)
                    dirty = true;
                doc.scenery = pending_event;
            } else
                doc = std::move(previous);
        }
        if (pending == 2)
            project_load();
        if (pending == 3)
            quit = true;
        pending = 0;
        pending_discard = false;
    }
    void request(int action) {
        pending = action;
        if (dirty)
            ask = true;
        else
            execute_pending();
    }
    void properties() {
        Item *i = item();
        if (!i)
            return;
        ImGui::Separator();
        ImGui::Text("%s properties", kind_name(i->kind));
        Document before = doc;
        bool edited = false;
        char name[256];
        snprintf(name, sizeof name, "%s", i->name.c_str());
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Name", name, sizeof name)) {
            i->name = name;
            edited = true;
        }
        if (!(i->kind == Wall && i->source))
            edited |= ImGui::Checkbox(i->kind == Object ? "Visible" : "Enabled", &i->enabled);
        if (i->kind != Wall && i->kind != Region && i->kind != Path) {
            ImGui::TextUnformatted("Position XYZ (m)");
            ImGui::SetNextItemWidth(-1);
            edited |= ImGui::DragFloat3("##Position", &i->pos.x, .1f);
        }
        if (i->kind == Object) {
            ImGui::TextUnformatted("Rotation XYZ (degrees)");
            ImGui::SetNextItemWidth(-1);
            edited |= ImGui::DragFloat3("##Rotation", &i->rotation.x, .5f);
            edited |= ImGui::DragFloat("Scale", &i->scale, .01f, .01f, 100);
            ImGui::TextWrapped("Replacement copies every material slice of the chosen source "
                               "object. Apply preview to update geometry.");
        }
        if (i->kind == Wall && i->enabled) {
            edited |= ImGui::DragFloat("Height", &i->values[0], .1f, .1f, 1000);
            ImGui::TextWrapped("Continuous vertical wall from each endpoint to the next. Height is "
                               "independent of the visible prop.");
        }
        if (i->kind == Path) {
            edited |= ImGui::DragFloat("Target speed km/h", &i->values[0], 1, 5, 250);
            bool loop = i->values[1] > .5f;
            if (ImGui::Checkbox("Loop", &loop)) {
                i->values[1] = loop;
                edited = true;
            }
        }
        if (i->kind == Race) {
            int type = int(i->values[0]) - 1, laps = int(i->values[1]);
            if (ImGui::Combo("Race type", &type, race_names, 6)) {
                i->values[0] = float(type + 1);
                edited = true;
            }
            if (ImGui::InputInt("Laps", &laps)) {
                i->values[1] = float(std::max(1, std::min(99, laps)));
                edited = true;
            }
            edited |= ImGui::DragFloat("Career stage", &i->values[2], 1, 0, 5);
            ImGui::TextWrapped("Position is the map icon/start location. Route points are the "
                               "editable course preview.");
        }
        if (i->kind == Light) {
            edited |= ImGui::ColorEdit3("Lamp color", &i->color.x);
            edited |= ImGui::SliderFloat("Lamp power", &i->values[2], 0, 1);
            edited |= ImGui::DragFloat("Inner radius", &i->values[0], .1f, 0, i->values[1]);
            edited |= ImGui::DragFloat("Outer radius", &i->values[1], .5f,
                                       std::max(.1f, i->values[0]), 1000);
            ImGui::TextWrapped(
                "Authored lamp glow preview; existing baked vertex lighting is retained.");
        }
        if (i->kind == Shop) {
            int type = int(i->values[0]);
            if (ImGui::Combo("Shop category", &type, shop_names, 5)) {
                i->values[0] = float(type);
                edited = true;
            }
        }
        if (i->kind == Region)
            edited |= ImGui::DragFloat("Unlock stage", &i->values[0], 1, 0, 5);
        if (!i->points.empty() && !(i->kind == Wall && i->source && !i->enabled)) {
            ImGui::SeparatorText(i->kind == Wall ? "Boundary points" : i->kind == Region ? "District corners" : "Route points");
            selected_point = std::max(0, std::min(int(i->points.size()) - 1, selected_point));
            if (ImGui::InputInt("Point", &selected_point))
                selected_point = std::max(0, std::min(int(i->points.size()) - 1, selected_point));
            ImGui::TextUnformatted("Point XYZ (m)");
            ImGui::SetNextItemWidth(-1);
            edited |= ImGui::DragFloat3("##Point XYZ", &i->points[size_t(selected_point)].x, .1f);
            if (ImGui::Button("Insert after")) {
                Point p = i->points[size_t(selected_point)],
                      q = selected_point + 1 < int(i->points.size())
                              ? i->points[size_t(selected_point) + 1]
                              : add(p, {5, 0, 0});
                i->points.insert(i->points.begin() + selected_point + 1, mul(add(p, q), .5f));
                selected_point++;
                edited = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove point") &&
                i->points.size() > size_t(i->kind == Region ? 3 : 2)) {
                i->points.erase(i->points.begin() + selected_point);
                selected_point = std::min(selected_point, int(i->points.size()) - 1);
                edited = true;
            }
            if (ImGui::Button("Snap point to ground")) {
                i->points[size_t(selected_point)] = ground(i->points[size_t(selected_point)]);
                edited = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Focus point"))
                focus(i->points[size_t(selected_point)]);
            ImGui::TextWrapped("Shift + click in the map appends a point. Ctrl + click moves the "
                               "selected point. Click a handle to select it.");
        } else if (i->points.empty())
            ImGui::TextWrapped("Ctrl + click in the map places this item on the ground.");
        if (edited) {
            undo.push_back(std::move(before));
            if (undo.size() > 32)
                undo.erase(undo.begin());
            redo.clear();
            changed();
        }
        if (ImGui::Button("Focus selected", {-1,0}))
            focus(i->points.empty() ? i->pos : i->points[size_t(selected_point)]);
        if (i->kind == Wall && i->source) return;
        const char *remove_label = i->kind == Wall ? "Delete custom wall" : "Delete item";
        if (ImGui::Button(remove_label, {-1,0})) {
            checkpoint();
            doc.items.erase(std::remove_if(doc.items.begin(), doc.items.end(),
                                           [&](const Item &x) { return x.id == selected; }),
                            doc.items.end());
            selected = 0;
            changed();
        }
    }
    void source_list() {
        ImGui::SeparatorText("Map sources");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##Asset filter", "Search asset name...", filter, sizeof filter);
        if (ImGui::BeginListBox("##Source objects", {-1, 160})) {
            int shown = 0;
            for (const auto &o : objects) {
                if (filter[0] && o.name.find(filter) == std::string::npos) continue;
                if (!filter[0] && std::hypot(o.center().x - camera.x, o.center().y - camera.y) > range) continue;
                ImGui::PushID((void *)(uintptr_t)o.id);
                std::string label=std::to_string(int(std::hypot(o.center().x-camera.x,o.center().y-camera.y)))+" m | "+o.name;
                if (ImGui::Selectable(label.c_str(), picked_object == o.id)) {
                    picked_object = o.id;
                    selected = 0;
                    for (const auto &i : doc.items)
                        if (i.kind == tab && i.source == o.id) selected = i.id;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\nSource %016llx\nCentre %.1f, %.1f, %.1f", o.name.c_str(),
                                      (unsigned long long)o.id, o.center().x, o.center().y, o.center().z);
                ImGui::PopID();
                if (++shown == 300) { ImGui::TextDisabled("Search to narrow the list"); break; }
            }
            if (!shown) ImGui::TextDisabled("No matches nearby. Search to include the full map.");
            ImGui::EndListBox();
        }
        ImGui::TextDisabled(filter[0] ? "Search covers the loaded map" : "Nearby sources; click in the map to pick");
    }
    void source_actions() {
        auto at = object_index.find(picked_object);
        if (at == object_index.end() || (tab != Wall && tab != Object)) return;
        const auto &o = objects[at->second];
        ImGui::TextWrapped("%s", o.name.c_str());
        if (ImGui::Button("Focus source", {-1, 0})) {
            const auto &preview = preview_objects[at->second];
            focus(preview.meshes.empty() ? o.center() : preview.center());
        }
        if (tab == Wall) {
            const Item *override = nullptr;
            for (const auto &i : doc.items)
                if (i.kind == Wall && i.source == o.id) override = &i;
            bool has_override = override != nullptr;
            bool removed = override && !override->enabled;
            ImGui::TextColored(removed ? ImVec4{1,.65f,.45f,1} : ImVec4{.45f,.85f,.7f,1}, "%s",
                               removed ? "Collision removed" : has_override ? "Custom boundary active" : "Original collision");
            ImGui::TextWrapped("Affects this whole source placement. The model and driving ground stay intact.");
            ImGui::BeginDisabled(removed);
            ImGui::PushStyleColor(ImGuiCol_Button, {.42f,.19f,.20f,1});
            if (ImGui::Button("Remove source collision", {-1,0})) collision_edit(true);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (ImGui::Button(has_override ? "Edit replacement boundary" : "Create replacement boundary", {-1,0})) collision_edit();
            ImGui::BeginDisabled(!has_override);
            if (ImGui::Button("Restore original collision", {-1,0})) collision_restore();
            ImGui::EndDisabled();
            if (has_override && !removed) ImGui::TextWrapped("Replacement starts as a box; adjust its points and height below.");
        } else if (ImGui::Button("Edit selected object", {-1,0})) object_edit();
        if (ImGui::CollapsingHeader("Source details")) {
            ImGui::Text("ID %016llx", (unsigned long long)o.id);
            ImGui::Text("%zu material slices", o.meshes.size());
            ImGui::Text("Bounds %.1f %.1f %.1f", o.lo.x, o.lo.y, o.lo.z);
            ImGui::Text("to %.1f %.1f %.1f", o.hi.x, o.hi.y, o.hi.z);
        }
    }
    void toolbar() {
        const int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({float(width),toolbar_height});
        ImGui::Begin("##Editor toolbar",nullptr,flags | ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginDisabled(drag.active);
        if (ImGui::Button("Project / map")) ImGui::OpenPopup("Project / map settings");
        ImGui::SameLine();
        if (ImGui::Button("Save")) project_save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save editable .ug2map project (Ctrl+S)");
        ImGui::SameLine();
        if (ImGui::Button("Export all edits")) request_report(KindCount);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a .txt report to send for game implementation");
        ImGui::SameLine();
        ImGui::BeginDisabled(undo.empty());
        if (ImGui::Button("Undo")) history(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(redo.empty());
        if (ImGui::Button("Redo")) history(false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!preview_dirty);
        if (ImGui::Button("Apply preview")) rebuild();
        ImGui::EndDisabled();
        ImGui::SameLine();
        std::string scenery=scene_event<0?"Free roam":scene_event==0?"All scenery":"Event "+std::to_string(scene_event);
        ImGui::Text("%s | %s | %s", doc.map.c_str(), scenery.c_str(), dirty ? "Unsaved" : "No unsaved edits");
        ImGui::SetNextWindowSize({520,0},ImGuiCond_Appearing);
        if (ImGui::BeginPopup("Project / map settings")) {
            ImGui::SeparatorText("Project file");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##Project file",file,sizeof file);
            if (ImGui::Button("Open project")) request(2);
            ImGui::SameLine();
            if (ImGui::Button("Save project")) project_save();
            ImGui::TextWrapped("Save keeps your editable project. Export creates the report to send for implementation.");
            ImGui::SeparatorText("Load a map");
            ImGui::SetNextItemWidth(-90);
            ImGui::InputText("##Tracks directory",root,sizeof root);
            ImGui::SameLine();
            if (ImGui::Button("Scan")) ntracks=res_list_tracks(root,tracks,128,doc.map.c_str(),&track);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Map",ntracks?tracks[track]:"No maps found")) {
                for (int n=0;n<ntracks;n++)
                    if (ImGui::Selectable(tracks[n],track==n)) track=n;
                ImGui::EndCombo();
            }
            if (ImGui::Button("Load map") && ntracks) {
                pending_track=track; pending_event=-1; request(1);
            }
            ImGui::TextWrapped("Map loads in free roam. Use Races & icons to preview event scenery.");
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::End();
        ImGui::SetNextWindowPos({0,float(height)-status_height});
        ImGui::SetNextWindowSize({float(width),status_height});
        ImGui::Begin("##Editor status",nullptr,flags);
        if (!error.empty()) ImGui::TextColored({1,.55f,.45f,1},"%s",error.c_str());
        else ImGui::TextUnformatted(status.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",error.empty()?status.c_str():error.c_str());
        ImGui::End();
    }
    void sidebar() {
        toolbar();
        const int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos({0,toolbar_height});
        ImGui::SetNextWindowSize({panel_width(),view_size().y});
        ImGui::Begin("##Tools and selection",nullptr,flags);
        ImGui::BeginDisabled(drag.active);
        ImGui::SeparatorText("Edit mode");
        if (ImGui::BeginTable("Editor modes",2,ImGuiTableFlags_SizingStretchSame)) {
            for (int k=0;k<KindCount;k++) {
                ImGui::TableNextColumn();
                if (ImGui::Selectable(kind_name(Kind(k)),tab==k,0,{0,24})) {
                    tab=Kind(k); selected=0; picked_object=0; pick_replacement=false;
                    if (tab==Wall) walls=true;
                }
            }
            ImGui::EndTable();
        }
        if (tab==Wall) {
            ImGui::SeparatorText("Collision display");
            ImGui::Checkbox("Show existing collision",&walls);
            ImGui::Checkbox("See through scenery",&through);
            ImGui::TextColored({.05f,.85f,1,1},"Cyan  - object walls");
            ImGui::TextColored({1,.55f,.12f,1},"Orange - road / terrain walls");
            ImGui::TextColored({1,.2f,.65f,1},"Pink  - generated / custom walls");
            ImGui::TextWrapped("Click a coloured wall or source below. Edit or remove it in Selection on the right.");
            source_list();
        }
        if (tab==Object) source_list();
        if (tab == Path) {
            ImGui::Checkbox("Show source road graph", &paths);
            if (navnode<0) ImGui::TextWrapped("Click a road-graph node in the map to select a street.");
            else ImGui::Text("Selected source node: %d", navnode);
            ImGui::BeginDisabled(navnode<0);
            if (ImGui::Button("Copy connected street"))
                import_path();
            ImGui::EndDisabled();
        }
        if (tab == Race && world) {
            if (ImGui::BeginCombo("Source event",
                                  world->city.nev
                                      ? std::to_string(world->city.ev[event_index].id).c_str()
                                      : "None")) {
                for (int n = 0; n < world->city.nev; n++) {
                    const auto &e = world->city.ev[n];
                    std::string label = std::to_string(e.id) + " - " + n2_race_name(e.info.kind);
                    if (ImGui::Selectable(label.c_str(), event_index == n))
                        event_index = n;
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(!world->city.nev);
            if (ImGui::Button("Copy source race route"))
                import_race();
            if (ImGui::Button("Preview selected race scenery") && world->city.nev) {
                pending_track = -1;
                for (int n = 0; n < ntracks; n++)
                    if (doc.map == tracks[n])
                        pending_track = n;
                if (pending_track < 0) {
                    error = "Rescan the current map directory first";
                } else {
                    pending_event = world->city.ev[event_index].id;
                    request(1);
                }
            }
            ImGui::EndDisabled();
        }
        if (tab == Light && world) {
            ImGui::Checkbox("Show source lamp positions", &source_lights);
            int count = world->neighborhood.nlights;
            ImGui::Text("%d source lights", count);
            if (count) {
                ImGui::SliderInt("Source lamp", &light_index, 0, count - 1);
                if (ImGui::Button("Focus source lamp"))
                    focus(point(world->neighborhood.lights[light_index].pos));
                ImGui::SameLine();
                if (ImGui::Button("Edit source lamp")) {
                    const auto &l = world->neighborhood.lights[light_index];
                    uint64_t key = light_id(l);
                    selected = 0;
                    for (const auto &x : doc.items)
                        if (x.kind == Light && x.source == key)
                            selected = x.id;
                    if (!selected) {
                        auto &i = new_item(Light);
                        i.source = key;
                        i.pos = point(l.pos);
                        i.values[0] = l.r_in;
                        i.values[1] = l.r_out;
                        i.values[2] = float(l.rgba >> 24) / 255;
                        i.color = {float(l.rgba & 255) / 255, float((l.rgba >> 8) & 255) / 255,
                                   float((l.rgba >> 16) & 255) / 255};
                    }
                }
            }
        }
        if (tab == Region && world) {
            ImGui::Checkbox("Show source districts", &show_regions);
            if (world->city.ndist) {
                ImGui::SliderInt("Source district", &region_index, 0, world->city.ndist - 1);
                const auto &d = world->city.dist[region_index];
                ImGui::Text("%s: %d navigation nodes", d.tok, d.n);
                if (ImGui::Button("Copy district bounds")) {
                    auto &i = new_item(Region);
                    i.name = d.tok;
                    i.source = uint64_t(region_index) + 1;
                    i.pos = {d.cx, d.cy, d.medz};
                    i.points = {{d.bb[0], d.bb[2], d.medz},
                                {d.bb[1], d.bb[2], d.medz},
                                {d.bb[1], d.bb[3], d.medz},
                                {d.bb[0], d.bb[3], d.medz}};
                    focus(i.pos);
                }
            }
        }
        ImGui::SeparatorText("Project edits");
        if (tab!=Object && ImGui::Button(tab==Wall?"Add custom wall":(std::string("Add ")+kind_name(tab)).c_str(),{-1,0})) {
            new_item(tab);
            picked_object=0;
        }
        if (ImGui::BeginListBox("##Project items",{-1,120})) {
            int count=0;
            for (const auto &i:doc.items) if (i.kind==tab) {
                count++;
                std::string label=i.name;
                if (i.kind==Wall) label+=i.source?(i.enabled?" [boundary]":" [removed]"):" [custom]";
                else if (!i.enabled) label+=" [off]";
                ImGui::PushID((void *)(uintptr_t)i.id);
                if (ImGui::Selectable(label.c_str(),selected==i.id)) {
                    selected=i.id; selected_point=0;
                    if (tab==Wall || tab==Object) picked_object=i.source;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",label.c_str());
                ImGui::PopID();
            }
            if (!count) ImGui::TextDisabled("No edits in this mode yet");
            ImGui::EndListBox();
        }
        std::string export_label="Export "+std::string(kind_name(tab))+" report";
        if (ImGui::Button(export_label.c_str(),{-1,0})) request_report(tab);
        if (tab==Wall) {
            if (collision_running) ImGui::SetNextItemOpen(true,ImGuiCond_Once);
            if (ImGui::CollapsingHeader("Check for collision problems")) collision_controls();
        }
        if (ImGui::CollapsingHeader("Camera settings & controls")) {
            ImGui::PushItemWidth(-120);
            ImGui::DragFloat3("Camera XYZ",&camera.x,.5f);
            ImGui::SliderFloat("Move speed",&speed,2,250);
            ImGui::SliderFloat("View distance",&range,100,2000);
            ImGui::SliderFloat("Field of view",&fov,35,100);
            ImGui::PopItemWidth();
            ImGui::TextWrapped("Right mouse: look. WASD: move. Q/E: down/up. Shift: faster. Wheel: speed.");
        }
        ImGui::EndDisabled();
        ImGui::End();

        ImGui::SetNextWindowPos({view_max().x,toolbar_height});
        ImGui::SetNextWindowSize({panel_width(),view_size().y});
        ImGui::Begin("##Selection",nullptr,flags);
        ImGui::BeginDisabled(drag.active);
        ImGui::SeparatorText("Selection");
        source_actions();
        Item *i=item();
        if (!i && !object_index.count(picked_object)) {
            ImGui::TextWrapped("Select an item in the map or in the list on the left.");
            ImGui::TextWrapped(tab==Wall?"To remove existing collision, click a coloured wall or choose a map source. To add a new wall, use Add custom wall."
                                        :"Source entries come from the loaded map. Project edits are the changes you have authored.");
        }
        if (i) {
            ImGui::PushItemWidth(-125);
            properties();
            ImGui::PopItemWidth();
        }
        i=item();
        if (i && i->kind==Object) {
            ImGui::SeparatorText("Model replacement");
            if (ImGui::Button("Pick replacement in map",{-1,0})) pick_replacement=true;
            if (pick_replacement) ImGui::TextWrapped("Click the donor model in the viewport.");
            if (ImGui::Button("Restore original model",{-1,0})) {
                checkpoint(); item()->replacement=0; changed();
            }
        }
        if (ImGui::CollapsingHeader("Move & snapping")) gizmo_controls();
        ImGui::Separator();
        ImGui::TextDisabled("Editor preview only");
        ImGui::TextWrapped("Export your edits to send for game implementation. Original archives are kept intact.");
        if (ask) {
            ImGui::OpenPopup("Unsaved project");
            ask = false;
        }
        if (ImGui::BeginPopupModal("Unsaved project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Save your current edits before continuing?");
            if (ImGui::Button("Save and continue")) {
                if (project_save()) {
                    execute_pending();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard and continue")) {
                pending_discard = true;
                execute_pending();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pending = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::End();
    }
    void viewport_input() {
        auto &io = ImGui::GetIO();
        update_view();
        if (drag.active) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                finish_gizmo(true);
            else if (!ImGui::IsMouseDown(0))
                finish_gizmo(false);
            else
                update_gizmo(io.MousePos);
            return;
        }
        if (!io.WantCaptureKeyboard && !io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_1, false)) gizmo_mode = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_2, false)) gizmo_mode = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) gizmo_mode = 2;
        }
        if (io.WantCaptureMouse || !in_view(io.MousePos))
            return;
        if (ImGui::IsMouseClicked(0)) {
            int handle = gizmo_hit(io.MousePos);
            if (!pick_replacement && !io.KeyCtrl && !io.KeyShift && handle >= 0) {
                begin_gizmo(handle, io.MousePos);
                return;
            }
            Item *i = item();
            if (i && io.KeyShift && !i->points.empty()) {
                checkpoint();
                i = item();
                i->points.push_back(cursor_ground(io.MousePos));
                selected_point = int(i->points.size()) - 1;
                changed();
            } else if (i && io.KeyCtrl) {
                checkpoint();
                i = item();
                if (i->points.empty())
                    i->pos = cursor_ground(io.MousePos);
                else
                    i->points[size_t(selected_point)] = cursor_ground(io.MousePos);
                changed();
            } else
                pick(io.MousePos);
        }
        if (ImGui::IsMouseDown(1)) {
            yaw -= io.MouseDelta.x * .003f;
            pitch = std::max(-1.5f, std::min(1.5f, pitch - io.MouseDelta.y * .003f));
        }
        if (io.MouseWheel)
            speed = std::max(2.f, std::min(250.f, speed + io.MouseWheel * 5));
        if (!io.WantCaptureKeyboard) {
            const Uint8 *keys = SDL_GetKeyboardState(nullptr);
            float move[3] = {float(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]),
                             float(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]),
                             float(keys[SDL_SCANCODE_E] - keys[SDL_SCANCODE_Q])};
            float cam[3] = {camera.x, camera.y, camera.z}, look[3];
            render_free_camera(cam, yaw, pitch, move,
                               speed * std::min(.1f, io.DeltaTime) * (io.KeyShift ? 4 : 1), look);
            camera = point(cam);
        }
    }
};
static bool screenshot(const char *path, int width, int height) {
    std::vector<unsigned char> pixels(size_t(width) * height * 3), top(pixels.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR)
        return false;
    for (int y = 0; y < height; y++)
        memcpy(top.data() + size_t(y) * width * 3,
               pixels.data() + size_t(height - y - 1) * width * 3, size_t(width) * 3);
    write_png(path, width, height, top.data());
    return access(path, F_OK) == 0;
}
int main(int argc, char **argv) {
    Editor e;
    int frames = 0, panel = -1;
    std::string shot, projectfile, verifyfile;
    int initial_event = -1;
    bool scene_override = false, check_collisions = false;
    for (int n = 1; n < argc; n++) {
        std::string arg = argv[n];
        if (arg == "--help") {
            puts("OpenUG2 map editor\n  --tracks DIR --map NAME --project FILE.ug2map\n  --race "
                 "EVENT --camera x,y,z --view yaw,pitch (degrees)\n  --walls --frames N --shot "
                 "FILE.png\n  --check-collisions (start an editor collision scan)\n  --verify FILE.ug2map (local-asset authoring self-check)\nBuild: make "
                 "map-editor. Original archives are never written.");
            return 0;
        }
        if (arg == "--check-collisions") { check_collisions = true; continue; }
        if (arg == "--walls") {
            e.walls = true;
            continue;
        }
        if (n + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg.c_str());
            return 1;
        }
        const char *v = argv[++n];
        if (arg == "--verify")
            verifyfile = v;
        else if (arg == "--panel") {
            panel = atoi(v);
            if (panel < 0 || panel >= KindCount)
                return 1;
        } else if (arg == "--tracks") {
            if (strlen(v) >= sizeof e.root)
                return 1;
            snprintf(e.root, sizeof e.root, "%s", v);
        } else if (arg == "--map")
            e.doc.map = v;
        else if (arg == "--project")
            projectfile = v;
        else if (arg == "--race") {
            initial_event = atoi(v);
            scene_override = true;
        } else if (arg == "--frames")
            frames = atoi(v);
        else if (arg == "--shot")
            shot = v;
        else if (arg == "--camera") {
            if (sscanf(v, "%f,%f,%f", &e.camera.x, &e.camera.y, &e.camera.z) != 3 ||
                !finite(e.camera))
                return 1;
        } else if (arg == "--view") {
            if (sscanf(v, "%f,%f", &e.yaw, &e.pitch) != 2 || !std::isfinite(e.yaw) ||
                !std::isfinite(e.pitch))
                return 1;
            e.yaw *= .01745329252f;
            e.pitch *= .01745329252f;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return 1;
        }
    }
    std::string error;
    if (projectfile.size() >= sizeof e.file || verifyfile.size() >= sizeof e.file || frames < 0)
        return 1;
    if (!projectfile.empty()) {
        if (!mapedit::load(projectfile, e.doc, error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        snprintf(e.file, sizeof e.file, "%s", projectfile.c_str());
    }
    if (!scene_override)
        initial_event = e.doc.scenery;
    e.doc.scenery = initial_event;
    if (!validate(e.doc, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window *win =
        SDL_CreateWindow("OpenUG2 Map Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         e.width, e.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "Window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 1100, 720);
    SDL_GLContext context = SDL_GL_CreateContext(win);
    if (!context) {
        fprintf(stderr, "OpenGL: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImFontConfig font;
    font.SizePixels = 15;
    ImGui::GetIO().Fonts->AddFontDefault(&font);
    auto &style = ImGui::GetStyle();
    style.WindowPadding = {12,12};
    style.FramePadding = {8,6};
    style.ItemSpacing = {8,8};
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.PopupRounding = 6;
    style.Colors[ImGuiCol_WindowBg] = {.075f,.09f,.12f,1};
    style.Colors[ImGuiCol_Header] = {.16f,.29f,.39f,1};
    style.Colors[ImGuiCol_Button] = {.15f,.25f,.33f,1};
    ImGui_ImplSDL2_InitForOpenGL(win, context);
    ImGui_ImplOpenGL2_Init();
    e.shader = render_program();
    e.quad = make_quad();
    e.ntracks = res_list_tracks(e.root, e.tracks, 128, e.doc.map.c_str(), &e.track);
    if (!e.load_map(e.doc.map, initial_event)) {
        fprintf(stderr, "%s\n", e.error.c_str());
        e.quit = true;
    }
    if (!e.quit && !verifyfile.empty() && !e.verify(verifyfile.c_str())) {
        fprintf(stderr, "%s\n", e.error.c_str());
        e.quit = true;
    }
    if (!e.quit && check_collisions && !e.start_collision_scan()) {
        fprintf(stderr,"%s\n",e.error.c_str()); e.quit=true;
    }
    if (panel >= 0) {
        e.tab = Kind(panel);
        e.selected = 0;
        for (const auto &i : e.doc.items)
            if (i.kind == e.tab) {
                e.selected = i.id;
                if (e.tab == Wall || e.tab == Object) e.picked_object = i.source;
                break;
            }
    }
    int exitcode = e.quit ? 1 : 0, ticks = 0;
    while (!e.quit) {
        SDL_Event event;
        bool close = false;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                close = true;
        }
        SDL_GetWindowSize(win, &e.width, &e.height);
        if (e.width < 1 || e.height < 1) {
            SDL_Delay(20);
            continue;
        }
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        e.viewport_input();
        if (close) {
            if (e.drag.active) e.finish_gizmo(true);
            e.request(3);
        }
        if (!e.drag.active && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            e.project_save();
        if (!e.drag.active && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            e.history(!ImGui::GetIO().KeyShift);
        e.sidebar();
        e.collision_step();
        e.render();
        ImGui::Render();
        /* The existing OpenGL2 backend uses client memory, not the engine VBOs. */
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        for (int attribute = 0; attribute < 4; attribute++)
            glDisableVertexAttribArray(attribute);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        if (++ticks == frames && frames > 0) {
            if (!shot.empty() && !screenshot(shot.c_str(), e.width, e.height)) {
                fprintf(stderr, "Screenshot failed\n");
                exitcode = 1;
            }
            e.quit = true;
        }
        SDL_GL_SwapWindow(win);
    }
    e.release();
    if (e.wallvbo)
        glDeleteBuffers(1, &e.wallvbo);
    glDeleteBuffers(1, &e.quad.vbo);
    glDeleteBuffers(1, &e.quad.ibo);
    if (e.quad.nbo)
        glDeleteBuffers(1, &e.quad.nbo);
    if (e.quad.cbo)
        glDeleteBuffers(1, &e.quad.cbo);
    glDeleteProgram(e.shader.prog);
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return exitcode;
}
