/*
 * Fourier transformed AO pair
 * \int e^{-i Gv \cdot r}  i(r) * j(r) dr^3
 *
 * eval_gz, invh, gxyz, gs:
 * - when eval_gz is    GTO_Gv_uniform_orth
 *   > invh is diagonal 3x3 matrix
 *   > Gv k-space grids = 2\pi * dot(invh.T,gxyz)
 *   > gxyz[3,nGv] = (kx[:nGv], ky[:nGv], kz[:nGv])
 *   > gs[3]: The number of *positive* G-vectors along each direction.
 * - when eval_gz is    GTO_Gv_uniform_nonorth
 *   > invh is 3x3 matrix = scipy.linalg.inv(cell.lattice_vectors)
 *     cell lattice vectors: a "three-column" array [a1|a2|a3]
 *   > Gv k-space grids = 2\pi * dot(invh.T,gxyz)
 *   > gxyz[3,nGv] = (kx[:nGv], ky[:nGv], kz[:nGv])
 *   > gs[3]: The number of *positive* G-vectors along each direction.
 * - when eval_gz is    GTO_Gv_general
 *   only Gv is needed
 * - when eval_gz is    GTO_Gv_nonuniform_orth
 *   > invh is the basic G value for each cartesian component
 *     Gx = invh[:gs[0]]
 *     Gy = invh[gs[0]:gs[0]+gs[1]]
 *     Gz = invh[gs[0]+gs[1]:]
 *   > gs[3]: Number of basic G values along each direction.
 *   > gxyz[3,nGv] are used to index the basic G value
 *   > Gv is not used
 */

/*
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <complex.h>
#include "config.h"
#include "cint.h"

#define SQRTPI          1.7724538509055160272981674833411451
#define EXPCUTOFF       100
#define NCTRMAX         72

typedef struct {
        const int *atm;
        const int *bas;
        const double *env;
        const int *shls;
        int natm;
        int nbas;

        int i_l;
        int j_l;
        int k_l;
        int l_l;
        int i_prim;
        int j_prim;
        int k_prim;
        int l_prim;
        int i_ctr;
        int j_ctr;
        int k_ctr;
        int l_ctr;
        int nfi;  // number of cartesion components
        int nfj;
        int nfk;
        int nfl;
        int nf;  // = nfi*nfj*nfk*nfl;
        int _padding1;
        const double *ri;
        const double *rj;
        const double *rk;
        const double *rl;
        double common_factor;

        int gbits;
        int ncomp_e1; // = 1 if spin free, = 4 when spin included, it
        int ncomp_e2; // corresponds to POSX,POSY,POSZ,POS1, see cint_const.h
        int ncomp_tensor; // e.g. = 3 for gradients

        /* values may diff based on the g0_2d4d algorithm */
        int li_ceil; // power of x, == i_l if nabla is involved, otherwise == i_l
        int lj_ceil;
        int lk_ceil;
        int ll_ceil;
        int g_stride_i; // nrys_roots * shift of (i++,k,l,j)
        int g_stride_k; // nrys_roots * shift of (i,k++,l,j)
        int g_stride_l; // nrys_roots * shift of (i,k,l++,j)
        int g_stride_j; // nrys_roots * shift of (i,k,l,j++)
        int nrys_roots;
        int g_size;

        int g2d_ijmax;
        int g2d_klmax;
        const double *rx_in_rijrx;
        const double *rx_in_rklrx;
        double rirj[3]; // diff by an sign in different g0_2d4d algorithm
        double rkrl[3];

        void (*f_rr)();  // function to compute recursive relation

        /* */
        void (*f_gout)();

        /* values are assigned during calculation */
        double ai;
        double aj;
        double ak;
        double al;
        double aij;
        double akl;
        double rij[3];
        double rijrx[3];
        double rkl[3];
        double rklrx[3];
        int *idx;
} CINTEnvVars;

void CINTg1e_index_xyz(int *idx, const CINTEnvVars *envs);
double CINTsquare_dist(const double *r1, const double *r2);
double CINTcommon_fac_sp(int l);
int CINTinit_int1e_EnvVars(CINTEnvVars *envs, const int *ng, const int *shls,
                           const int *atm, const int natm,
                           const int *bas, const int nbas, const double *env);

static void init1e_envs(CINTEnvVars *envs, const int *shls,
                        const int *atm, const int natm,
                        const int *bas, const int nbas, const double *env)
{
        int ng[] = {0, 0, 0, 0, 0, 0, 0, 0};
        CINTinit_int1e_EnvVars(envs, ng, shls, atm, natm, bas, nbas, env);

        if (envs->li_ceil < envs->lj_ceil) {
                int dli = envs->li_ceil + 1;
                int dlj = envs->li_ceil + envs->lj_ceil + 1;
                envs->g_stride_i = 1;
                envs->g_stride_j = dli;
                envs->g_size     = dli * dlj;
        }
}


static const int _LEN_CART[] = {
        1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 66, 78, 91, 105, 120, 136
};
static const int _CUM_LEN_CART[] = {
        1, 4, 10, 20, 35, 56, 84, 120, 165, 220, 286, 364, 455, 560, 680, 816,
};

/*
 * WHEREX_IF_L_INC1 = [xyz2addr(x,y,z) for x,y,z in loopcart(L_MAX) if x > 0]
 * WHEREY_IF_L_INC1 = [xyz2addr(x,y,z) for x,y,z in loopcart(L_MAX) if y > 0]
 * WHEREZ_IF_L_INC1 = [xyz2addr(x,y,z) for x,y,z in loopcart(L_MAX) if z > 0]
 */
static const int _UPIDY[] = {
        1,
        3, 4,
        6, 7, 8,
        10, 11, 12, 13,
        15, 16, 17, 18, 19,
        21, 22, 23, 24, 25, 26,
        28, 29, 30, 31, 32, 33, 34,
        36, 37, 38, 39, 40, 41, 42, 43,
        45, 46, 47, 48, 49, 50, 51, 52, 53,
        55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
        66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76,
        78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
        91, 92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,
       105,106,107,108,109,110,111,112,113,114,115,116,117,118,
       120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,
};
static const int _UPIDZ[] = {
        2,
        4, 5,
        7, 8, 9,
        11, 12, 13, 14,
        16, 17, 18, 19, 20,
        22, 23, 24, 25, 26, 27,
        29, 30, 31, 32, 33, 34, 35,
        37, 38, 39, 40, 41, 42, 43, 44,
        46, 47, 48, 49, 50, 51, 52, 53, 54,
        56, 57, 58, 59, 60, 61, 62, 63, 64, 65,
        67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
        79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
        92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,104,
       106,107,108,109,110,111,112,113,114,115,116,117,118,119,
       121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,
};
/*
 * _DOWN_XYZ, _DOWN_XYZ_ORDER, _DOWN1, _DOWN2 labels the index in the 1D
 * recursive relation f_{i+1} = i/2a * f_{i-1} + X * f_{i}
 * _DOWN_XYZ_ORDER      i in i/2a
 * _DOWN2               index of f_{i-1}
 * _DOWN_XYZ            index of X
 * _DOWN1               index of f_{i}
 */
static const int _DOWN1[] = {
-1,
0, 0, 0,
0, 1, 2, 1, 2, 2,
0, 0, 0, 3, 4, 5, 3, 3, 5, 5,
0, 0, 0, 3, 2, 5, 6, 7, 8, 9, 6, 6, 8, 9, 9,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 11, 12, 13, 14, 10, 10, 12, 13, 14, 14,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 16, 17, 18, 19, 20, 15, 15, 17, 18, 19, 20, 20,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 22, 23, 24, 25, 26, 27, 21, 21, 23, 24, 25, 26, 27, 27,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 29, 30, 31, 32, 33, 34, 35, 28, 28, 30, 31, 32, 33, 34, 35, 35,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 36, 36, 38, 39, 40, 41, 42, 43, 44, 44,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 45, 45, 47, 48, 49, 50, 51, 52, 53, 54, 54,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 36, 47, 48, 49, 50, 51, 52, 44, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 55, 55, 57, 58, 59, 60, 61, 62, 63, 64, 65, 65,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 36, 47, 48, 49, 50, 51, 52, 44, 54, 55, 45, 57, 58, 59, 60, 61, 62, 63, 54, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 66, 66, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 77,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 36, 47, 48, 49, 50, 51, 52, 44, 54, 55, 45, 57, 58, 59, 60, 61, 62, 63, 54, 65, 66, 55, 68, 69, 70, 71, 72, 73, 74, 75, 65, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 78, 78, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 90,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 36, 47, 48, 49, 50, 51, 52, 44, 54, 55, 45, 57, 58, 59, 60, 61, 62, 63, 54, 65, 66, 55, 68, 69, 70, 71, 72, 73, 74, 75, 65, 77, 78, 66, 80, 81, 82, 83, 84, 85, 86, 87, 88, 77, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 91, 91, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 104,
0, 0, 0, 3, 2, 5, 6, 3, 5, 9, 10, 6, 12, 9, 14, 15, 10, 17, 18, 14, 20, 21, 15, 23, 24, 25, 20, 27, 28, 21, 30, 31, 32, 33, 27, 35, 36, 28, 38, 39, 40, 41, 42, 35, 44, 45, 36, 47, 48, 49, 50, 51, 52, 44, 54, 55, 45, 57, 58, 59, 60, 61, 62, 63, 54, 65, 66, 55, 68, 69, 70, 71, 72, 73, 74, 75, 65, 77, 78, 66, 80, 81, 82, 83, 84, 85, 86, 87, 88, 77, 90, 91, 78, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 90, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 105, 105, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 119,
};
static const int _DOWN2[] = {
-1,
-1, -1, -1,
0, -1, -1, 0, -1, 0,
0, -1, -1, -1, -1, -1, 1, -1, -1, 2,
0, -1, -1, 3, -1, 5, -1, -1, -1, -1, 3, -1, 5, -1, 5,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, -1, -1, -1, -1, -1, 6, -1, 8, 9, -1, 9,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, -1, -1, -1, -1, -1, -1, 10, -1, 12, 13, 14, -1, 14,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, -1, -1, -1, -1, -1, -1, -1, 15, -1, 17, 18, 19, 20, -1, 20,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, -1, -1, -1, -1, -1, -1, -1, -1, 21, -1, 23, 24, 25, 26, 27, -1, 27,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, -1, -1, -1, -1, -1, -1, -1, -1, -1, 28, -1, 30, 31, 32, 33, 34, 35, -1, 35,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 36, -1, 38, 39, 40, 41, 42, 43, 44, -1, 44,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, 45, -1, 47, 48, 49, 50, 51, 52, -1, 54, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 45, -1, 47, 48, 49, 50, 51, 52, 53, 54, -1, 54,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, 45, -1, 47, 48, 49, 50, 51, 52, -1, 54, 55, -1, 57, 58, 59, 60, 61, 62, 63, -1, 65, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 55, -1, 57, 58, 59, 60, 61, 62, 63, 64, 65, -1, 65,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, 45, -1, 47, 48, 49, 50, 51, 52, -1, 54, 55, -1, 57, 58, 59, 60, 61, 62, 63, -1, 65, 66, -1, 68, 69, 70, 71, 72, 73, 74, 75, -1, 77, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 66, -1, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, -1, 77,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, 45, -1, 47, 48, 49, 50, 51, 52, -1, 54, 55, -1, 57, 58, 59, 60, 61, 62, 63, -1, 65, 66, -1, 68, 69, 70, 71, 72, 73, 74, 75, -1, 77, 78, -1, 80, 81, 82, 83, 84, 85, 86, 87, 88, -1, 90, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 78, -1, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, -1, 90,
0, -1, -1, 3, -1, 5, 6, -1, -1, 9, 10, -1, 12, -1, 14, 15, -1, 17, 18, -1, 20, 21, -1, 23, 24, 25, -1, 27, 28, -1, 30, 31, 32, 33, -1, 35, 36, -1, 38, 39, 40, 41, 42, -1, 44, 45, -1, 47, 48, 49, 50, 51, 52, -1, 54, 55, -1, 57, 58, 59, 60, 61, 62, 63, -1, 65, 66, -1, 68, 69, 70, 71, 72, 73, 74, 75, -1, 77, 78, -1, 80, 81, 82, 83, 84, 85, 86, 87, 88, -1, 90, 91, -1, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, -1, 104, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 91, -1, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, -1, 104,
};
static const int _DOWN_XYZ[] = {
2,
0, 1, 2,
0, 0, 0, 1, 1, 2,
0, 1, 2, 0, 0, 0, 1, 2, 1, 2,
0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 1, 2, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
0, 1, 2, 0, 1, 0, 0, 2, 1, 0, 0, 2, 0, 1, 0, 0, 2, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
};
static const int _DOWN_XYZ_ORDER[] = {
0,
0, 0, 0,
1, 0, 0, 1, 0, 1,
2, 0, 0, 0, 0, 0, 2, 0, 0, 2,
3, 0, 0, 1, 0, 1, 0, 0, 0, 0, 3, 0, 1, 0, 3,
4, 0, 0, 2, 0, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 4, 0, 2, 1, 0, 4,
5, 0, 0, 3, 0, 3, 2, 0, 0, 2, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 5, 0, 3, 2, 1, 0, 5,
6, 0, 0, 4, 0, 4, 3, 0, 0, 3, 2, 0, 2, 0, 2, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 6, 0, 4, 3, 2, 1, 0, 6,
7, 0, 0, 5, 0, 5, 4, 0, 0, 4, 3, 0, 3, 0, 3, 2, 0, 2, 2, 0, 2, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 5, 4, 3, 2, 1, 0, 7,
8, 0, 0, 6, 0, 6, 5, 0, 0, 5, 4, 0, 4, 0, 4, 3, 0, 3, 3, 0, 3, 2, 0, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 6, 5, 4, 3, 2, 1, 0, 8,
9, 0, 0, 7, 0, 7, 6, 0, 0, 6, 5, 0, 5, 0, 5, 4, 0, 4, 4, 0, 4, 3, 0, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 7, 6, 5, 4, 3, 2, 1, 0, 9,
10, 0, 0, 8, 0, 8, 7, 0, 0, 7, 6, 0, 6, 0, 6, 5, 0, 5, 5, 0, 5, 4, 0, 4, 4, 4, 0, 4, 3, 0, 3, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 8, 7, 6, 5, 4, 3, 2, 1, 0, 10,
11, 0, 0, 9, 0, 9, 8, 0, 0, 8, 7, 0, 7, 0, 7, 6, 0, 6, 6, 0, 6, 5, 0, 5, 5, 5, 0, 5, 4, 0, 4, 4, 4, 4, 0, 4, 3, 0, 3, 3, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11,
12, 0, 0, 10, 0, 10, 9, 0, 0, 9, 8, 0, 8, 0, 8, 7, 0, 7, 7, 0, 7, 6, 0, 6, 6, 6, 0, 6, 5, 0, 5, 5, 5, 5, 0, 5, 4, 0, 4, 4, 4, 4, 4, 0, 4, 3, 0, 3, 3, 3, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 0, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 12,
13, 0, 0, 11, 0, 11, 10, 0, 0, 10, 9, 0, 9, 0, 9, 8, 0, 8, 8, 0, 8, 7, 0, 7, 7, 7, 0, 7, 6, 0, 6, 6, 6, 6, 0, 6, 5, 0, 5, 5, 5, 5, 5, 0, 5, 4, 0, 4, 4, 4, 4, 4, 4, 0, 4, 3, 0, 3, 3, 3, 3, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 13, 0, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 13,
14, 0, 0, 12, 0, 12, 11, 0, 0, 11, 10, 0, 10, 0, 10, 9, 0, 9, 9, 0, 9, 8, 0, 8, 8, 8, 0, 8, 7, 0, 7, 7, 7, 7, 0, 7, 6, 0, 6, 6, 6, 6, 6, 0, 6, 5, 0, 5, 5, 5, 5, 5, 5, 0, 5, 4, 0, 4, 4, 4, 4, 4, 4, 4, 0, 4, 3, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0, 3, 2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 14, 0, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 14,
};
#define WHEREX_IF_L_INC1(i)     i
#define WHEREY_IF_L_INC1(i)     _UPIDY[i]
#define WHEREZ_IF_L_INC1(i)     _UPIDZ[i]
#define STARTX_IF_L_DEC1(i)     0
#define STARTY_IF_L_DEC1(i)     ((i<2)?0:_LEN_CART[i-2])
#define STARTZ_IF_L_DEC1(i)     (_LEN_CART[i-1]-1)
#define ADDR_IF_L_DEC1(l,m)     _DOWN1[_CUM_LEN_CART[l-1]+m]
#define ADDR_IF_L_DEC2(l,m)     _DOWN2[_CUM_LEN_CART[l-1]+m]
#define DEC1_XYZ(l,m)           _DOWN_XYZ[_CUM_LEN_CART[l-1]+m]
#define DEC1_XYZ_ORDER(l,m)     _DOWN_XYZ_ORDER[_CUM_LEN_CART[l-1]+m]

static int vrr1d(double complex *g, double *rijri, double aij,
                 double *Gv, int topl, int nGv)
{
        int cumxyz = 1;
        if (topl == 0) {
                return cumxyz;
        }

        double *kx = Gv;
        double *ky = kx + nGv;
        double *kz = ky + nGv;
        int i, n, m, l;
        double a2;
        double complex *p0, *p1, *p2, *dec1, *dec2;
        double *ka2 = malloc(sizeof(double) * nGv*3);
        double *kxa2 = ka2;
        double *kya2 = kxa2 + nGv;
        double *kza2 = kya2 + nGv;
        a2 = .5 / aij;
        for (n = 0; n < nGv; n++) {
                kxa2[n] = kx[n] * a2;
                kya2[n] = ky[n] * a2;
                kza2[n] = kz[n] * a2;
        }

        p0 = g + nGv;
        for (n = 0; n < nGv; n++) {
                p0[      n] = (rijri[0] - kxa2[n]*_Complex_I) * g[n];
                p0[nGv  +n] = (rijri[1] - kya2[n]*_Complex_I) * g[n];
                p0[nGv*2+n] = (rijri[2] - kza2[n]*_Complex_I) * g[n];
        }
        cumxyz += 3;

        for (l = 1; l < topl; l++) {
                p0 = g + cumxyz * nGv;
                dec1 = p0   - _LEN_CART[l  ] * nGv;
                dec2 = dec1 - _LEN_CART[l-1] * nGv;
                for (i = 0; i < _LEN_CART[l+1]; i++) {
                        m = DEC1_XYZ(l+1,i);
                        kxa2 = ka2 + m * nGv;
                        a2 = .5/aij * DEC1_XYZ_ORDER(l+1,i);
                        p1 = dec1 + ADDR_IF_L_DEC1(l+1,i) * nGv;
                        p2 = dec2 + ADDR_IF_L_DEC2(l+1,i) * nGv;
                        if (ADDR_IF_L_DEC2(l+1,i) < 0) {
                                for (n = 0; n < nGv; n++) {
                                        p0[n] = (rijri[m]-kxa2[n]*_Complex_I)*p1[n];
                                }
                        } else {
                                for (n = 0; n < nGv; n++) {
                                        p0[n] = a2*p2[n] + (rijri[m]-kxa2[n]*_Complex_I)*p1[n];
                                }
                        }
                        p0 += nGv;
                }
                cumxyz += _LEN_CART[l+1];
        }
        free(ka2);
        return cumxyz;
}

/*
 * if li = 3, lj = 1
 * (10 + X*00 -> 01):
 *  gs + X*fs -> fp
 */
static void vrr2d_ket_inc1(double complex *out, const double complex *g,
                           double *rirj, int li, int lj, int nGv)
{
        if (lj == 0) {
                memcpy(out, g, sizeof(double complex)*_LEN_CART[li]*nGv);
                return;
        }
        const int row_10 = _LEN_CART[li+1];
        const int row_00 = _LEN_CART[li  ];
        const int col_00 = _LEN_CART[lj-1];
        const double complex *g00 = g;
        const double complex *g10 = g + row_00*col_00*nGv;
        int i, j, n;
        const double complex *p00, *p10;
        double complex *p01 = out;

        for (j = STARTX_IF_L_DEC1(lj); j < _LEN_CART[lj-1]; j++) {
        for (i = 0; i < row_00; i++) {
                p00 = g00 + (j*row_00+i) * nGv;
                p10 = g10 + (j*row_10+WHEREX_IF_L_INC1(i)) * nGv;
                for (n = 0; n < nGv; n++) {
                        p01[n] = p10[n] + rirj[0] * p00[n];
                }
                p01 += nGv;
        } }
        for (j = STARTY_IF_L_DEC1(lj); j < _LEN_CART[lj-1]; j++) {
        for (i = 0; i < row_00; i++) {
                p00 = g00 + (j*row_00+i) * nGv;
                p10 = g10 + (j*row_10+WHEREY_IF_L_INC1(i)) * nGv;
                for (n = 0; n < nGv; n++) {
                        p01[n] = p10[n] + rirj[1] * p00[n];
                }
                p01 += nGv;
        } }
        j = STARTZ_IF_L_DEC1(lj);
        if (j < _LEN_CART[lj-1]) {
        for (i = 0; i < row_00; i++) {
                p00 = g00 + (j*row_00+i) * nGv;
                p10 = g10 + (j*row_10+WHEREZ_IF_L_INC1(i)) * nGv;
                for (n = 0; n < nGv; n++) {
                        p01[n] = p10[n] + rirj[2] * p00[n];
                }
                p01 += nGv;
        } }
}
/*
 * transpose i, j when store in out
 */
static void vrr2d_inc1_swapij(double complex *out, const double complex *g,
                              double *rirj, int li, int lj, int nGv)
{
        if (lj == 0) {
                memcpy(out, g, sizeof(double complex)*_LEN_CART[li]*nGv);
                return;
        }
        const int row_01 = _LEN_CART[lj];
        const int row_10 = _LEN_CART[li+1];
        const int row_00 = _LEN_CART[li  ];
        const int col_00 = _LEN_CART[lj-1];
        const double complex *g00 = g;
        const double complex *g10 = g + row_00*col_00*nGv;
        int i, j, n;
        const double complex *p00, *p10;
        double complex *p01 = out;

        for (j = STARTX_IF_L_DEC1(lj); j < _LEN_CART[lj-1]; j++) {
                for (i = 0; i < row_00; i++) {
                        p00 = g00 + (j*row_00+i) * nGv;
                        p10 = g10 + (j*row_10+WHEREX_IF_L_INC1(i)) * nGv;
                        p01 = out + i*row_01 * nGv;
                        for (n = 0; n < nGv; n++) {
                                p01[n] = p10[n] + rirj[0] * p00[n];
                        }
                }
                out += nGv;
        }
        for (j = STARTY_IF_L_DEC1(lj); j < _LEN_CART[lj-1]; j++) {
                for (i = 0; i < row_00; i++) {
                        p00 = g00 + (j*row_00+i) * nGv;
                        p10 = g10 + (j*row_10+WHEREY_IF_L_INC1(i)) * nGv;
                        p01 = out + i*row_01 * nGv;
                        for (n = 0; n < nGv; n++) {
                                p01[n] = p10[n] + rirj[1] * p00[n];
                        }
                }
                out += nGv;
        }
        j = STARTZ_IF_L_DEC1(lj);
        if (j < _LEN_CART[lj-1]) {
                for (i = 0; i < row_00; i++) {
                        p00 = g00 + (j*row_00+i) * nGv;
                        p10 = g10 + (j*row_10+WHEREZ_IF_L_INC1(i)) * nGv;
                        p01 = out + i*row_01 * nGv;
                        for (n = 0; n < nGv; n++) {
                                p01[n] = p10[n] + rirj[2] * p00[n];
                        }
                }
        }
}
/* (li+lj,0) => (li,lj) */
static void vrr2d(double complex *out, double complex *g,
                  double complex *gbuf2, CINTEnvVars *envs, int nGv)
{
        const int li = envs->li_ceil;
        const int lj = envs->lj_ceil;
        const int nmax = li + lj;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double complex *g00, *g01, *gswap, *pg00, *pg01;
        int row_01, col_01, row_00, col_00;
        int i, j;
        double rirj[3];
        rirj[0] = ri[0] - rj[0];
        rirj[1] = ri[1] - rj[1];
        rirj[2] = ri[2] - rj[2];

        g00 = gbuf2;
        g01 = g;
        for (j = 1; j < lj; j++) {
                gswap = g00;
                g00 = g01;
                g01 = gswap;
                pg00 = g00;
                pg01 = g01;
                for (i = li; i <= nmax-j; i++) {
                        vrr2d_ket_inc1(pg01, pg00, rirj, i, j, nGv);
                        row_01 = _LEN_CART[i];
                        col_01 = _LEN_CART[j];
                        row_00 = _LEN_CART[i  ];
                        col_00 = _LEN_CART[j-1];
                        pg00 += row_00*col_00 * nGv;
                        pg01 += row_01*col_01 * nGv;
                }
        }
        vrr2d_ket_inc1(out, g01, rirj, li, lj, nGv);
}
/* (0,li+lj) => (li,lj) */
static void hrr2d(double complex *out, double complex *g,
                  double complex *gbuf2, CINTEnvVars *envs, int nGv)
{
        const int li = envs->li_ceil;
        const int lj = envs->lj_ceil;
        const int nmax = li + lj;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double complex *g00, *g01, *gswap, *pg00, *pg01;
        int row_01, col_01, row_00, col_00;
        int i, j;
        double rjri[3];
        rjri[0] = rj[0] - ri[0];
        rjri[1] = rj[1] - ri[1];
        rjri[2] = rj[2] - ri[2];

        g00 = gbuf2;
        g01 = g;
        for (i = 1; i < li; i++) {
                gswap = g00;
                g00 = g01;
                g01 = gswap;
                pg00 = g00;
                pg01 = g01;
                for (j = lj; j <= nmax-i; j++) {
                        vrr2d_ket_inc1(pg01, pg00, rjri, j, i, nGv);
                        row_01 = _LEN_CART[j];
                        col_01 = _LEN_CART[i];
                        row_00 = _LEN_CART[j  ];
                        col_00 = _LEN_CART[i-1];
                        pg00 += row_00*col_00 * nGv;
                        pg01 += row_01*col_01 * nGv;
                }
        }
        vrr2d_inc1_swapij(out, g01, rjri, lj, li, nGv);
}

/*
 * Recursive relation
 */
static void aopair_rr_igtj_early(double complex *g, double ai, double aj,
                                 CINTEnvVars *envs, void (*eval_gz)(),
                                 double complex fac, double *Gv, double *invh,
                                 int *gxyz, int *gs, int nGv)
{
        const int topl = envs->li_ceil + envs->lj_ceil;
        const double aij = ai + aj;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double rij[3], rijri[3];

        rij[0] = (ai * ri[0] + aj * rj[0]) / aij;
        rij[1] = (ai * ri[1] + aj * rj[1]) / aij;
        rij[2] = (ai * ri[2] + aj * rj[2]) / aij;
        rijri[0] = rij[0] - ri[0];
        rijri[1] = rij[1] - ri[1];
        rijri[2] = rij[2] - ri[2];

        (*eval_gz)(g, aij, rij, fac, Gv, invh, gxyz, gs, nGv);
        vrr1d(g, rijri, aij, Gv, topl, nGv);
}
static void aopair_rr_iltj_early(double complex *g, double ai, double aj,
                                 CINTEnvVars *envs, void (*eval_gz)(),
                                 double complex fac, double *Gv, double *invh,
                                 int *gxyz, int *gs, int nGv)
{
        const int topl = envs->li_ceil + envs->lj_ceil;
        const double aij = ai + aj;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double rij[3], rijrj[3];

        rij[0] = (ai * ri[0] + aj * rj[0]) / aij;
        rij[1] = (ai * ri[1] + aj * rj[1]) / aij;
        rij[2] = (ai * ri[2] + aj * rj[2]) / aij;
        rijrj[0] = rij[0] - rj[0];
        rijrj[1] = rij[1] - rj[1];
        rijrj[2] = rij[2] - rj[2];

        (*eval_gz)(g, aij, rij, fac, Gv, invh, gxyz, gs, nGv);
        vrr1d(g, rijrj, aij, Gv, topl, nGv);
}

static void aopair_rr_igtj_lazy(double complex *g, double ai, double aj,
                                CINTEnvVars *envs, void (*eval_gz)(),
                                double complex fac, double *Gv, double *invh,
                                int *gxyz, int *gs, int nGv)
{
        const int nmax = envs->li_ceil + envs->lj_ceil;
        const int lj = envs->lj_ceil;
        const int dj = envs->g_stride_j;
        const double aij = ai + aj;
        const double a2 = .5 / aij;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double rij[3], rirj[3], rijri[3];
        double complex *gx = g;
        double complex *gy = gx + envs->g_size * nGv;
        double complex *gz = gy + envs->g_size * nGv;
        double *kx = Gv;
        double *ky = kx + nGv;
        double *kz = ky + nGv;
        size_t off0, off1, off2;
        int i, j, n, ptr;
        double ia2;

        rirj[0] = ri[0] - rj[0];
        rirj[1] = ri[1] - rj[1];
        rirj[2] = ri[2] - rj[2];
        rij[0] = (ai * ri[0] + aj * rj[0]) / aij;
        rij[1] = (ai * ri[1] + aj * rj[1]) / aij;
        rij[2] = (ai * ri[2] + aj * rj[2]) / aij;
        rijri[0] = rij[0] - ri[0];
        rijri[1] = rij[1] - ri[1];
        rijri[2] = rij[2] - ri[2];

        for (n = 0; n < nGv; n++) {
                gx[n] = 1;
                gy[n] = 1;
        }
        (*eval_gz)(gz, aij, rij, fac, Gv, invh, gxyz, gs, nGv);

        if (nmax > 0) {
                for (n = 0; n < nGv; n++) {
                        if (gz[n] != 0) {
                                gx[nGv+n] = (rijri[0] - kx[n]*a2*_Complex_I) * gx[n];
                                gy[nGv+n] = (rijri[1] - ky[n]*a2*_Complex_I) * gy[n];
                                gz[nGv+n] = (rijri[2] - kz[n]*a2*_Complex_I) * gz[n];
                        }
                }
        }

        for (i = 1; i < nmax; i++) {
                off0 = (i-1) * nGv;
                off1 =  i    * nGv;
                off2 = (i+1) * nGv;
                ia2 = i * a2;
                for (n = 0; n < nGv; n++) {
                        if (gz[n] != 0) {
                                gx[off2+n] = ia2 * gx[off0+n] + (rijri[0] - kx[n]*a2*_Complex_I) * gx[off1+n];
                                gy[off2+n] = ia2 * gy[off0+n] + (rijri[1] - ky[n]*a2*_Complex_I) * gy[off1+n];
                                gz[off2+n] = ia2 * gz[off0+n] + (rijri[2] - kz[n]*a2*_Complex_I) * gz[off1+n];
                        }
                }
        }

        for (j = 1; j <= lj; j++) {
                ptr = dj * j;
                for (i = ptr; i <= ptr + nmax - j; i++) {
                        off0 =  i    * nGv - dj * nGv;  // [i,  j-1]
                        off1 = (i+1) * nGv - dj * nGv;  // [i+1,j-1]
                        off2 =  i    * nGv;             // [i,  j  ]
                        for (n = 0; n < nGv; n++) {
                                if (gz[n] != 0) {
                                        gx[off2+n] = gx[off1+n] + rirj[0] * gx[off0+n];
                                        gy[off2+n] = gy[off1+n] + rirj[1] * gy[off0+n];
                                        gz[off2+n] = gz[off1+n] + rirj[2] * gz[off0+n];
                                }
                        }
                }
        }
}
static void aopair_rr_iltj_lazy(double complex *g, double ai, double aj,
                                CINTEnvVars *envs, void (*eval_gz)(),
                                double complex fac, double *Gv, double *invh,
                                int *gxyz, int *gs, int nGv)
{
        const int nmax = envs->li_ceil + envs->lj_ceil;
        const int li = envs->li_ceil;
        const int dj = envs->g_stride_j;
        const double aij = ai + aj;
        const double a2 = .5 / aij;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        double rij[3], rirj[3], rijrj[3];
        double complex *gx = g;
        double complex *gy = gx + envs->g_size * nGv;
        double complex *gz = gy + envs->g_size * nGv;
        double *kx = Gv;
        double *ky = kx + nGv;
        double *kz = ky + nGv;
        size_t off0, off1, off2;
        int i, j, n;
        double ia2;

        rirj[0] = rj[0] - ri[0];
        rirj[1] = rj[1] - ri[1];
        rirj[2] = rj[2] - ri[2];
        rij[0] = (ai * ri[0] + aj * rj[0]) / aij;
        rij[1] = (ai * ri[1] + aj * rj[1]) / aij;
        rij[2] = (ai * ri[2] + aj * rj[2]) / aij;
        rijrj[0] = rij[0] - rj[0];
        rijrj[1] = rij[1] - rj[1];
        rijrj[2] = rij[2] - rj[2];

        for (n = 0; n < nGv; n++) {
                gx[n] = 1;
                gy[n] = 1;
        }
        (*eval_gz)(gz, aij, rij, fac, Gv, invh, gxyz, gs, nGv);

        if (nmax > 0) {
                off0 = dj * nGv;
                for (n = 0; n < nGv; n++) {
                        if (gz[n] != 0) {
                                gx[off0+n] = (rijrj[0] - kx[n]*a2*_Complex_I) * gx[n];
                                gy[off0+n] = (rijrj[1] - ky[n]*a2*_Complex_I) * gy[n];
                                gz[off0+n] = (rijrj[2] - kz[n]*a2*_Complex_I) * gz[n];
                        }
                }
        }

        for (i = 1; i < nmax; i++) {
                off0 = (i-1) * dj * nGv;
                off1 =  i    * dj * nGv;
                off2 = (i+1) * dj * nGv;
                ia2 = i * a2;
                for (n = 0; n < nGv; n++) {
                        if (gz[n] != 0) {
                                gx[off2+n] = ia2 * gx[off0+n] + (rijrj[0] - kx[n]*a2*_Complex_I) * gx[off1+n];
                                gy[off2+n] = ia2 * gy[off0+n] + (rijrj[1] - ky[n]*a2*_Complex_I) * gy[off1+n];
                                gz[off2+n] = ia2 * gz[off0+n] + (rijrj[2] - kz[n]*a2*_Complex_I) * gz[off1+n];
                        }
                }
        }

        for (i = 1; i <= li; i++) {
                for (j = 0; j <= nmax - i; j++) {
                        off0 = (i-1) * nGv +  j    * dj * nGv;  // [i-1,j  ]
                        off1 = (i-1) * nGv + (j+1) * dj * nGv;  // [i-1,j+1]
                        off2 =  i    * nGv +  j    * dj * nGv;  // [i  ,j  ]
                        for (n = 0; n < nGv; n++) {
                                if (gz[n] != 0) {
                                        gx[off2+n] = gx[off1+n] + rirj[0] * gx[off0+n];
                                        gy[off2+n] = gy[off1+n] + rirj[1] * gy[off0+n];
                                        gz[off2+n] = gz[off1+n] + rirj[2] * gz[off0+n];
                                }
                        }
                }
        }
}

static void inner_prod(double complex *g, double complex *gout,
                       const int *idx, const CINTEnvVars *envs, int nGv,
                       int empty)
{
        int ix, iy, iz, n, k;
        double complex *gz = g + envs->g_size * nGv * 2;
        if (empty) {
                for (n = 0; n < envs->nf; n++) {
                        ix = idx[n*3+0];
                        iy = idx[n*3+1];
                        iz = idx[n*3+2];
                        for (k = 0; k < nGv; k++) {
                                if (gz[k] != 0) {
                                        gout[n*nGv+k] = g[ix*nGv+k] * g[iy*nGv+k] * g[iz*nGv+k];
                                } else {
                                        gout[n*nGv+k] = 0;
                                }
                        }
                }
        } else {
                for (n = 0; n < envs->nf; n++) {
                        ix = idx[n*3+0];
                        iy = idx[n*3+1];
                        iz = idx[n*3+2];
                        for (k = 0; k < nGv; k++) {
                                if (gz[k] != 0) {
                                        gout[n*nGv+k] += g[ix*nGv+k] * g[iy*nGv+k] * g[iz*nGv+k];
                                }
                        }
                }
        }
}

static void prim_to_ctr(double complex *gc, const int nf, const double complex *gp,
                        const int nprim, const int nctr, const double *coeff,
                        int empty)
{
        double complex *pgc = gc;
        int n, i;
        double c;

        if (empty) {
                for (n = 0; n < nctr; n++) {
                        c = coeff[nprim*n];
                        if (c != 0) {
                                for (i = 0; i < nf; i++) {
                                        pgc[i] = gp[i] * c;
                                }
                        }
                        pgc += nf;
                }
        } else {
                for (n = 0; n < nctr; n++) {
                        c = coeff[nprim*n];
                        if (c != 0) {
                                for (i = 0; i < nf; i++) {
                                        pgc[i] += gp[i] * c;
                                }
                        }
                        pgc += nf;
                }
        }
}


static const int _GBUFSIZE[] = {
        1, 4, 10, 10, 20, 48, 20, 35, 75, 150, 35, 56, 108, 216, 384,
        56, 84, 147, 294, 510, 850, 84, 120, 192, 384, 654, 1090, 1640,
        120, 165, 243, 486, 816, 1360, 2040, 3030
};
#define bufsize(i,j)    _GBUFSIZE[((i>=j) ? (i*(i+1)/2+j) : (j*(j+1)/2+i))]

int GTO_aopair_early_contract(double complex *out, CINTEnvVars *envs,
                              void (*eval_gz)(), double complex fac,
                              double *Gv, double *invh, int *gxyz, int *gs,int nGv)
{
        const int *shls  = envs->shls;
        const int *bas = envs->bas;
        const double *env = envs->env;
        const int i_sh = shls[0];
        const int j_sh = shls[1];
        const int i_l = envs->i_l;
        const int j_l = envs->j_l;
        const int i_ctr = envs->i_ctr;
        const int j_ctr = envs->j_ctr;
        const int nf = envs->nf;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        const double *ai = env + bas(PTR_EXP, i_sh);
        const double *aj = env + bas(PTR_EXP, j_sh);
        const double *ci = env + bas(PTR_COEFF, i_sh);
        const double *cj = env + bas(PTR_COEFF, j_sh);
        double fac1i, fac1j;
        double aij, dij, eij;
        int ip, jp, n;
        int empty[2] = {1, 1};
        int *jempty = empty + 0;
        int *iempty = empty + 1;
        const int len1 = bufsize(i_l,j_l) * nGv;
        const int leni = len1 * i_ctr;
        const int lenj = len1 * i_ctr * j_ctr;
        double complex *gctrj = malloc(sizeof(double complex)*(lenj+leni+len1));
        double complex *g = gctrj + lenj;
        double complex *gctri, *g1d;

        if (j_ctr == 1) {
                gctri = gctrj;
                iempty = jempty;
        } else {
                gctri = g;
                g += leni;
        }
        g1d = g;

        void (*aopair_rr)();
        int offset_g1d;
        if (i_l >= j_l) {
                aopair_rr = aopair_rr_igtj_early;
                offset_g1d = _CUM_LEN_CART[i_l] - _LEN_CART[i_l];
        } else {
                aopair_rr = aopair_rr_iltj_early;
                offset_g1d = _CUM_LEN_CART[j_l] - _LEN_CART[j_l];
        }
        int len_g1d = _CUM_LEN_CART[i_l+j_l] - offset_g1d;

        double rrij = CINTsquare_dist(ri, rj);
        double fac1 = SQRTPI * M_PI * CINTcommon_fac_sp(i_l) * CINTcommon_fac_sp(j_l);

        *jempty = 1;
        for (jp = 0; jp < envs->j_prim; jp++) {
                if (j_ctr == 1) {
                        fac1j = fac1 * cj[jp];
                } else {
                        fac1j = fac1;
                        *iempty = 1;
                }
                for (ip = 0; ip < envs->i_prim; ip++) {
                        aij = ai[ip] + aj[jp];
                        eij = (ai[ip] * aj[jp] / aij) * rrij;
                        if (eij > EXPCUTOFF) {
                                continue;
                        }

                        dij = exp(-eij) / (aij * sqrt(aij));
                        fac1i = fac1j * dij;
                        (*aopair_rr)(g, ai[ip], aj[jp], envs, eval_gz,
                                     fac*fac1i, Gv, invh, gxyz, gs, nGv);

                        prim_to_ctr(gctri, len_g1d*nGv, g1d+offset_g1d*nGv,
                                    envs->i_prim, i_ctr, ci+ip, *iempty);
                        *iempty = 0;
                }
                if (!*iempty) {
                        if (j_ctr > 1) {
                                prim_to_ctr(gctrj, i_ctr*len_g1d*nGv, gctri,
                                            envs->j_prim,j_ctr, cj+jp, *jempty);
                        }
                        *jempty = 0;
                }
        }

        if (!*jempty) {
                g1d = gctrj;
                for (n = 0; n < i_ctr*j_ctr; n++) {
                        if (i_l >= j_l) {
                                vrr2d(out+n*nf*nGv, g1d, gctrj+lenj, envs, nGv);
                        } else {
                                hrr2d(out+n*nf*nGv, g1d, gctrj+lenj, envs, nGv);
                        }
                        g1d += len_g1d * nGv;
                }
        }
        free(gctrj);

        return !*jempty;
}

int GTO_aopair_lazy_contract(double complex *gctr, CINTEnvVars *envs,
                             void (*eval_gz)(), double complex fac,
                             double *Gv, double *invh, int *gxyz, int *gs,int nGv)
{
        const int *shls  = envs->shls;
        const int *bas = envs->bas;
        const double *env = envs->env;
        const int i_sh = shls[0];
        const int j_sh = shls[1];
        const int i_l = envs->i_l;
        const int j_l = envs->j_l;
        const int i_ctr = envs->i_ctr;
        const int j_ctr = envs->j_ctr;
        const int nf = envs->nf;
        const double *ri = envs->ri;
        const double *rj = envs->rj;
        const double *ai = env + bas(PTR_EXP, i_sh);
        const double *aj = env + bas(PTR_EXP, j_sh);
        const double *ci = env + bas(PTR_COEFF, i_sh);
        const double *cj = env + bas(PTR_COEFF, j_sh);
        double fac1i, fac1j;
        double aij, dij, eij;
        int ip, jp;
        int empty[3] = {1, 1, 1};
        int *jempty = empty + 0;
        int *iempty = empty + 1;
        int *gempty = empty + 2;
        const int len1 = envs->g_size * 3 * nGv;
        const int leng = nf * nGv;
        const int leni = nf * i_ctr * nGv;
        double complex *g = malloc(sizeof(double complex) * (len1+leng+leni));
        double complex *g1 = g + len1;
        double complex *gout, *gctri;

        if (j_ctr == 1) {
                gctri = gctr;
                iempty = jempty;
        } else {
                gctri = g1;
                g1 += leni;
        }
        if (i_ctr == 1) {
                gout = gctri;
                gempty = iempty;
        } else {
                gout = g1;
        }

        void (*aopair_rr)();
        if (i_l >= j_l) {
                aopair_rr = aopair_rr_igtj_lazy;
        } else {
                aopair_rr = aopair_rr_iltj_lazy;
        }

        int *idx = malloc(sizeof(int) * nf * 3);
        CINTg1e_index_xyz(idx, envs);

        double rrij = CINTsquare_dist(ri, rj);
        double fac1 = SQRTPI * M_PI * CINTcommon_fac_sp(i_l) * CINTcommon_fac_sp(j_l);

        *jempty = 1;
        for (jp = 0; jp < envs->j_prim; jp++) {
                if (j_ctr == 1) {
                        fac1j = fac1 * cj[jp];
                } else {
                        fac1j = fac1;
                        *iempty = 1;
                }
                for (ip = 0; ip < envs->i_prim; ip++) {
                        aij = ai[ip] + aj[jp];
                        eij = (ai[ip] * aj[jp] / aij) * rrij;
                        if (eij > EXPCUTOFF) {
                                continue;
                        }

                        dij = exp(-eij) / (aij * sqrt(aij));
                        if (i_ctr == 1) {
                                fac1i = fac1j * dij * ci[ip];
                        } else {
                                fac1i = fac1j * dij;
                        }
                        (*aopair_rr)(g, ai[ip], aj[jp], envs, eval_gz,
                                     fac*fac1i, Gv, invh, gxyz, gs, nGv);

                        inner_prod(g, gout, idx, envs, nGv, *gempty);
                        if (i_ctr > 1) {
                                prim_to_ctr(gctri, nf*nGv, gout, envs->i_prim,
                                            i_ctr, ci+ip, *iempty);
                        }
                        *iempty = 0;
                }
                if (!*iempty) {
                        if (j_ctr > 1) {
                                prim_to_ctr(gctr, i_ctr*nf*nGv, gctri, envs->j_prim,
                                            j_ctr, cj+jp, *jempty);
                        }
                        *jempty = 0;
                }
        }
        free(g);
        free(idx);

        return !*jempty;
}

/*
 * Gv = 2\pi * dot(invh,gxyz)
 * kk = dot(Gv, Gv)
 * kr = dot(rij, Gv) = 2\pi * dot(rij,invh.T, Gv) = dot(b, Gv)
 * out = fac * exp(-.25 * kk / aij) * (cos(kr) - sin(kr) * _Complex_I);
 *
 * Orthorhombic, invh is diagonal
 */
void GTO_Gv_uniform_orth(double complex *out, double aij, double *rij,
                         double complex fac, double *Gv, double *invh,
                         int *gxyz, int *gs, int nGv)
{
        const int nx = gs[0];
        const int ny = gs[1];
        const int nz = gs[2];
        double Gvfac[3];
        double b[3];  // dot(rij, invh)
        Gvfac[0] = 2 * M_PI * invh[0];
        Gvfac[1] = 2 * M_PI * invh[4];
        Gvfac[2] = 2 * M_PI * invh[8];
        b[0] = Gvfac[0] * rij[0];
        b[1] = Gvfac[1] * rij[1];
        b[2] = Gvfac[2] * rij[2];

        double complex zbuf[2*(nx+ny+nz)+3];
        double kkpool[2*(nx+ny+nz)+3];
        double complex *csx = zbuf;
        double complex *csy = csx + 2*nx+1;
        double complex *csz = csy + 2*ny+1;
        double *kkx = kkpool;
        double *kky = kkx + 2*nx+1;
        double *kkz = kky + 2*ny+1;
        csx += nx;
        csy += ny;
        csz += nz;
        kkx += nx;
        kky += ny;
        kkz += nz;
        int *gx = gxyz;
        int *gy = gx + nGv;
        int *gz = gy + nGv;

        const double cutoff = EXPCUTOFF * aij * 4;
        int n, ix, iy, iz;
        double kR, kk;
        // kkpool = -1 to label uninitialized kk pool, because kk > 0
        for (n = 0; n < 2*(nx+ny+nz)+3; n++) {
                kkpool[n] = -1;
        }

        for (n = 0; n < nGv; n++) {
                ix = gx[n];
                iy = gy[n];
                iz = gz[n];
                if (kkx[ix] < 0) {
                        kk = Gvfac[0] * ix;
                        kR = b[0] * ix;
                        kkx[ix] = .25 * kk*kk / aij;
                        csx[ix] = exp(-kkx[ix]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kky[iy] < 0) {
                        kk = Gvfac[1] * iy;
                        kR = b[1] * iy;
                        kky[iy] = .25 * kk*kk / aij;
                        csy[iy] = exp(-kky[iy]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kkz[iz] < 0) {
                        kk = Gvfac[2] * iz;
                        kR = b[2] * iz;
                        kkz[iz] = .25 * kk*kk / aij;
                        csz[iz] = fac * exp(-kkz[iz]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kkx[ix] + kky[iy] + kkz[iz] < cutoff) {
                        out[n] = csx[ix] * csy[iy] * csz[iz];
                } else {
                        out[n] = 0;
                }
        }
}
/*
 * Gv = 2\pi * dot(invh,gxyz)
 * kk = dot(Gv, Gv)
 * kr = dot(rij, Gv) = 2\pi * dot(rij,invh.T, Gv) = dot(b, Gv)
 * out = fac * exp(-.25 * kk / aij) * (cos(kr) - sin(kr) * _Complex_I);
 *
 * Non-orthorhombic, invh is diagonal
 */
void GTO_Gv_uniform_nonorth(double complex *out, double aij, double *rij,
                            double complex fac, double *Gv, double *invh,
                            int *gxyz, int *gs, int nGv)
{
        const int nx = gs[0];
        const int ny = gs[1];
        const int nz = gs[2];
        double b[3];  // dot(rij, invh)
        b[0]  = rij[0] * invh[0];
        b[1]  = rij[0] * invh[3];
        b[2]  = rij[0] * invh[6];
        b[0] += rij[1] * invh[1];
        b[1] += rij[1] * invh[4];
        b[2] += rij[1] * invh[7];
        b[0] += rij[2] * invh[2];
        b[1] += rij[2] * invh[5];
        b[2] += rij[2] * invh[8];
        b[0] *= 2 * M_PI;
        b[1] *= 2 * M_PI;
        b[2] *= 2 * M_PI;

        double *kx = Gv;
        double *ky = kx + nGv;
        double *kz = ky + nGv;
        double complex zbuf[2*(nx+ny+nz)+3];
        int empty[2*(nx+ny+nz)+3];
        memset(empty, 1, sizeof(int) * (2*(nx+ny+nz)+3));
        double complex *csx = zbuf;
        double complex *csy = csx + 2*nx+1;
        double complex *csz = csy + 2*ny+1;
        int *xempty = empty;
        int *yempty = xempty + 2*nx+1;
        int *zempty = yempty + 2*ny+1;
        csx += nx;
        csy += ny;
        csz += nz;
        xempty += nx;
        yempty += ny;
        zempty += nz;
        int *gx = gxyz;
        int *gy = gx + nGv;
        int *gz = gy + nGv;

        const double cutoff = EXPCUTOFF * aij * 4;
        int n, ix, iy, iz;
        double kR, kk;
        for (n = 0; n < nGv; n++) {
                kk = kx[n] * kx[n] + ky[n] * ky[n] + kz[n] * kz[n];
                if (kk < cutoff) {
                        ix = gx[n];
                        iy = gy[n];
                        iz = gz[n];
                        if (xempty[ix]) {
                                kR = b[0] * ix;
                                csx[ix] = cos(kR) - sin(kR)*_Complex_I;
                                xempty[ix] = 0;
                        }
                        if (yempty[iy]) {
                                kR = b[1] * iy;
                                csy[iy] = cos(kR) - sin(kR)*_Complex_I;
                                yempty[iy] = 0;
                        }
                        if (zempty[iz]) {
                                kR = b[2] * iz;
                                csz[iz] = fac * (cos(kR) - sin(kR)*_Complex_I);
                                zempty[iz] = 0;
                        }
                        out[n] = exp(-.25*kk/aij) * csx[ix]*csy[iy]*csz[iz];
                } else {
                        out[n] = 0;
                }
        }
}

void GTO_Gv_general(double complex *out, double aij, double *rij,
                    double complex fac, double *Gv, double *invh,
                    int *gxyz, int *gs, int nGv)
{
        double *kx = Gv;
        double *ky = kx + nGv;
        double *kz = ky + nGv;
        const double cutoff = EXPCUTOFF * aij * 4;
        int n;
        double kR, kk;
        for (n = 0; n < nGv; n++) {
                kk = kx[n] * kx[n] + ky[n] * ky[n] + kz[n] * kz[n];
                if (kk < cutoff) {
                        kR = kx[n] * rij[0] + ky[n] * rij[1] + kz[n] * rij[2];
                        out[n] = exp(-.25*kk/aij) * fac * (cos(kR) - sin(kR)*_Complex_I);
                } else {
                        out[n] = 0;
                }
        }
}

/*
 * invh is the basic G value for each cartesian component
 * Gx = invh[:gs[0]]
 * Gy = invh[gs[0]:gs[0]+gs[1]]
 * Gz = invh[gs[0]+gs[1]:]
 * gxyz stores the index of Gx, Gy, Gz.  All indices are positive
 */
void GTO_Gv_nonuniform_orth(double complex *out, double aij, double *rij,
                            double complex fac, double *Gv, double *invh,
                            int *gxyz, int *gs, int nGv)
{
        const int nx = gs[0];
        const int ny = gs[1];
        const int nz = gs[2];
        double *Gxbase = invh;
        double *Gybase = Gxbase + nx;
        double *Gzbase = Gybase + ny;

        double complex zbuf[nx+ny+nz];
        double kkpool[2*(nx+ny+nz)+3];
        double complex *csx = zbuf;
        double complex *csy = csx + nx;
        double complex *csz = csy + ny;
        double *kkx = kkpool;
        double *kky = kkx + nx;
        double *kkz = kky + ny;
        int *gx = gxyz;
        int *gy = gx + nGv;
        int *gz = gy + nGv;

        const double cutoff = EXPCUTOFF * aij * 4;
        int n, ix, iy, iz;
        double kR;
        // kkpool = -1 to label uninitialized kk pool, because kk > 0
        for (n = 0; n < nx+ny+nz; n++) {
                kkpool[n] = -1;
        }

        for (n = 0; n < nGv; n++) {
                ix = gx[n];
                iy = gy[n];
                iz = gz[n];
                if (kkx[ix] < 0) {
                        kR = rij[0] * Gxbase[ix];
                        kkx[ix] = .25 * Gxbase[ix]*Gxbase[ix] / aij;
                        csx[ix] = exp(-kkx[ix]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kky[iy] < 0) {
                        kR = rij[1] * Gybase[iy];
                        kky[iy] = .25 * Gybase[iy]*Gybase[iy] / aij;
                        csy[iy] = exp(-kky[iy]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kkz[iz] < 0) {
                        kR = rij[2] * Gzbase[iz];
                        kkz[iz] = .25 * Gzbase[iz]*Gzbase[iz] / aij;
                        csz[iz] = fac * exp(-kkz[iz]) * (cos(kR)-sin(kR)*_Complex_I);
                }
                if (kkx[ix] + kky[iy] + kkz[iz] < cutoff) {
                        out[n] = csx[ix] * csy[iy] * csz[iz];
                } else {
                        out[n] = 0;
                }
        }
}



static void zcopy_ij(double complex *out, const double complex *gctr,
                     const int mi, const int mj, const int ni, const int nGv)
{
        int i, j, k;
        for (j = 0; j < mj; j++) {
                for (i = 0; i < mi; i++) {
                        for (k = 0; k < nGv; k++) {
                                out[i*nGv+k] = gctr[i*nGv+k];
                        }
                }
                out  += ni * nGv;
                gctr += mi * nGv;
        }
}

static void aopair_c2s_cart(double complex *out, double complex *gctr,
                            CINTEnvVars *envs, int *dims, int nGv)
{
        const int i_ctr = envs->i_ctr;
        const int j_ctr = envs->j_ctr;
        const int nfi = envs->nfi;
        const int nfj = envs->nfj;
        const int ni = nfi*i_ctr;
        const int nj = nfj*j_ctr;
        const int nf = envs->nf;
        int ic, jc;
        double complex *pout;

        for (jc = 0; jc < nj; jc += nfj) {
        for (ic = 0; ic < ni; ic += nfi) {
                pout = out + (dims[0] * jc + ic) * nGv;
                zcopy_ij(pout, gctr, nfi, nfj, dims[0], nGv);
                gctr += nf * nGv;
        } }
}


#define C2S(sph, nket, cart, l) \
        (double complex *)CINTc2s_ket_sph((double *)(sph), nket, (double *)(cart), l)
#define OF_CMPLX        2
static void aopair_c2s_sph(double complex *out, double complex *gctr,
                           CINTEnvVars *envs, int *dims, int nGv)
{
        const int i_l = envs->i_l;
        const int j_l = envs->j_l;
        const int i_ctr = envs->i_ctr;
        const int j_ctr = envs->j_ctr;
        const int di = i_l * 2 + 1;
        const int dj = j_l * 2 + 1;
        const int ni = di*i_ctr;
        const int nj = dj*j_ctr;
        const int nfi = envs->nfi;
        const int nf = envs->nf;
        int ic, jc, k;
        const int buflen = nfi*dj;
        double complex *buf1 = malloc(sizeof(double complex) * buflen*2 * nGv);
        double complex *buf2 = buf1 + buflen * nGv;
        double complex *pout, *pij, *buf;

        for (jc = 0; jc < nj; jc += dj) {
        for (ic = 0; ic < ni; ic += di) {
                buf = C2S(buf1, nfi*nGv*OF_CMPLX, gctr, j_l);
                pij = C2S(buf2, nGv*OF_CMPLX, buf, i_l);
                for (k = nGv; k < dj*nGv; k+=nGv) {
                        pout = C2S(buf2+k*di, nGv*OF_CMPLX, buf+k*nfi, i_l);
                }

                pout = out + (dims[0] * jc + ic) * nGv;
                zcopy_ij(pout, pij, di, dj, dims[0], nGv);
                gctr += nf * nGv;
        } }
        free(buf1);
}

/*************************************************
 *
 * eval_aopair is one of GTO_aopair_early_contract,
 * GTO_aopair_lazy_contract
 *
 * eval_gz is one of GTO_Gv_general, GTO_Gv_uniform_orth,
 * GTO_Gv_uniform_nonorth, GTO_Gv_nonuniform_orth
 *
 *************************************************/

int GTO_ft_ovlp_cart(double complex *out, int *shls, int *dims,
                     int (*eval_aopair)(), void (*eval_gz)(), double complex fac,
                     double *Gv, double *invh, int *gxyz, int *gs, int nGv,
                     int *atm, int natm, int *bas, int nbas, double *env)
{
        CINTEnvVars envs;
        init1e_envs(&envs, shls, atm, natm, bas, nbas, env);

        const int i_ctr = envs.i_ctr;
        const int j_ctr = envs.j_ctr;
        int ntot = envs.nf * i_ctr * j_ctr * nGv;
        double complex *gctr = malloc(sizeof(double complex) * ntot);
        if (eval_gz == NULL) {
                eval_gz = GTO_Gv_general;
        }
        if (eval_gz != GTO_Gv_general) {
                assert(gxyz != NULL);
        }

        if (eval_aopair == NULL) {
                if (envs.i_prim*envs.j_prim < i_ctr*j_ctr*3) {
                        eval_aopair = GTO_aopair_lazy_contract;
                } else {
                        eval_aopair = GTO_aopair_early_contract;
                }
        }
        int has_value = (*eval_aopair)(gctr, &envs, eval_gz,
                                       fac, Gv, invh, gxyz, gs, nGv);

        if (has_value) {
                aopair_c2s_cart(out, gctr, &envs, dims, nGv);
        }
        free(gctr);
        return has_value;
}

int GTO_ft_ovlp_sph(double complex *out, int *shls, int *dims,
                    int (*eval_aopair)(), void (*eval_gz)(), double complex fac,
                    double *Gv, double *invh, int *gxyz, int *gs, int nGv,
                    int *atm, int natm, int *bas, int nbas, double *env)
{
        CINTEnvVars envs;
        init1e_envs(&envs, shls, atm, natm, bas, nbas, env);

        const int i_ctr = envs.i_ctr;
        const int j_ctr = envs.j_ctr;
        int ntot = envs.nf * i_ctr * j_ctr * nGv;
        double complex *gctr = malloc(sizeof(double complex) * ntot);
        if (eval_gz == NULL) {
                eval_gz = GTO_Gv_general;
        }
        if (eval_gz != GTO_Gv_general) {
                assert(gxyz != NULL);
        }

        if (eval_aopair == NULL) {
                if (envs.i_prim*envs.j_prim < i_ctr*j_ctr*3) {
                        eval_aopair = GTO_aopair_lazy_contract;
                } else {
                        eval_aopair = GTO_aopair_early_contract;
                }
        }
        int has_value = (*eval_aopair)(gctr, &envs, eval_gz,
                                       fac, Gv, invh, gxyz, gs, nGv);

        if (has_value) {
                aopair_c2s_sph(out, gctr, &envs, dims, nGv);
        }
        free(gctr);
        return has_value;
}


/*************************************************
 *
 *************************************************/

static void zcopy_s2_igtj(double complex *out, double complex *in,
                          int nGv, int ip, int di, int dj)
{
        const size_t ip1 = ip + 1;
        int i, j, n;
        double complex *pin;
        for (i = 0; i < di; i++) {
                for (j = 0; j < dj; j++) {
                        pin = in  + nGv * (j*di+i);
                        for (n = 0; n < nGv; n++) {
                                out[j*nGv+n] = pin[n];
                        }
                }
                out += (ip1 + i) * nGv;
        }
}
static void zcopy_s2_ieqj(double complex *out, double complex *in,
                          int nGv, int ip, int di, int dj)
{
        const size_t ip1 = ip + 1;
        int i, j, n;
        double complex *pin;
        for (i = 0; i < di; i++) {
                for (j = 0; j <= i; j++) {
                        pin = in  + nGv * (j*di+i);
                        for (n = 0; n < nGv; n++) {
                                out[j*nGv+n] = pin[n];
                        }
                }
                out += (ip1 + i) * nGv;
        }
}

void GTO_ft_fill_s1(int (*intor)(), void (*eval_gz)(), double complex *mat,
                    int ish, int jsh, double *buf,
                    int *shls_slice, int *ao_loc, double complex fac,
                    double *Gv, double *invh, double *gxyz, int *gs, int nGv,
                    int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ish0 = shls_slice[0];
        const int ish1 = shls_slice[1];
        const int jsh0 = shls_slice[2];
        const int jsh1 = shls_slice[3];
        ish += ish0;
        jsh += jsh0;
        const int nrow = ao_loc[ish1] - ao_loc[ish0];
        const int ncol = ao_loc[jsh1] - ao_loc[jsh0];
        const size_t off = ao_loc[ish] - ao_loc[ish0] + (ao_loc[jsh] - ao_loc[jsh0]) * nrow;
        int shls[2] = {ish, jsh};
        int dims[2] = {nrow, ncol};
        (*intor)(mat+off*nGv, shls, dims, NULL, eval_gz,
                 fac, Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env);
}

void GTO_ft_fill_s1hermi(int (*intor)(), void (*eval_gz)(), double complex *mat,
                         int ish, int jsh, double complex *buf,
                         int *shls_slice, int *ao_loc, double complex fac,
                         double *Gv, double *invh, double *gxyz, int *gs, int nGv,
                         int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ish0 = shls_slice[0];
        const int jsh0 = shls_slice[2];
        ish += ish0;
        jsh += jsh0;
        const int ip = ao_loc[ish] - ao_loc[ish0];
        const int jp = ao_loc[jsh] - ao_loc[jsh0];
        if (ip < jp) {
                return;
        }

        const int ish1 = shls_slice[1];
        const int jsh1 = shls_slice[3];
        const int nrow = ao_loc[ish1] - ao_loc[ish0];
        const int ncol = ao_loc[jsh1] - ao_loc[jsh0];
        const size_t off = ao_loc[ish] - ao_loc[ish0] + (ao_loc[jsh] - ao_loc[jsh0]) * nrow;
        int shls[2] = {ish, jsh};
        int dims[2] = {nrow, ncol};
        (*intor)(mat+off*nGv, shls, dims, NULL, eval_gz,
                 fac, Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env);

        if (ip != jp && ish0 == jsh0 && ish1 == jsh1) {
                const int di = ao_loc[ish+1] - ao_loc[ish];
                const int dj = ao_loc[jsh+1] - ao_loc[jsh];
                double complex *in = mat + off * nGv;
                double complex *out = mat + (ao_loc[jsh] - ao_loc[jsh0] +
                                             (ao_loc[ish] - ao_loc[ish0]) * nrow) * nGv;
                int i, j, n;
                double complex *pout, *pin;
                for (i = 0; i < di; i++) {
                        for (j = 0; j < dj; j++) {
                                pin  = in  + nGv * (j*nrow+i);
                                pout = out + nGv * (i*nrow+j);
                                for (n = 0; n < nGv; n++) {
                                        pout[n] = pin[n];
                                }
                        }
                }
        }
}

void GTO_ft_fill_s2(int (*intor)(), void (*eval_gz)(), double complex *mat,
                    int ish, int jsh, double complex *buf,
                    int *shls_slice, int *ao_loc, double complex fac,
                    double *Gv, double *invh, double *gxyz, int *gs, int nGv,
                    int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ish0 = shls_slice[0];
        const int jsh0 = shls_slice[2];
        ish += ish0;
        jsh += jsh0;
        const int ip = ao_loc[ish];
        const int jp = ao_loc[jsh] - ao_loc[jsh0];
        if (ip < jp) {
                return;
        }

        const int di = ao_loc[ish+1] - ao_loc[ish];
        const int dj = ao_loc[jsh+1] - ao_loc[jsh];
        const int i0 = ao_loc[ish0];
        const size_t off = ip * (ip + 1) / 2 - i0 * (i0 + 1) / 2 + jp;
        int shls[2] = {ish, jsh};
        int dims[2] = {di, dj};
        (*intor)(buf, shls, dims, NULL, eval_gz,
                 fac, Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env);

        if (ip != jp) {
                zcopy_s2_igtj(mat+off*nGv, buf, nGv, ip, di, dj);
        } else {
                zcopy_s2_ieqj(mat+off*nGv, buf, nGv, ip, di, dj);
        }
}

/*
 * Fourier transform AO pairs and add to mat (inplace)
 */
void GTO_ft_ovlp_mat(int (*intor)(), void (*eval_gz)(), void (*fill)(),
                     double complex *mat, int *shls_slice, int *ao_loc, double phase,
                     double *Gv, double *invh, double *gxyz, int *gs, int nGv,
                     int *atm, int natm, int *bas, int nbas, double *env)
{
        const int ish0 = shls_slice[0];
        const int ish1 = shls_slice[1];
        const int jsh0 = shls_slice[2];
        const int jsh1 = shls_slice[3];
        const int nish = ish1 - ish0;
        const int njsh = jsh1 - jsh0;
        const double complex fac = cos(phase) + sin(phase)*_Complex_I;

#pragma omp parallel default(none) \
        shared(intor, eval_gz, fill, mat, shls_slice, ao_loc, \
               Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env)
{
        int i, j, ij;
        double complex *buf = malloc(sizeof(double complex) * NCTRMAX*NCTRMAX*nGv);
#pragma omp for schedule(dynamic)
        for (ij = 0; ij < nish*njsh; ij++) {
                i = ij / njsh;
                j = ij % njsh;
                (*fill)(intor, eval_gz, mat, i, j, buf, shls_slice, ao_loc, fac,
                        Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env);
        }
        free(buf);
}
}


/*
 * Given npair of shls in shls_lst, FT their AO pair value and add to
 * out (inplace)
 */
void GTO_ft_ovlp_shls(int (*intor)(), void (*eval_gz)(), double complex *out,
                      int npair, int *shls_lst, int *ao_loc, double phase,
                      double *Gv, double *invh, int *gxyz, int *gs, int nGv,
                      int *atm, int natm, int *bas, int nbas, double *env)
{
        int n, di, dj, ish, jsh;
        int *ijloc = malloc(sizeof(int) * npair);
        ijloc[0] = 0;
        for (n = 1; n < npair; n++) {
                ish = shls_lst[n*2-2];
                jsh = shls_lst[n*2-1];
                di = ao_loc[ish+1] - ao_loc[ish];
                dj = ao_loc[jsh+1] - ao_loc[jsh];
                ijloc[n] = ijloc[n-1] + di*dj;
        }
        const double complex fac = cos(phase) + sin(phase)*_Complex_I;

#pragma omp parallel default(none) \
        shared(intor, out, Gv, invh, gxyz, gs, nGv, npair, shls_lst, ao_loc, \
               eval_gz, atm, natm, bas, nbas, env, ijloc) \
        private(n)
{
        int ish, jsh;
        int dims[2];
#pragma omp for schedule(dynamic)
        for (n = 0; n < npair; n++) {
                ish = shls_lst[n*2  ];
                jsh = shls_lst[n*2+1];
                dims[0] = ao_loc[ish+1] - ao_loc[ish];
                dims[1] = ao_loc[jsh+1] - ao_loc[jsh];
                (*intor)(out+(size_t)ijloc[n]*nGv, shls_lst+n*2, dims, NULL, eval_gz,
                         fac, Gv, invh, gxyz, gs, nGv, atm, natm, bas, nbas, env);
        }
}
        free(ijloc);
}

