#ifndef OPENUG2_WORLD_COLLISION_RULES_H
#define OPENUG2_WORLD_COLLISION_RULES_H
#include "world_collision.h"

/* Reviewed user-authored boundary edits; model geometry stays in the asset reader.
   shortcut: exact source edits target this archive set; revalidate for other editions. */
static const float city_collision_points_0[][3] = {
    {-52.166626f, 119.454491f, 1.96345758f},
    {-44.666626f, 119.454491f, 1.96345758f},
    {-40.916626f, 119.454491f, 1.96345758f},
    {-39.041626f, 119.454491f, 1.96345758f},
    {-37.166626f, 119.454491f, 1.96345758f},
};

static const float city_collision_points_1[][3] = {
    {-359.18689f, 266.858185f, 10.0833511f},
    {-349.219574f, 266.858185f, 10.0833511f},
    {-349.219574f, 267.832977f, 10.0833511f},
    {-359.18689f, 267.832977f, 10.0833511f},
    {-359.18689f, 266.858185f, 10.0833511f},
};

static const float city_collision_points_2[][3] = {
    {-349.227173f, 267.638275f, 10.0832691f},
    {-339.252716f, 267.638275f, 10.0832691f},
    {-339.252716f, 268.477661f, 10.0832691f},
    {-349.227173f, 268.477661f, 10.0832691f},
    {-349.227173f, 267.638275f, 10.0832691f},
};

static const float city_collision_points_3[][3] = {
    {-369.134277f, 265.933075f, 10.0751619f},
    {-359.176697f, 265.933075f, 10.0751619f},
    {-359.176697f, 267.050049f, 10.0751619f},
    {-369.134277f, 267.050049f, 10.0751619f},
    {-369.134277f, 265.933075f, 10.0751619f},
};

static const float city_collision_points_4[][3] = {
    {-339.259247f, 267.708221f, 10.071372f},
    {-329.270203f, 268.281311f, 10.071372f},
    {-329.270203f, 270.213654f, 10.071372f},
    {-339.259247f, 268.975311f, 10.071372f},
    {-339.259247f, 268.281311f, 10.071372f},
};

static const float city_collision_points_5[][3] = {
    {-329.276337f, 268.796692f, 10.0484629f},
    {-319.301819f, 268.796692f, 10.0484629f},
    {-319.301819f, 269.394012f, 10.0484629f},
    {-329.276337f, 269.394012f, 10.0484629f},
    {-329.276337f, 268.796692f, 10.0484629f},
};

static const float city_collision_points_6[][3] = {
    {-329.318085f, 269.641754f, 10.0497198f},
    {-319.332886f, 269.641754f, 10.0497198f},
    {-319.332886f, 270.239075f, 10.0497198f},
    {-329.318085f, 270.239075f, 10.0497198f},
    {-329.318085f, 269.641754f, 10.0497198f},
};

static const float city_collision_points_7[][3] = {
    {-349.293671f, 268.486084f, 10.0837173f},
    {-339.306519f, 268.486084f, 10.0837173f},
    {-339.306519f, 269.326447f, 10.0837173f},
    {-349.293671f, 269.608032f, 10.0837173f},
    {-349.293671f, 268.486084f, 10.0837173f},
};

static const float city_collision_points_8[][3] = {
    {-359.266205f, 267.706757f, 10.0823078f},
    {-349.285217f, 267.706757f, 10.0823078f},
    {-349.285217f, 268.682526f, 10.0823078f},
    {-359.266205f, 268.682526f, 10.0823078f},
    {-359.266205f, 267.706757f, 10.0823078f},
};

static const float city_collision_points_9[][3] = {
    {-297.841766f, 352.173553f, 2.49093556f},
    {-299.244446f, 352.173553f, 2.49093556f},
    {-299.244446f, 352.628662f, 2.49093556f},
    {-299.69574f, 352.628662f, 2.49093556f},
    {-299.69574f, 352.173553f, 2.49093556f},
};

static const float city_collision_points_10[][3] = {
    {-309.306f, 355.710968f, 2.50725985f},
    {-305.863281f, 353.785889f, 2.50725985f},
    {-305.863281f, 354.240997f, 2.50725985f},
    {-306.314575f, 354.240997f, 2.50725985f},
    {-306.314575f, 353.785889f, 2.50725985f},
};

static const float city_collision_points_11[][3] = {
    {-314.889343f, 379.535248f, 1.99640834f},
    {-305.375305f, 375.874756f, 1.99640834f},
    {-304.945435f, 375.989349f, 1.99640834f},
    {-314.889343f, 379.491364f, 1.99640834f},
    {-304.873932f, 375.989929f, 1.99640834f},
};

static const float city_collision_points_12[][3] = {
    {-304.85437f, 376.264923f, 1.69932902f},
    {-296.832489f, 373.091461f, 1.69932902f},
    {-296.781158f, 373.266693f, 1.69932902f},
    {-305.576996f, 376.383179f, 1.69932902f},
    {-305.107178f, 376.360474f, 1.69932902f},
};

static const WCollisionRule city_collision_rules[] = {
    {0, UINT64_C(0x38dba), 0.0f, 0, NULL},
    {0, UINT64_C(0xc12b68b), 0.0f, 0, NULL},
    {0, UINT64_C(0xeed4938), 0.0f, 0, NULL},
    {0, UINT64_C(0x1403d328), 0.0f, 0, NULL},
    {0, UINT64_C(0x15732fc0), 0.0f, 0, NULL},
    {0, UINT64_C(0x19fb2eb8), 8.0f, 0, NULL},
    {0, UINT64_C(0x1cccd807), 0.0f, 0, NULL},
    {0, UINT64_C(0x1d885202), 0.0f, 0, NULL},
    {0, UINT64_C(0x27ecab57), 0.0f, 0, NULL},
    {0, UINT64_C(0x2b122739), 0.0f, 0, NULL},
    {0, UINT64_C(0x30eda5ff), 0.0f, 0, NULL},
    {0, UINT64_C(0x347e8f15), 0.0f, 0, NULL},
    {0, UINT64_C(0x40c4f117), 0.0f, 0, NULL},
    {0, UINT64_C(0x43b64229), 0.0f, 0, NULL},
    {0, UINT64_C(0x43b6422a), 0.0f, 0, NULL},
    {0, UINT64_C(0x53785ec5), 0.0f, 0, NULL},
    {0, UINT64_C(0x54efdf0b), 0.0f, 0, NULL},
    {0, UINT64_C(0x55a706c6), 0.0f, 0, NULL},
    {0, UINT64_C(0x59d4d80d), 8.0f, 0, NULL},
    {0, UINT64_C(0x67f8b815), 0.0f, 0, NULL},
    {0, UINT64_C(0x6de22877), 0.0f, 0, NULL},
    {0, UINT64_C(0x70d9cbe4), 0.0f, 0, NULL},
    {0, UINT64_C(0x7592d73e), 0.0f, 0, NULL},
    {0, UINT64_C(0x75d8b56b), 0.0f, 0, NULL},
    {0, UINT64_C(0x75d8cab2), 0.0f, 0, NULL},
    {0, UINT64_C(0x75d8cef1), 0.0f, 0, NULL},
    {0, UINT64_C(0x75d8cef2), 0.0f, 0, NULL},
    {0, UINT64_C(0x809d61db), 0.0f, 0, NULL},
    {0, UINT64_C(0x83bb71ff), 0.0f, 0, NULL},
    {0, UINT64_C(0x8ed3713e), 0.0f, 0, NULL},
    {0, UINT64_C(0x8ee589bf), 0.0f, 0, NULL},
    {0, UINT64_C(0xa8b4c566), 0.0f, 0, NULL},
    {0, UINT64_C(0xb0201657), 0.0f, 0, NULL},
    {0, UINT64_C(0xc8fd175c), 0.0f, 0, NULL},
    {0, UINT64_C(0xd241727e), 0.0f, 0, NULL},
    {0, UINT64_C(0xed176d93), 0.0f, 0, NULL},
    {0, UINT64_C(0xf0c0386e), 0.0f, 0, NULL},
    {0, UINT64_C(0xfa2a3df8), 8.0f, 0, NULL},
    {0, UINT64_C(0xfefd5383), 0.0f, 0, NULL},
    {2, 0, 8.0f, 5, city_collision_points_0},
    {1, UINT64_C(0x4152344c02be0068), 8.0f, 5, city_collision_points_1},
    {1, UINT64_C(0x4152344c071100ea), 8.0f, 5, city_collision_points_2},
    {1, UINT64_C(0x4152344c02be0069), 8.0f, 5, city_collision_points_3},
    {1, UINT64_C(0x4152344c07110118), 8.0f, 5, city_collision_points_4},
    {1, UINT64_C(0x4152344c07110109), 8.0f, 5, city_collision_points_5},
    {1, UINT64_C(0x4152344c07110116), 8.0f, 5, city_collision_points_6},
    {1, UINT64_C(0x4152344c07110100), 8.0f, 5, city_collision_points_7},
    {1, UINT64_C(0x4152344c02be006e), 8.0f, 5, city_collision_points_8},
    {1, UINT64_C(0x4152344c071100d7), 8.0f, 5, city_collision_points_9},
    {1, UINT64_C(0x4152344c071100d8), 8.0f, 5, city_collision_points_10},
    {1, UINT64_C(0x4152344c071100be), 8.0f, 5, city_collision_points_11},
    {1, UINT64_C(0x4152344c071100ba), 8.0f, 5, city_collision_points_12},
};
static const WCollisionEdits city_collision_edits = {
    "STREAML4RA", (int)(sizeof city_collision_rules / sizeof *city_collision_rules), city_collision_rules
};
#endif
