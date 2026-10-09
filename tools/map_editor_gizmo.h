#ifndef OPENUG2_MAP_EDITOR_GIZMO_H
#define OPENUG2_MAP_EDITOR_GIZMO_H
#include "map_editor_document.h"

namespace mapedit { namespace gizmo {
inline Point sub(Point a, Point b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Point add(Point a, Point b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Point mul(Point p, float s) { return {p.x*s, p.y*s, p.z*s}; }
inline float dot(Point a, Point b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Point cross(Point a, Point b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
inline Point normalized(Point p) { return mul(p, 1/std::max(.00001f, std::sqrt(dot(p,p)))); }
inline Point axis(int n) { return {float(n==0), float(n==1), float(n==2)}; }
inline float snap(float v, float step) { return step>0 ? std::round(v/step)*step : v; }
inline bool axis_parameter(Point camera, Point ray, Point pivot, Point direction, float &t) {
    float c=dot(ray,direction), denominator=1-c*c;
    if (denominator<.0001f) return false;
    Point d=sub(camera,pivot);
    t=(dot(d,direction)-c*dot(d,ray))/denominator;
    return std::isfinite(t);
}
inline bool plane_hit(Point camera, Point ray, Point pivot, Point normal, Point &hit) {
    float denominator=dot(ray,normal);
    if (std::fabs(denominator)<.0001f) return false;
    float t=dot(sub(pivot,camera),normal)/denominator;
    if (!std::isfinite(t) || t<=0) return false;
    hit=add(camera,mul(ray,t));
    return finite(hit);
}
inline Point rotate_vector(Point p, Point a, float angle) {
    return add(add(mul(p,std::cos(angle)),mul(cross(a,p),std::sin(angle))),
               mul(a,dot(a,p)*(1-std::cos(angle))));
}
inline Point rotate_world(Point rotation, int n, float angle) {
    Item basis; basis.rotation=rotation;
    Point a=axis(n), x=rotate_vector(transform({1,0,0},{},basis),a,angle),
          y=rotate_vector(transform({0,1,0},{},basis),a,angle),
          z=rotate_vector(transform({0,0,1},{},basis),a,angle);
    float ry=std::asin(std::max(-1.f,std::min(1.f,-x.z)));
    float rx,rz;
    if (std::fabs(std::cos(ry))>.0001f) {
        rx=std::atan2(y.z,z.z); rz=std::atan2(x.y,x.x);
    } else {
        rx=std::atan2(-z.y,y.y); rz=0;
    }
    return mul({rx,ry,rz},57.295779513f);
}
inline float segment_distance(float px,float py,float ax,float ay,float bx,float by) {
    float dx=bx-ax,dy=by-ay,denominator=dx*dx+dy*dy;
    float t=denominator>.001f ? std::max(0.f,std::min(1.f,((px-ax)*dx+(py-ay)*dy)/denominator)) : 0;
    return std::hypot(px-ax-t*dx,py-ay-t*dy);
}
} }
#endif
