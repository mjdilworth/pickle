// Utility functions for computing the keystone transformation matrix
#ifndef COMPUTE_KEYSTONE_MATRIX_H
#define COMPUTE_KEYSTONE_MATRIX_H

#include <stdbool.h>

/**
 * Computes a 3x3 transformation matrix for keystone correction based on 4 corner points
 * 
 * @param points Array of 4 corner points in normalized coordinates [0,1]
 * @param matrix Output 3x3 matrix (row-major) for keystone transformation
 */
void compute_keystone_matrix(float points[4][2], float *matrix);

#endif /* COMPUTE_KEYSTONE_MATRIX_H */