#include "world_resident.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "physics.h"

int world_resident_policy_valid(const WResidentPolicy *policy) {
    if (!policy || !isfinite(policy->resident_radius) ||
        !isfinite(policy->draw_radius) ||
        !isfinite(policy->safety_margin) ||
        !isfinite(policy->cell_size)) return 0;
    return policy->draw_radius > 0.0f && policy->safety_margin >= 0.0f &&
           policy->cell_size > 0.0f &&
           policy->resident_radius >= policy->draw_radius +
                                      policy->cell_size +
                                      policy->safety_margin;
}

int world_resident_target(const WResidentPolicy *policy,
                          float player_x, float player_y,
                          float active_x, float active_y,
                          float out_center[2]) {
    if (!world_resident_policy_valid(policy) || !out_center ||
        !isfinite(player_x) || !isfinite(player_y) ||
        !isfinite(active_x) || !isfinite(active_y)) return 0;

    float dx = player_x - active_x;
    float dy = player_y - active_y;
    float trigger2 = policy->cell_size * policy->cell_size;
    if (dx * dx + dy * dy <= trigger2) return 0;

    float target_x = floorf(player_x / policy->cell_size + 0.5f) *
                     policy->cell_size;
    float target_y = floorf(player_y / policy->cell_size + 0.5f) *
                     policy->cell_size;
    if (target_x == active_x && target_y == active_y) return 0;

    out_center[0] = target_x;
    out_center[1] = target_y;
    return 1;
}

int world_resident_route_point(const WResidentPolicy *policy,
                               const WorldResident *active,
                               const WorldCity *city,
                               float from_x, float from_y, float reference_z,
                               const float (*visited)[2], int visited_count,
                               float out_pos[3], float out_center[2]) {
    if (!world_resident_policy_valid(policy) || !active || !city ||
        !city->nav || city->nnav <= 0 || !out_pos || !out_center ||
        !isfinite(from_x) || !isfinite(from_y) || !isfinite(reference_z) ||
        visited_count < 0 || (visited_count && !visited)) return 0;

    int best = -1, best_cat = WSURF_NONE;
    float best_d2 = 1e30f, best_z = 0.0f, best_center[2] = {0.0f, 0.0f};
    float max_d2 = policy->draw_radius * policy->draw_radius;
    for (int i = 0; i < city->nnav; i++) {
        float x = city->nav[i*2], y = city->nav[i*2+1];
        float dx = x - from_x, dy = y - from_y;
        float d2 = dx*dx + dy*dy;
        if (!isfinite(x) || !isfinite(y) || d2 > max_d2) continue;
        if (visited_count >= 2) {
            float route_dx = active->center[0] - visited[visited_count-2][0];
            float route_dy = active->center[1] - visited[visited_count-2][1];
            float dot = dx*route_dx + dy*route_dy;
            float route2 = route_dx*route_dx + route_dy*route_dy;
            if (dot <= 0.5f * sqrtf(d2 * route2)) continue;
        }
        float center[2];
        if (!world_resident_target(policy, x, y,
                                   active->center[0], active->center[1], center))
            continue;
        int seen = 0;
        for (int j = 0; j < visited_count; j++)
            if (center[0] == visited[j][0] && center[1] == visited[j][1]) {
                seen = 1;
                break;
            }
        if (seen) continue;
        WGroundHit hit;
        int cat = world_ground_hit(&active->world.scene, x, y, reference_z, &hit);
        if (cat == WSURF_NONE) continue;
        if (best < 0 || cat < best_cat || (cat == best_cat && d2 < best_d2)) {
            best = i;
            best_cat = cat;
            best_d2 = d2;
            best_z = hit.z;
            best_center[0] = center[0];
            best_center[1] = center[1];
        }
    }
    if (best < 0) return 0;
    out_pos[0] = city->nav[best*2];
    out_pos[1] = city->nav[best*2+1];
    out_pos[2] = best_z;
    out_center[0] = best_center[0];
    out_center[1] = best_center[1];
    return 1;
}

void world_resident_resources_free(WorldResidentResources *resources) {
    if (!resources) return;
    if (resources->terrain_texture) {
        int shared = 0;
        for (int i = 0; i < resources->texture_count; i++)
            if (resources->textures &&
                resources->textures[i] == resources->terrain_texture) shared = 1;
        if (!shared) glDeleteTextures(1, &resources->terrain_texture);
    }
    if (resources->textures && resources->texture_count > 0)
        glDeleteTextures(resources->texture_count, resources->textures);
    render_batch_array_free(&resources->ordinary, &resources->ordinary_count);
    render_batch_array_free(&resources->sky, &resources->sky_count);
    render_batch_array_free(&resources->glow, &resources->glow_count);
    render_batch_array_free(&resources->vista, &resources->vista_count);
    free(resources->texture_keys);
    free(resources->textures);
    free(resources->texture_modes);
    free(resources->mesh_textures);
    free(resources->mesh_modes);
    free(resources->mesh_batch);
    free(resources->debug_batches);
    free(resources->vista_mesh);
    free(resources->obstacles);
    free(resources->obstacle_z);
    free(resources->obstacle_src);
    memset(resources, 0, sizeof *resources);
}

static int resident_vista_batches(WorldResidentResources *resources,
                                  const WorldNeighborhood *neighborhood) {
    for (int i = 0; i < neighborhood->vista.count; i++) {
        N2Scene one = { &neighborhood->vista.meshes[i], 1, 1 };
        GLuint texture = 0;
        for (int j = 0; j < resources->texture_count; j++)
            if (resources->texture_keys[j] == one.meshes[0].texkey) {
                texture = resources->textures[j];
                break;
            }
        N2Batch *part = NULL;
        int count = upload_cat_batches(&one, one.meshes[0].cat,
                                       &texture, &part, NULL);
        if (count <= 0) { free(part); continue; }
        int total = resources->vista_count + count;
        N2Batch *batches = (N2Batch *)realloc(
            resources->vista, (size_t)total * sizeof *batches);
        int *sources = (int *)realloc(resources->vista_mesh,
                                      (size_t)total * sizeof *sources);
        if (!batches || !sources) {
            if (batches) resources->vista = batches;
            if (sources) resources->vista_mesh = sources;
            render_batch_array_free(&part, &count);
            return 0;
        }
        resources->vista = batches;
        resources->vista_mesh = sources;
        memcpy(resources->vista + resources->vista_count, part,
               (size_t)count * sizeof *part);
        for (int j = 0; j < count; j++)
            resources->vista_mesh[resources->vista_count+j] = i;
        resources->vista_count = total;
        free(part); /* handles moved into resources->vista */
    }
    return 1;
}

static uint32_t wrb_ms(struct timespec *a, struct timespec *b) {
    return (uint32_t)((b->tv_sec - a->tv_sec) * 1000 +
                      (b->tv_nsec - a->tv_nsec) / 1000000);
}

int world_resident_resources_build(WorldResidentResources *resources,
                                   WorldNeighborhood *neighborhood,
                                   WResidentBuildTiming *timing) {
    if (!resources || !neighborhood || neighborhood->scene.count <= 0 ||
        !neighborhood->scene.meshes || !neighborhood->mbb) return 0;
    world_resident_resources_free(resources);
    while (glGetError() != GL_NO_ERROR) {}
    struct timespec rt0, rt1;

    int mesh_count = neighborhood->scene.count;
    int texture_cap = mesh_count + neighborhood->vista.count +
                      (neighborhood->nlights > 0 ? 1 : 0);
    if (texture_cap <= 0) texture_cap = 1;
    resources->texture_keys = (uint32_t *)calloc(
        (size_t)texture_cap, sizeof *resources->texture_keys);
    resources->textures = (GLuint *)calloc(
        (size_t)texture_cap, sizeof *resources->textures);
    resources->texture_modes = (unsigned char *)calloc(
        (size_t)texture_cap, sizeof *resources->texture_modes);
    resources->mesh_textures = (GLuint *)calloc(
        (size_t)mesh_count, sizeof *resources->mesh_textures);
    resources->mesh_modes = (unsigned char *)calloc(
        (size_t)mesh_count, sizeof *resources->mesh_modes);
    resources->mesh_batch = (int *)malloc(
        (size_t)mesh_count * sizeof *resources->mesh_batch);
    if (!resources->texture_keys || !resources->textures ||
        !resources->texture_modes || !resources->mesh_textures ||
        !resources->mesh_modes || !resources->mesh_batch) goto fail;
    resources->mesh_count = mesh_count;

    if (timing) clock_gettime(CLOCK_MONOTONIC, &rt0);
    World facade;
    memset(&facade, 0, sizeof facade);
    facade.neighborhood = *neighborhood;
    resources->texture_count = world_bind_textures(
        &facade, resources->texture_keys, resources->textures,
        resources->texture_modes, texture_cap);
    *neighborhood = facade.neighborhood;
    for (int i = 0; i < resources->texture_count; i++)
        if (!resources->textures[i]) goto fail;

    for (int i = 0; i < mesh_count; i++) {
        resources->mesh_batch[i] = -1;
        for (int j = 0; j < resources->texture_count; j++)
            if (resources->texture_keys[j] == neighborhood->scene.meshes[i].texkey) {
                resources->mesh_textures[i] = resources->textures[j];
                resources->mesh_modes[i] = (unsigned char)n2_world_draw_mode(
                    &neighborhood->scene.meshes[i], resources->texture_modes[j]);
                break;
            }
    }
    if (neighborhood->have_grass)
        resources->terrain_texture = upload_tex(&neighborhood->grass);
    if (timing) { clock_gettime(CLOCK_MONOTONIC, &rt1);
                  timing->textures_ms = wrb_ms(&rt0, &rt1); rt0 = rt1; }

    resources->sky_count = upload_cat_batches(
        &neighborhood->scene, N2_SKY, resources->mesh_textures,
        &resources->sky, NULL);
    resources->glow_count = upload_cat_batches(
        &neighborhood->scene, N2_GLOW, resources->mesh_textures,
        &resources->glow, NULL);
    resources->ordinary_count = upload_world_batches(
        &neighborhood->scene, (const float (*)[4])neighborhood->mbb,
        resources->mesh_textures, resources->terrain_texture,
        &resources->ordinary, NULL, resources->mesh_batch,
        resources->mesh_modes);
    if (resources->ordinary_count <= 0 || !resources->ordinary) goto fail;
    if (!resident_vista_batches(resources, neighborhood)) goto fail;

    resources->debug_batches = (WorldMeshBatch *)calloc(
        (size_t)resources->ordinary_count, sizeof *resources->debug_batches);
    if (!resources->debug_batches) goto fail;
    for (int i = 0; i < resources->ordinary_count; i++) {
        resources->debug_batches[i].vbo = resources->ordinary[i].vbo;
        resources->debug_batches[i].ibo = resources->ordinary[i].ibo;
        resources->debug_batches[i].index_count =
            (uint32_t)resources->ordinary[i].index_count;
        resources->debug_batches[i].chunk_id = (uint32_t)i;
    }
    if (timing) { clock_gettime(CLOCK_MONOTONIC, &rt1);
                  timing->batches_ms = wrb_ms(&rt0, &rt1); rt0 = rt1; }

    int obstacle_cap = mesh_count;
    resources->obstacles = (float (*)[4])calloc(
        (size_t)obstacle_cap, sizeof *resources->obstacles);
    resources->obstacle_z = (float (*)[2])calloc(
        (size_t)obstacle_cap, sizeof *resources->obstacle_z);
    resources->obstacle_src = (int *)calloc(
        (size_t)obstacle_cap, sizeof *resources->obstacle_src);
    if (!resources->obstacles || !resources->obstacle_z ||
        !resources->obstacle_src) goto fail;
    resources->obstacle_count = phys_collect_walls(
        &neighborhood->scene, resources->obstacles,
        resources->obstacle_src, resources->obstacle_z, obstacle_cap);
    if (timing) { clock_gettime(CLOCK_MONOTONIC, &rt1);
                  timing->collision_ms = wrb_ms(&rt0, &rt1); }
    if (glGetError() != GL_NO_ERROR) goto fail;
    return 1;

fail:
    world_resident_resources_free(resources);
    return 0;
}

int world_resident_validate_cpu(const WorldResident *resident,
                                float player_x, float player_y, float player_z) {
    if (!resident || !isfinite(player_x) || !isfinite(player_y) ||
        !isfinite(player_z) || !isfinite(resident->center[0]) ||
        !isfinite(resident->center[1]) || !isfinite(resident->radius) ||
        resident->radius <= 0.0f) return 0;
    const WorldNeighborhood *world = &resident->world;
    const N2Scene *scene = &world->scene;
    if (!scene->meshes || scene->count <= 0 || !world->mbb ||
        !world->grid.start || !world->grid.list ||
        world->grid.gw <= 0 || world->grid.gh <= 0 ||
        world->grid.meshes != scene->meshes ||
        world->inst_stats.rejected_meshes != 0 ||
        world->center[0] != resident->center[0] ||
        world->center[1] != resident->center[1] ||
        world->radius != resident->radius) return 0;

    int ground_meshes = 0;
    for (int i = 0; i < scene->count; i++) {
        const N2Mesh *mesh = &scene->meshes[i];
        if (!mesh->verts || !mesh->idx || mesh->nverts <= 0 || mesh->nidx <= 0)
            return 0;
        if (mesh->cat == N2_ROAD || mesh->cat == N2_TERRAIN) ground_meshes++;
        for (int j = 0; j < mesh->nverts; j++)
            for (int k = 0; k < 3; k++)
                if (!isfinite(mesh->verts[j*5+k])) return 0;
        for (int j = 0; j < mesh->nidx; j++)
            if (mesh->idx[j] >= mesh->nverts) return 0;
        for (int j = 0; j < 4; j++)
            if (!isfinite(world->mbb[i][j])) return 0;
    }
    if (!ground_meshes) return 0;

    WGroundHit hit;
    return world_ground_hit(scene, player_x, player_y, player_z, &hit) !=
           WSURF_NONE;
}

int world_resident_prepare(WorldResident *candidate,
                           const WResidentBuildArgs *args,
                           float center_x, float center_y,
                           WResidentBuildTiming *timing) {
    if (!candidate || !args || !args->track_root || !args->trackname ||
        !world_resident_policy_valid(&args->policy) ||
        !isfinite(center_x) || !isfinite(center_y)) return 0;
    if (timing) memset(timing, 0, sizeof *timing);
    candidate->center[0] = center_x;
    candidate->center[1] = center_y;
    candidate->radius = args->policy.resident_radius;
    struct timespec t0, t1;
    if (timing) clock_gettime(CLOCK_MONOTONIC, &t0);

    WLoadOptions options = {
        1, center_x, center_y, args->policy.resident_radius,
        args->scenery_event
    };
    if (!world_neighborhood_load(&candidate->world, args->track_root,
                                 args->trackname, &options)) return 0;
    for (int i = 0; i < candidate->world.scene.count; i++)
        if (candidate->world.scene.meshes[i].cat == N2_SKY)
            candidate->world.scene.meshes[i].texkey = n2_sky_remap_key(
                args->sky_profile, candidate->world.scene.meshes[i].texkey);
    if (timing) { clock_gettime(CLOCK_MONOTONIC, &t1);
                  timing->neighborhood_ms = wrb_ms(&t0, &t1); }
    return 1;
}

int world_resident_finish(WorldResident *candidate, float x, float y, float z,
                          WResidentBuildTiming *timing) {
    struct timespec t0, t1;
    if (timing) clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!world_resident_validate_cpu(candidate, x, y, z)) return 0;
    if (timing) { clock_gettime(CLOCK_MONOTONIC, &t1);
                  timing->validate_ms = wrb_ms(&t0, &t1); t0 = t1; }

    if (!world_resident_resources_build(&candidate->resources,
                                        &candidate->world, timing)) return 0;
    if (timing) timing->total_ms = timing->neighborhood_ms +
                    timing->validate_ms + timing->textures_ms +
                    timing->batches_ms + timing->collision_ms;
    return 1;
}

int world_resident_build(WorldResident *candidate,
                         const WResidentBuildArgs *args,
                         float center_x, float center_y,
                         float player_x, float player_y, float player_z,
                         WResidentBuildTiming *timing) {
    if (!candidate) return 0;
    memset(candidate, 0, sizeof *candidate);
    if (world_resident_prepare(candidate, args, center_x, center_y, timing) &&
        world_resident_finish(candidate, player_x, player_y, player_z, timing))
        return 1;
    world_resident_resources_free(&candidate->resources);
    world_neighborhood_free(&candidate->world);
    memset(candidate, 0, sizeof *candidate);
    return 0;
}

struct WResidentJob {
    SDL_Thread *thread;
    SDL_atomic_t done;
    WResidentBuildArgs args;
    char *root, *track;
    WorldResident *candidate;
    WResidentBuildTiming timing;
    int ok;
};

/* Loader-only parser scratch is shared within each translation unit. Keep
 * ownership until join, including completed-but-not-yet-consumed requests. */
static SDL_atomic_t resident_loader_busy;

static int resident_worker(void *data) {
    WResidentJob *job = data;
    job->ok = world_resident_prepare(job->candidate, &job->args,
                                     job->candidate->center[0],
                                     job->candidate->center[1], &job->timing);
    /* SDL's atomic release/acquire publishes all candidate writes. No GL,
     * active-scene queries or partial-output cleanup occurs on this thread. */
    SDL_AtomicSet(&job->done, 1);
    return 0;
}

static void resident_job_free(WResidentJob *job) {
    free(job->root); free(job->track); free(job);
}

int world_resident_job_start(WResidentJob **slot, const WResidentBuildArgs *args,
                             float center_x, float center_y) {
    if (!slot || *slot || !args || !args->track_root || !args->trackname ||
        !world_resident_policy_valid(&args->policy) ||
        !isfinite(center_x) || !isfinite(center_y)) return 0;
    WResidentJob *job = calloc(1, sizeof *job);
    if (!job) return 0;
    job->root = malloc(strlen(args->track_root) + 1);
    job->track = malloc(strlen(args->trackname) + 1);
    job->candidate = calloc(1, sizeof *job->candidate);
    if (!job->root || !job->track || !job->candidate) {
        free(job->candidate); resident_job_free(job); return 0;
    }
    strcpy(job->root, args->track_root);
    strcpy(job->track, args->trackname);
    job->args = *args;
    job->args.track_root = job->root;
    job->args.trackname = job->track;
    job->candidate->center[0] = center_x;
    job->candidate->center[1] = center_y;
    SDL_AtomicSet(&job->done, 0);
    if (!SDL_AtomicCAS(&resident_loader_busy, 0, 1)) {
        free(job->candidate); resident_job_free(job); return 0;
    }
    job->thread = SDL_CreateThread(resident_worker, "world-prepare", job);
    if (!job->thread) {
        SDL_AtomicSet(&resident_loader_busy, 0);
        free(job->candidate); resident_job_free(job); return 0;
    }
    *slot = job;
    return 1;
}

int world_resident_job_take(WResidentJob **slot, WorldResident **candidate,
                            WResidentBuildTiming *timing) {
    if (!slot || !*slot || !candidate || !SDL_AtomicGet(&(*slot)->done)) return 0;
    WResidentJob *job = *slot;
    SDL_WaitThread(job->thread, NULL);
    *candidate = job->candidate;
    if (timing) *timing = job->timing;
    int result = job->ok ? 1 : -1;
    resident_job_free(job);
    SDL_AtomicSet(&resident_loader_busy, 0);
    *slot = NULL;
    return result;
}

void world_resident_job_cancel(WResidentJob **slot) {
    if (!slot || !*slot) return;
    WResidentJob *job = *slot;
    SDL_WaitThread(job->thread, NULL);
    world_resident_free(job->candidate);
    resident_job_free(job);
    SDL_AtomicSet(&resident_loader_busy, 0);
    *slot = NULL;
}

void world_resident_activate(WorldResident **active,
                             WorldResident **candidate) {
    if (!active || !candidate || !*candidate) return;
    WorldResident *next = *candidate;
    WorldResident *previous = *active;
    next->generation = previous ? previous->generation + 1 : 1;
    *active = next;
    *candidate = previous;
    world_ground_grid_activate(&next->world.grid);
}

void world_resident_free(WorldResident *resident) {
    if (!resident) return;
    world_resident_resources_free(&resident->resources);
    world_neighborhood_free(&resident->world);
    free(resident);
}
