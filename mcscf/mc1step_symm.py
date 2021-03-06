#!/usr/bin/env python
#
# Author: Qiming Sun <osirpt.sun@gmail.com>
#

import numpy
from pyscf import symm
from pyscf.lib import logger
from pyscf.mcscf import mc1step
from pyscf.mcscf import mc2step
from pyscf.mcscf import casci_symm
from pyscf import fci


class CASSCF(mc1step.CASSCF):
    __doc__ = mc1step.CASSCF.__doc__
    def __init__(self, mf, ncas, nelecas, ncore=None, frozen=None):
        assert(mf.mol.symmetry)
        self.orbsym = []
        mc1step.CASSCF.__init__(self, mf, ncas, nelecas, ncore, frozen)
        self.fcisolver = fci.solver(mf.mol, self.nelecas[0]==self.nelecas[1], True)

    def mc1step(self, mo_coeff=None, ci0=None, callback=None):
        return self.kernel(mo_coeff, ci0, callback, mc1step.kernel)

    def mc2step(self, mo_coeff=None, ci0=None, callback=None):
        return self.kernel(mo_coeff, ci0, callback, mc2step.kernel)

    def kernel(self, mo_coeff=None, ci0=None, callback=None, _kern=None):
        if mo_coeff is None:
            mo_coeff = self.mo_coeff
        else:
            self.mo_coeff = mo_coeff
        if callback is None: callback = self.callback
        if _kern is None: _kern = mc1step.kernel

        if self.verbose >= logger.WARN:
            self.check_sanity()
        self.dump_flags()
        log = logger.Logger(self.stdout, self.verbose)

        casci_symm.label_symmetry_(self, self.mo_coeff)

        if (hasattr(self.fcisolver, 'wfnsym') and
            self.fcisolver.wfnsym is None and
            hasattr(self.fcisolver, 'guess_wfnsym')):
            wfnsym = self.fcisolver.guess_wfnsym(self.ncas, self.nelecas, ci0,
                                                 verbose=log)
            wfnsym = symm.irrep_id2name(self.mol.groupname, wfnsym)
            log.info('Active space CI wfn symmetry = %s', wfnsym)

        self.converged, self.e_tot, self.e_cas, self.ci, \
                self.mo_coeff, self.mo_energy = \
                _kern(self, mo_coeff,
                      tol=self.conv_tol, conv_tol_grad=self.conv_tol_grad,
                      ci0=ci0, callback=callback, verbose=self.verbose)
        log.note('CASSCF energy = %.15g', self.e_tot)
        self._finalize()
        return self.e_tot, self.e_cas, self.ci, self.mo_coeff, self.mo_energy

    def uniq_var_indices(self, nmo, ncore, ncas, frozen):
        mask = mc1step.CASSCF.uniq_var_indices(self, nmo, ncore, ncas, frozen)
# Call _symmetrize function to remove the symmetry forbidden matrix elements
# (by setting their mask value to 0 in _symmetrize).  Then pack_uniq_var and
# unpack_uniq_var function only operates on those symmetry allowed matrix
# elements.
        return _symmetrize(mask, self.orbsym, self.mol.groupname)

    def _eig(self, mat, b0, b1):
        return casci_symm.eig(mat, numpy.array(self.orbsym[b0:b1]))

    def cas_natorb_(self, mo_coeff=None, ci=None, eris=None, sort=False,
                    casdm1=None, verbose=None):
        self.mo_coeff, self.ci, occ = self.cas_natorb(mo_coeff, ci, eris,
                                                      sort, casdm1, verbose)
        if sort:
            casci_symm.label_symmetry_(self, self.mo_coeff)
        return self.mo_coeff, self.ci, occ

    def canonicalize_(self, mo_coeff=None, ci=None, eris=None, sort=False,
                      cas_natorb=False, casdm1=None, verbose=None):
        self.mo_coeff, ci, self.mo_energy = \
                self.canonicalize(mo_coeff, ci, eris,
                                  sort, cas_natorb, casdm1, verbose)
        if sort:
            casci_symm.label_symmetry_(self, self.mo_coeff)
        if cas_natorb:  # When active space is changed, the ci solution needs to be updated
            self.ci = ci
        return self.mo_coeff, ci, self.mo_energy

def _symmetrize(mat, orbsym, groupname):
    mat1 = numpy.zeros_like(mat)
    orbsym = numpy.asarray(orbsym)
    allowed = orbsym.reshape(-1,1) == orbsym
    mat1[allowed] = mat[allowed]
    return mat1


if __name__ == '__main__':
    from pyscf import gto
    from pyscf import scf
    import pyscf.fci
    from pyscf.mcscf import addons

    mol = gto.Mole()
    mol.verbose = 0
    mol.output = None

    mol.atom = [
        ['O', ( 0., 0.    , 0.   )],
        ['H', ( 0., -0.757, 0.587)],
        ['H', ( 0., 0.757 , 0.587)],]
    mol.basis = {'H': 'cc-pvdz',
                 'O': 'cc-pvdz',}
    mol.symmetry = 1
    mol.build()

    m = scf.RHF(mol)
    ehf = m.scf()
    mc = CASSCF(m, 6, 4)
    mc.fcisolver = pyscf.fci.solver(mol)
    mc.verbose = 4
    mo = addons.sort_mo(mc, m.mo_coeff, (3,4,6,7,8,9), 1)
    emc = mc.mc1step(mo)[0]
    print(ehf, emc, emc-ehf)
    #-76.0267656731 -76.0873922924 -0.0606266193028
    print(emc - -76.0873923174, emc - -76.0926176464)

    mc = CASSCF(m, 6, (3,1))
    #mc.fcisolver = pyscf.fci.direct_spin1
    mc.fcisolver = pyscf.fci.solver(mol, False)
    mc.verbose = 4
    emc = mc.mc1step(mo)[0]
    print(emc - -75.7155632535814)

    mc = CASSCF(m, 6, (3,1))
    mc.fcisolver.wfnsym = 'B1'
    mc.verbose = 4
    emc = mc.mc1step(mo)[0]
    print(emc - -75.6406597705231)
