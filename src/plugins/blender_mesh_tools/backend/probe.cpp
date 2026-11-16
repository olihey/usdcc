// SPDX-License-Identifier: BSD-3-Clause
#include "mesh_api.h"
#include <iostream>

int main() {
    std::cout << usdcc_blender_mesh_build() << '\n';
    char report[512];
    for (const unsigned count : {1u, 100u, 10u}) {
        if (usdcc_blender_mesh_probe(count, report, sizeof(report))) {
            std::cerr << "FAIL: " << report << '\n';
            return 1;
        }
        std::cout << report << '\n';
        usdcc_blender_mesh_shutdown(); // verify the persistent executor can restart
    }
    if (!usdcc_blender_mesh_probe(0, report, sizeof(report)) ||
        !usdcc_blender_mesh_probe(1, nullptr, 0)) return 1;
    char tiny = 'x';
    if (!usdcc_blender_mesh_probe(1, &tiny, 1) || tiny != '\0') return 1;
    return 0;
}
