#include "compute_keystone_matrix.h"
#include <math.h>
#include <stdio.h>

// Matrix operations helper functions
static void matrix3x3_multiply(float *a, float *b, float *result) {
    float temp[9];
    temp[0] = a[0]*b[0] + a[1]*b[3] + a[2]*b[6];
    temp[1] = a[0]*b[1] + a[1]*b[4] + a[2]*b[7];
    temp[2] = a[0]*b[2] + a[1]*b[5] + a[2]*b[8];
    
    temp[3] = a[3]*b[0] + a[4]*b[3] + a[5]*b[6];
    temp[4] = a[3]*b[1] + a[4]*b[4] + a[5]*b[7];
    temp[5] = a[3]*b[2] + a[4]*b[5] + a[5]*b[8];
    
    temp[6] = a[6]*b[0] + a[7]*b[3] + a[8]*b[6];
    temp[7] = a[6]*b[1] + a[7]*b[4] + a[8]*b[7];
    temp[8] = a[6]*b[2] + a[7]*b[5] + a[8]*b[8];
    
    for (int i = 0; i < 9; i++) {
        result[i] = temp[i];
    }
}

static void matrix3x3_invert(float *m, float *result) {
    float det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                m[1] * (m[3] * m[8] - m[5] * m[6]) +
                m[2] * (m[3] * m[7] - m[4] * m[6]);
    
    float invDet = 1.0f / det;
    
    result[0] =  (m[4] * m[8] - m[5] * m[7]) * invDet;
    result[1] = -(m[1] * m[8] - m[2] * m[7]) * invDet;
    result[2] =  (m[1] * m[5] - m[2] * m[4]) * invDet;
    
    result[3] = -(m[3] * m[8] - m[5] * m[6]) * invDet;
    result[4] =  (m[0] * m[8] - m[2] * m[6]) * invDet;
    result[5] = -(m[0] * m[5] - m[2] * m[3]) * invDet;
    
    result[6] =  (m[3] * m[7] - m[4] * m[6]) * invDet;
    result[7] = -(m[0] * m[7] - m[1] * m[6]) * invDet;
    result[8] =  (m[0] * m[4] - m[1] * m[3]) * invDet;
}

void compute_keystone_matrix(float points[4][2], float *matrix) {
    // Standard rectangle (unit square)
    float src_x[4] = {0, 1, 0, 1}; // Top-left, top-right, bottom-left, bottom-right
    float src_y[4] = {0, 0, 1, 1};
    
    // Target quadrilateral (keystone corners)
    float dst_x[4] = {points[0][0], points[1][0], points[3][0], points[2][0]};
    float dst_y[4] = {points[0][1], points[1][1], points[3][1], points[2][1]};
    
    // Compute the homography (perspective transformation matrix)
    float a[8][8] = {{0}};
    float b[8] = {0};
    
    // Set up the system of equations
    for (int i = 0; i < 4; i++) {
        a[i*2][0] = src_x[i];
        a[i*2][1] = src_y[i];
        a[i*2][2] = 1;
        a[i*2][6] = -src_x[i] * dst_x[i];
        a[i*2][7] = -src_y[i] * dst_x[i];
        b[i*2] = dst_x[i];
        
        a[i*2+1][3] = src_x[i];
        a[i*2+1][4] = src_y[i];
        a[i*2+1][5] = 1;
        a[i*2+1][6] = -src_x[i] * dst_y[i];
        a[i*2+1][7] = -src_y[i] * dst_y[i];
        b[i*2+1] = dst_y[i];
    }
    
    // Solve the system using Gaussian elimination
    for (int i = 0; i < 8; i++) {
        // Find the maximum element in the current column
        int max_row = i;
        float max_val = fabsf(a[i][i]);
        for (int j = i + 1; j < 8; j++) {
            if (fabsf(a[j][i]) > max_val) {
                max_val = fabsf(a[j][i]);
                max_row = j;
            }
        }
        
        // Swap rows if needed
        if (max_row != i) {
            for (int j = i; j < 8; j++) {
                float temp = a[i][j];
                a[i][j] = a[max_row][j];
                a[max_row][j] = temp;
            }
            float temp = b[i];
            b[i] = b[max_row];
            b[max_row] = temp;
        }
        
        // Eliminate
        for (int j = i + 1; j < 8; j++) {
            float factor = a[j][i] / a[i][i];
            for (int k = i; k < 8; k++) {
                a[j][k] -= factor * a[i][k];
            }
            b[j] -= factor * b[i];
        }
    }
    
    // Back-substitution
    float h[8];
    for (int i = 7; i >= 0; i--) {
        h[i] = b[i];
        for (int j = i + 1; j < 8; j++) {
            h[i] -= a[i][j] * h[j];
        }
        h[i] /= a[i][i];
    }
    
    // Construct the transformation matrix
    matrix[0] = h[0];
    matrix[1] = h[1];
    matrix[2] = h[2];
    matrix[3] = h[3];
    matrix[4] = h[4];
    matrix[5] = h[5];
    matrix[6] = h[6];
    matrix[7] = h[7];
    matrix[8] = 1.0f; // Last element is always 1 for a homography
    
    // Invert the matrix (we need the inverse for mapping from dest to source)
    float invMatrix[9];
    matrix3x3_invert(matrix, invMatrix);
    
    // Copy the inverted matrix back to output
    for (int i = 0; i < 9; i++) {
        matrix[i] = invMatrix[i];
    }
}