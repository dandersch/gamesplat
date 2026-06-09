#pragma once

struct GaussianScene;

bool gaussian_load_ply(const char* path, GaussianScene* scene);
bool gaussian_load_sog(const char* path, GaussianScene* scene);
