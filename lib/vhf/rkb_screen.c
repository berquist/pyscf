/*
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <assert.h>
#include "cint.h"
#include "cvhf.h"
#include "fblas.h"
#include "optimizer.h"

#define MAX(I,J)        ((I) > (J) ? (I) : (J))

#define LL 0
#define SS 1
#define SL 2
#define LS 3


int cint2e();
int cint2e_spsp1spsp2();

int CVHFrkbllll_prescreen(int *shls, CVHFOpt *opt,
                          int *atm, int *bas, double *env)
{
        if (!opt) {
                return 1; // no screen
        }
        int i = shls[0];
        int j = shls[1];
        int k = shls[2];
        int l = shls[3];
        int n = opt->nbas;
        assert(opt->q_cond);
        assert(opt->dm_cond);
        assert(i < n);
        assert(j < n);
        assert(k < n);
        assert(l < n);
        double qijkl = opt->q_cond[i*n+j] * opt->q_cond[k*n+l];
        double dmin = opt->direct_scf_cutoff * qijkl;
        return (opt->dm_cond[j*n+i] > dmin)
            || (opt->dm_cond[l*n+k] > dmin)
            || (opt->dm_cond[j*n+k] > dmin)
            || (opt->dm_cond[j*n+l] > dmin)
            || (opt->dm_cond[i*n+k] > dmin)
            || (opt->dm_cond[i*n+l] > dmin);
}

int CVHFrkbllll_vkscreen(int *shls, CVHFOpt *opt,
                         double **dms_cond, int n_dm, double *dm_atleast,
                         int *atm, int *bas, double *env)
{
        int i = shls[0];
        int j = shls[1];
        int k = shls[2];
        int l = shls[3];
        int nbas = opt->nbas;
        int idm;
        double qijkl = opt->q_cond[i*nbas+j] * opt->q_cond[k*nbas+l];
        double *pdmscond = opt->dm_cond + nbas*nbas;
        for (idm = 0; idm < n_dm/2; idm++) {
// note in _vhf.rdirect_mapdm, J and K share the same DM
                dms_cond[idm*2+0] = pdmscond + idm*nbas*nbas; // for vj
                dms_cond[idm*2+1] = pdmscond + idm*nbas*nbas; // for vk
        }
        *dm_atleast = opt->direct_scf_cutoff * qijkl;
        return 1;
}

int CVHFrkbssll_prescreen(int *shls, CVHFOpt *opt,
                          int *atm, int *bas, double *env)
{
        if (!opt) {
                return 1; // no screen
        }
        int i = shls[0];
        int j = shls[1];
        int k = shls[2];
        int l = shls[3];
        int n = opt->nbas;
        assert(opt->q_cond);
        assert(opt->dm_cond);
        assert(i < n);
        assert(j < n);
        assert(k < n);
        assert(l < n);
        double *dmsl = opt->dm_cond + n*n*SL;
        double qijkl = opt->q_cond[n*n*SS+i*n+j] * opt->q_cond[k*n+l];
        double dmin = opt->direct_scf_cutoff * qijkl;
        return (opt->dm_cond[n*n*SS+j*n+i] > dmin)
            || (opt->dm_cond[l*n+k] > dmin)
            || (dmsl[j*n+k] > dmin)
            || (dmsl[j*n+l] > dmin)
            || (dmsl[i*n+k] > dmin)
            || (dmsl[i*n+l] > dmin);
}

// be careful with the order in dms_cond, the current order (dmll, dmss, dmsl)
// is consistent to the function _call_veff_ssll in dhf.py
int CVHFrkbssll_vkscreen(int *shls, CVHFOpt *opt,
                         double **dms_cond, int n_dm, double *dm_atleast,
                         int *atm, int *bas, double *env)
{
        int i = shls[0];
        int j = shls[1];
        int k = shls[2];
        int l = shls[3];
        int nbas = opt->nbas;
        int idm;
        double qijkl = opt->q_cond[nbas*nbas*SS+i*nbas+j] * opt->q_cond[k*nbas+l];
        double *pdmscond = opt->dm_cond + 4*nbas*nbas;
        int nset = n_dm / 3;
        double *dmscondll = pdmscond + nset*nbas*nbas*LL;
        double *dmscondss = pdmscond + nset*nbas*nbas*SS;
        double *dmscondsl = pdmscond + nset*nbas*nbas*SL;
        for (idm = 0; idm < nset; idm++) {
                dms_cond[nset*0+idm] = dmscondll + idm*nbas*nbas;
                dms_cond[nset*1+idm] = dmscondss + idm*nbas*nbas;
                dms_cond[nset*2+idm] = dmscondsl + idm*nbas*nbas;
        }
        *dm_atleast = opt->direct_scf_cutoff * qijkl;
        return 1;
}


static void set_qcond(int (*intor)(), double *qcond,
                      int *atm, int natm, int *bas, int nbas, double *env)
{
        double complex *buf;
        double qtmp;
        int i, j, di, dj, ish, jsh;
        int shls[4];
        for (ish = 0; ish < nbas; ish++) {
                di = CINTcgto_spinor(ish, bas);
                for (jsh = 0; jsh <= ish; jsh++) {
                        dj = CINTcgto_spinor(jsh, bas);
                        buf = malloc(sizeof(double complex) * di*dj*di*dj);
                        shls[0] = ish;
                        shls[1] = jsh;
                        shls[2] = ish;
                        shls[3] = jsh;
                        qtmp = 0;
                        if (0 != (*intor)(buf, shls, atm, natm, bas, nbas, env, NULL)) {
                                for (i = 0; i < di; i++) {
                                for (j = 0; j < dj; j++) {
                                        qtmp = MAX(qtmp, cabs(buf[i+di*j+di*dj*i+di*dj*di*j]));
                                } }
                        }
                        qtmp = 1./sqrt(qtmp+1e-60);
                        qcond[ish*nbas+jsh] = qtmp;
                        qcond[jsh*nbas+ish] = qtmp;
                        free(buf);

                }
        }
}

void CVHFrkbllll_direct_scf(CVHFOpt *opt, int *atm, int natm,
                            int *bas, int nbas, double *env)
{
        if (opt->q_cond) {
                free(opt->q_cond);
        }
        opt->q_cond = (double *)malloc(sizeof(double) * nbas*nbas);

        set_qcond(cint2e, opt->q_cond, atm, natm, bas, nbas, env);
}

void CVHFrkbssss_direct_scf(CVHFOpt *opt, int *atm, int natm,
                            int *bas, int nbas, double *env)
{
        if (opt->q_cond) {
                free(opt->q_cond);
        }
        opt->q_cond = (double *)malloc(sizeof(double) * nbas*nbas);

        const int INC1 = 1;
        int nn = nbas * nbas;
        // c1 = 1 / ... because "qcond" has been set to 1/qijkl in set_qcond
        double c1 = 1/(.25/(env[PTR_LIGHT_SPEED]*env[PTR_LIGHT_SPEED]));
        set_qcond(cint2e_spsp1spsp2, opt->q_cond, atm, natm, bas, nbas, env);
        dscal_(&nn, &c1, opt->q_cond, &INC1);
}


void CVHFrkbssll_direct_scf(CVHFOpt *opt, int *atm, int natm,
                            int *bas, int nbas, double *env)
{
        if (opt->q_cond) {
                free(opt->q_cond);
        }
        opt->q_cond = (double *)malloc(sizeof(double) * nbas*nbas*2);

        const int INC1 = 1;
        int nn = nbas * nbas;
        double c1 = 1/(.25/(env[PTR_LIGHT_SPEED]*env[PTR_LIGHT_SPEED]));
        set_qcond(cint2e, opt->q_cond, atm, natm, bas, nbas, env);
        set_qcond(cint2e_spsp1spsp2, opt->q_cond+nbas*nbas,
                  atm, natm, bas, nbas, env);
        dscal_(&nn, &c1, opt->q_cond+nbas*nbas, &INC1);
}

static void set_dmcond(double *dmcond, double *dmscond, double complex *dm,
                       double direct_scf_cutoff, int nset,
                       int *atm, int natm, int *bas, int nbas, double *env)
{
        int *ao_loc = malloc(sizeof(int) * (nbas+1));
        CINTshells_spinor_offset(ao_loc, bas, nbas);
        ao_loc[nbas] = ao_loc[nbas-1] + CINTcgto_spinor(nbas-1, bas);
        int nao = ao_loc[nbas];

        double dmax, dmaxi;
        int i, j, ish, jsh;
        int iset;
        double complex *pdm;

        for (ish = 0; ish < nbas; ish++) {
        for (jsh = 0; jsh < nbas; jsh++) {
                dmax = 0;
                for (iset = 0; iset < nset; iset++) {
                        dmaxi = 0;
                        pdm = dm + nao*nao*iset;
                        for (i = ao_loc[ish]; i < ao_loc[ish+1]; i++) {
                        for (j = ao_loc[jsh]; j < ao_loc[jsh+1]; j++) {
                                dmaxi = MAX(dmaxi, cabs(pdm[i*nao+j]));
                        } }
                        dmscond[iset*nbas*nbas+ish*nbas+jsh] = dmaxi;
                        dmax = MAX(dmax, dmaxi);
                }
                dmcond[ish*nbas+jsh] = dmax;
        } }
        free(ao_loc);
}

//  dm_cond ~ 1+nset, dm_cond + dms_cond
void CVHFrkbllll_direct_scf_dm(CVHFOpt *opt, double complex *dm, int nset,
                               int *atm, int natm, int *bas, int nbas, double *env)
{
        if (opt->dm_cond) { // NOT reuse opt->dm_cond because nset may be diff in different call
                free(opt->dm_cond);
        }
        opt->dm_cond = (double *)malloc(sizeof(double)*nbas*nbas*(1+nset));
        memset(opt->dm_cond, 0, sizeof(double)*nbas*nbas*(1+nset));
        // dmcond followed by dmscond which are max matrix element for each dm
        set_dmcond(opt->dm_cond, opt->dm_cond+nbas*nbas, dm,
                   opt->direct_scf_cutoff, nset, atm, natm, bas, nbas, env);
}

void CVHFrkbssss_direct_scf_dm(CVHFOpt *opt, double complex *dm, int nset,
                               int *atm, int natm, int *bas, int nbas,
                               double *env)
{
        if (opt->dm_cond) {
                free(opt->dm_cond);
        }
        opt->dm_cond = (double *)malloc(sizeof(double)*nbas*nbas*(1+nset));
        memset(opt->dm_cond, 0, sizeof(double)*nbas*nbas*(1+nset));
        set_dmcond(opt->dm_cond, opt->dm_cond+nbas*nbas, dm,
                   opt->direct_scf_cutoff, nset, atm, natm, bas, nbas, env);
}

// the current order of dmscond (dmll, dmss, dmsl) is consistent to the
// function _call_veff_ssll in dhf.py
void CVHFrkbssll_direct_scf_dm(CVHFOpt *opt, double complex *dm, int nset,
                               int *atm, int natm, int *bas, int nbas,
                               double *env)
{
        if (opt->dm_cond) {
                free(opt->dm_cond);
        }
        nset = nset / 3;
        opt->dm_cond = (double *)malloc(sizeof(double)*nbas*nbas*4*(1+nset));
        memset(opt->dm_cond, 0, sizeof(double)*nbas*nbas*4*(1+nset));

        // 4 types of dmcond (LL,SS,SL,SS) followed by 4 types of dmscond
        int n2c = CINTtot_cgto_spinor(bas, nbas);
        double *dmcondll = opt->dm_cond + nbas*nbas*LL;
        double *dmcondss = opt->dm_cond + nbas*nbas*SS;
        double *dmcondsl = opt->dm_cond + nbas*nbas*SL;
        //double *dmcondls = opt->dm_cond + nbas*nbas*LS;
        double *pdmscond = opt->dm_cond + nbas*nbas*4;
        double *dmscondll = pdmscond + nset*nbas*nbas*LL;
        double *dmscondss = pdmscond + nset*nbas*nbas*SS;
        double *dmscondsl = pdmscond + nset*nbas*nbas*SL;
        //double *dmscondls = dmscond + nset*nbas*nbas*LS;
        double complex *dmll = dm + n2c*n2c*LL*nset;
        double complex *dmss = dm + n2c*n2c*SS*nset;
        double complex *dmsl = dm + n2c*n2c*SL*nset;
        //double complex *dmls = dm + n2c*n2c*LS*nset;

        set_dmcond(dmcondll, dmscondll, dmll,
                   opt->direct_scf_cutoff, nset, atm, natm, bas, nbas, env);
        set_dmcond(dmcondss, dmscondss, dmss,
                   opt->direct_scf_cutoff, nset, atm, natm, bas, nbas, env);
        set_dmcond(dmcondsl, dmscondsl, dmsl,
                   opt->direct_scf_cutoff, nset, atm, natm, bas, nbas, env);
}
