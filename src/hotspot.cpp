#include "hotspot.h"
#include "refview.h"
#include "sj.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// On-disk format (.hotspots sidecar, JSON):
//
// {
//   "version": 1,
//   "image":   "n01w02_equirect.png",      // optional sanity check
//   "hotspots": [
//     {
//       "shape":  { "type": "polygon",
//                   "points": [[u,v], [u,v], ...] },     // >= 3 points, [0,1]
//       "action": { "type": "warp",
//                   "target": "n01w03_equirect.png" }
//     },
//     {
//       "shape":  { "type": "polygon",
//                   "points": [[u,v], [u,v], ...] },
//       "action": { "type": "inspect",
//                   "target": { "position": [x, y, z],
//                               "yaw": 0.0, "pitch": 0.0,        // radians
//                               "ortho_size": 1.0 } }            // optional,
//                               default 1.0
//     }
//   ]
// }
//
// Polygons are implicitly closed (last -> first). Winding is irrelevant
// (hit test is even-odd). Seam-crossing regions are emitted by the
// converter as multiple independent hotspots sharing the same action.
//
// "warp"    lerps the camera position to another refview node.
// "inspect" lerps the camera position+orientation to a free transform
//           and switches the projection to orthographic.
// ---------------------------------------------------------------------------

// ---- internal types -------------------------------------------------------

// Mutable in-progress hotspot used during parsing. We don't know the polygon
// point count up front, so we grow dynamically and shrink to fit at the end.
struct HotspotBuild {
  HotspotShapeType type;
  float (*points)[2];
  uint32_t point_count;
  uint32_t point_cap;

  HotspotActionType action_type;
  // warp: image_name to resolve later
  char warp_target[256];
  // inspect: free camera transform
  float insp_position[3];
  float insp_yaw;
  float insp_pitch;
  float insp_ortho_size; // defaults to 1.0 if absent
  bool valid;            // false -> drop after parsing
};

static void build_free(HotspotBuild *b) {
  free(b->points);
  b->points = NULL;
  b->point_count = b->point_cap = 0;
}

// ---- public API -----------------------------------------------------------

void hotspot_free_array(Hotspot *hotspots, uint32_t count) {
  if (!hotspots)
    return;
  for (uint32_t i = 0; i < count; i++) {
    if (hotspots[i].type == HOTSPOT_SHAPE_POLYGON) {
      free(hotspots[i].polygon.points);
    }
  }
  free(hotspots);
}

void hotspot_load_for_set(RefViewSet *set) {
  if (!set || !set->views)
    return;
  uint32_t total = 0;
  for (uint32_t view_i = 0; view_i < set->count; view_i++) {
    RefView *v = &set->views[view_i];

    // load one view
    {
      char sidecar[1024];

      // make sidecar path
      {
        const char *slash_a = strrchr(v->image_name, '/');
        const char *slash_b = strrchr(v->image_name, '\\');
        const char *slash = slash_a > slash_b ? slash_a : slash_b;
        const char *dot = strrchr(v->image_name, '.');
        if (dot && (slash == NULL || dot > slash)) {
          snprintf(sidecar, sizeof(sidecar), "%s/%.*s.hotspots", set->image_dir,
                   (int)(dot - v->image_name), v->image_name);
        } else {
          snprintf(sidecar, sizeof(sidecar), "%s/%s.hotspots", set->image_dir,
                   v->image_name);
        }
      }

      size_t data_size = 0;
      void *data = SDL_LoadFile(sidecar, &data_size);
      if (!data) {
        // Missing file is silent (most views won't have hotspots).
        // Distinguish "not found" from real I/O errors? SDL_LoadFile sets an
        // error message either way; we just skip.
        continue;
      }

      HotspotBuild *builds = NULL;
      uint32_t bcount = 0;
      char image_field[256] = {};

      bool ok = false;

      // parse sidecar
      {
        if (sizeof(image_field))
          image_field[0] = '\0';

        uint32_t bcap = 0;
        int version = 0;
        bool have_version = false;

        sj_Reader r = sj_reader((const char *)data, data_size);
        sj_Value root = sj_read(&r);
        if (!sjp_expect_object(&r, root))
          goto parse_sidecar_fail;

        sj_Value key, val;
        while (sj_iter_object(&r, root, &key, &val)) {
          if (key.type != SJ_STRING) {
            sjp_set_error(&r, "expected string");
            goto parse_sidecar_fail;
          }
          if (sjp_eq(key, "version")) {
            if (!sjp_parse_int(&r, val, &version))
              goto parse_sidecar_fail;
            have_version = true;
          } else if (sjp_eq(key, "image")) {
            if (!sjp_copy_string(&r, val, image_field, sizeof(image_field)))
              goto parse_sidecar_fail;
          } else if (sjp_eq(key, "hotspots")) {
            if (!sjp_expect_array(&r, val))
              goto parse_sidecar_fail;
            sj_Value item;
            while (sj_iter_array(&r, val, &item)) {
              if (bcount == bcap) {
                bcap = bcap ? bcap * 2 : 8;
                builds = (HotspotBuild *)realloc(builds,
                                                 bcap * sizeof(HotspotBuild));
              }

              HotspotBuild *b = &builds[bcount];

              // parse hotspot
              {
                *b = {};
                b->valid = true;
                b->insp_ortho_size = 1.0f;
                if (!sjp_expect_object(&r, item))
                  goto parse_sidecar_fail;
                bool has_shape = false;
                bool has_action = false;
                sj_Value hotspot_key, hotspot_val;
                while (sj_iter_object(&r, item, &hotspot_key, &hotspot_val)) {
                  if (hotspot_key.type != SJ_STRING) {
                    sjp_set_error(&r, "expected string");
                    goto parse_sidecar_fail;
                  }
                  if (sjp_eq(hotspot_key, "shape")) {
                    // parse shape
                    {
                      if (!sjp_expect_object(&r, hotspot_val))
                        goto parse_sidecar_fail;
                      bool has_type = false;
                      bool has_points = false;
                      sj_Value shape_key, shape_val;
                      while (sj_iter_object(&r, hotspot_val, &shape_key,
                                            &shape_val)) {
                        if (shape_key.type != SJ_STRING) {
                          sjp_set_error(&r, "expected string");
                          goto parse_sidecar_fail;
                        }
                        if (sjp_eq(shape_key, "type")) {
                          char type[32];
                          if (!sjp_copy_string(&r, shape_val, type,
                                               sizeof(type)))
                            goto parse_sidecar_fail;
                          if (strcmp(type, "polygon") == 0) {
                            b->type = HOTSPOT_SHAPE_POLYGON;
                            has_type = true;
                          } else {
                            LOG(WARN|HOTSPOT|PARSE, "Hotspot: unknown shape.type \"%s\", "
                                    "dropping hotspot",
                                    type);
                            b->valid = false;
                            has_type = true; // pretend, so we don't double-log
                          }
                        } else if (sjp_eq(shape_key, "points")) {
                          // parse polygon points
                          {
                            if (!sjp_expect_array(&r, shape_val))
                              goto parse_sidecar_fail;
                            sj_Value point;
                            while (sj_iter_array(&r, shape_val, &point)) {
                              if (!sjp_expect_array(&r, point))
                                goto parse_sidecar_fail;
                              float u, v;
                              int count = 0;
                              sj_Value coord;
                              while (sj_iter_array(&r, point, &coord)) {
                                if (count == 0) {
                                  if (!sjp_parse_float(&r, coord, &u))
                                    goto parse_sidecar_fail;
                                } else if (count == 1) {
                                  if (!sjp_parse_float(&r, coord, &v))
                                    goto parse_sidecar_fail;
                                } else {
                                  sjp_set_error(&r, "array too long");
                                  goto parse_sidecar_fail;
                                }
                                count++;
                              }
                              if (r.error)
                                goto parse_sidecar_fail;
                              if (count != 2) {
                                sjp_set_error(&r, "array length mismatch");
                                goto parse_sidecar_fail;
                              }
                              if (u < 0.0f || u > 1.0f || v < 0.0f ||
                                  v > 1.0f) {
                                // Mark hotspot invalid but keep parsing the
                                // rest of the file.
                                b->valid = false;
                              }
                              // build push point
                              {
                                if (b->point_count == b->point_cap) {
                                  uint32_t newcap =
                                      b->point_cap ? b->point_cap * 2 : 8;
                                  b->points = (float (*)[2])realloc(
                                      b->points, newcap * sizeof(float[2]));
                                  b->point_cap = newcap;
                                }
                                b->points[b->point_count][0] = u;
                                b->points[b->point_count][1] = v;
                                b->point_count++;
                              }
                            }
                            if (r.error)
                              goto parse_sidecar_fail;
                          }
                          has_points = true;
                        }
                      }
                      if (r.error)
                        goto parse_sidecar_fail;
                      if (!has_type) {
                        LOG(WARN|HOTSPOT|PARSE, "Hotspot: shape missing 'type'");
                        b->valid = false;
                      }
                      if (b->type == HOTSPOT_SHAPE_POLYGON) {
                        if (!has_points) {
                          LOG(WARN|HOTSPOT|PARSE, "Hotspot: polygon missing 'points'");
                          b->valid = false;
                        }
                        if (b->point_count < 3) {
                          if (b->valid)
                            LOG(WARN|HOTSPOT|PARSE, "Hotspot: polygon has %u points (need >= "
                                    "3), dropping",
                                    b->point_count);
                          b->valid = false;
                        }
                      }
                    }
                    has_shape = true;
                  } else if (sjp_eq(hotspot_key, "action")) {
                    // parse action
                    {
                      if (!sjp_expect_object(&r, hotspot_val))
                        goto parse_sidecar_fail;
                      bool has_type = false;
                      bool has_target = false;
                      sj_Value action_key, action_val;
                      while (sj_iter_object(&r, hotspot_val, &action_key,
                                            &action_val)) {
                        if (action_key.type != SJ_STRING) {
                          sjp_set_error(&r, "expected string");
                          goto parse_sidecar_fail;
                        }
                        if (sjp_eq(action_key, "type")) {
                          char type[32];
                          if (!sjp_copy_string(&r, action_val, type,
                                               sizeof(type)))
                            goto parse_sidecar_fail;
                          if (strcmp(type, "warp") == 0) {
                            b->action_type = HOTSPOT_ACTION_WARP;
                            has_type = true;
                          } else if (strcmp(type, "inspect") == 0) {
                            b->action_type = HOTSPOT_ACTION_INSPECT;
                            has_type = true;
                          } else {
                            LOG(WARN|HOTSPOT|PARSE, "Hotspot: unknown action.type \"%s\", "
                                    "dropping hotspot",
                                    type);
                            b->valid = false;
                            has_type = true;
                          }
                        } else if (sjp_eq(action_key, "target")) {
                          if (action_val.type == SJ_STRING) {
                            if (!sjp_copy_string(&r, action_val, b->warp_target,
                                                 sizeof(b->warp_target)))
                              goto parse_sidecar_fail;
                            has_target = true;
                          } else if (action_val.type == SJ_OBJECT) {
                            // parse inspect target
                            {
                              if (!sjp_expect_object(&r, action_val))
                                goto parse_sidecar_fail;
                              bool has_position = false;
                              sj_Value target_key, target_val;
                              while (sj_iter_object(&r, action_val, &target_key,
                                                    &target_val)) {
                                if (target_key.type != SJ_STRING) {
                                  sjp_set_error(&r, "expected string");
                                  goto parse_sidecar_fail;
                                }
                                if (sjp_eq(target_key, "position")) {
                                  if (!sjp_parse_float3(&r, target_val,
                                                        b->insp_position))
                                    goto parse_sidecar_fail;
                                  has_position = true;
                                } else if (sjp_eq(target_key, "yaw")) {
                                  if (!sjp_parse_float(&r, target_val,
                                                       &b->insp_yaw))
                                    goto parse_sidecar_fail;
                                } else if (sjp_eq(target_key, "pitch")) {
                                  if (!sjp_parse_float(&r, target_val,
                                                       &b->insp_pitch))
                                    goto parse_sidecar_fail;
                                } else if (sjp_eq(target_key, "ortho_size")) {
                                  if (!sjp_parse_float(&r, target_val,
                                                       &b->insp_ortho_size))
                                    goto parse_sidecar_fail;
                                }
                              }
                              if (r.error)
                                goto parse_sidecar_fail;
                              if (!has_position) {
                                LOG(WARN|HOTSPOT|PARSE, "Hotspot: inspect target missing "
                                        "'position'");
                                b->valid = false;
                              }
                            }
                            has_target = true;
                          } else {
                            LOG(WARN|HOTSPOT|PARSE, "Hotspot: action.target must be string or "
                                    "object");
                            b->valid = false;
                          }
                        }
                      }
                      if (r.error)
                        goto parse_sidecar_fail;
                      if (!has_type) {
                        LOG(WARN|HOTSPOT|PARSE, "Hotspot: action missing 'type'");
                        b->valid = false;
                      }
                      if (!has_target) {
                        LOG(WARN|HOTSPOT|PARSE, "Hotspot: action missing 'target'");
                        b->valid = false;
                      }
                    }
                    has_action = true;
                  }
                }
                if (r.error)
                  goto parse_sidecar_fail;
                if (!has_shape || !has_action) {
                  LOG(WARN|HOTSPOT|PARSE, "Hotspot: entry missing shape or action");
                  b->valid = false;
                }
              }

              bcount++;
            }
            if (r.error)
              goto parse_sidecar_fail;
          }
        }
        if (r.error)
          goto parse_sidecar_fail;

        if (!have_version) {
          LOG(ERROR|HOTSPOT|PARSE, "Hotspot [%s]: missing 'version' field", sidecar);
          goto parse_sidecar_fail;
        }
        if (version != 1) {
          LOG(ERROR|HOTSPOT|PARSE, "Hotspot [%s]: unsupported version %d (only 1 supported)",
                  sidecar, version);
          goto parse_sidecar_fail;
        }

        ok = true;

      parse_sidecar_fail:
        if (!ok) {
          if (r.error) {
            LOG(ERROR|HOTSPOT|PARSE, "Hotspot [%s]: parse error at byte %d: %s", sidecar,
                    sjp_error_offset(&r), r.error ? r.error : "?");
          }
          if (builds) {
            for (uint32_t i = 0; i < bcount; i++)
              build_free(&builds[i]);
            free(builds);
            builds = NULL;
            bcount = 0;
          }
        }
      }

      SDL_free(data);
      if (!ok)
        continue;

      // Sanity-check `image` field if present.
      if (image_field[0]) {
        const char *my_base;

        // path basename
        {
          const char *a = strrchr(v->image_name, '/');
          const char *b = strrchr(v->image_name, '\\');
          const char *s = a > b ? a : b;
          my_base = s ? s + 1 : v->image_name;
        }

        if (strcmp(image_field, my_base) != 0 &&
            strcmp(image_field, v->image_name) != 0) {
          LOG(WARN|HOTSPOT|PARSE, "Hotspot [%s]: image field \"%s\" does not match view "
                  "\"%s\", skipping",
                  sidecar, image_field, v->image_name);
          for (uint32_t i = 0; i < bcount; i++)
            build_free(&builds[i]);
          free(builds);
          continue;
        }
      }

      // Resolve targets and produce final Hotspot array, dropping invalid
      // entries.
      Hotspot *finals = (Hotspot *)calloc(bcount, sizeof(Hotspot));
      uint32_t fcount = 0;
      for (uint32_t i = 0; i < bcount; i++) {
        HotspotBuild *b = &builds[i];
        if (!b->valid) {
          build_free(b);
          continue;
        }

        if (b->action_type == HOTSPOT_ACTION_WARP) {
          int32_t target = -1;

          // resolve target
          {
            for (uint32_t target_i = 0; target_i < set->count; target_i++) {
              if (strcmp(set->views[target_i].image_name, b->warp_target) ==
                  0) {
                target = (int32_t)target_i;
                break;
              }
            }
          }

          if (target < 0) {
            LOG(WARN|HOTSPOT|PARSE,
                "Hotspot [%s]: warp target \"%s\" not found, dropping hotspot",
                sidecar, b->warp_target);
            build_free(b);
            continue;
          }
          Hotspot *h = &finals[fcount++];
          h->type = b->type;
          h->polygon.points = b->points;
          h->polygon.count = b->point_count;
          h->action.type = HOTSPOT_ACTION_WARP;
          h->action.warp.target_view = target;
          // Detach from build so build_free won't double-free.
          b->points = NULL;
          b->point_count = b->point_cap = 0;
        } else if (b->action_type == HOTSPOT_ACTION_INSPECT) {
          Hotspot *h = &finals[fcount++];
          h->type = b->type;
          h->polygon.points = b->points;
          h->polygon.count = b->point_count;
          h->action.type = HOTSPOT_ACTION_INSPECT;
          h->action.inspect.position[0] = b->insp_position[0];
          h->action.inspect.position[1] = b->insp_position[1];
          h->action.inspect.position[2] = b->insp_position[2];
          h->action.inspect.yaw = b->insp_yaw;
          h->action.inspect.pitch = b->insp_pitch;
          h->action.inspect.ortho_size = b->insp_ortho_size;
          b->points = NULL;
          b->point_count = b->point_cap = 0;
        } else {
          build_free(b);
        }
      }
      free(builds);

      if (fcount == 0) {
        free(finals);
        LOG(WARN|HOTSPOT|PARSE, "Hotspot [%s]: no valid hotspots after parse", sidecar);
        continue;
      }

      // Shrink-to-fit (optional; tiny arrays).
      if (fcount < bcount) {
        Hotspot *shrunk = (Hotspot *)realloc(finals, fcount * sizeof(Hotspot));
        if (shrunk)
          finals = shrunk;
      }

      v->hotspots = finals;
      v->hotspot_count = fcount;
      LOG(INFO|HOTSPOT|LOAD, "Hotspot [%s]: loaded %u hotspot(s)", sidecar, fcount);
      total += v->hotspot_count;
    }
  }
  LOG(INFO|HOTSPOT|LOAD, "Hotspot: %u hotspot(s) across %u view(s)", total, set->count);
}

int32_t hotspot_pick(const RefView *view, float u, float v) {
  if (!view || !view->hotspots || view->hotspot_count == 0)
    return -1;
  for (uint32_t i = 0; i < view->hotspot_count; i++) {
    const Hotspot *h = &view->hotspots[i];
    if (h->type == HOTSPOT_SHAPE_POLYGON) {
      bool inside = false;

      // point in polygon
      {
        const float (*pts)[2] = h->polygon.points;
        uint32_t n = h->polygon.count;
        for (uint32_t pi = 0, pj = n - 1; pi < n; pj = pi++) {
          float ui = pts[pi][0], vi = pts[pi][1];
          float uj = pts[pj][0], vj = pts[pj][1];
          if ((vi > v) != (vj > v)) {
            float x_at = (uj - ui) * (v - vi) / (vj - vi) + ui;
            if (u < x_at)
              inside = !inside;
          }
        }
      }

      if (inside) {
        return (int32_t)i;
      }
    }
  }
  return -1;
}
