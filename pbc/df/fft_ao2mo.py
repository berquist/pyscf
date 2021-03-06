#!/usr/bin/env python
#
# Author: Qiming Sun <osirpt.sun@gmail.com>
#

'''
Integral transformation with FFT

(ij|kl) = \int dr1 dr2 i*(r1) j(r1) v(r12) k*(r2) l(r2)
        = (ij|G) v(G) (G|kl)

i*(r) j(r) = 1/N \sum_G e^{iGr}  (G|ij)
           = 1/N \sum_G e^{-iGr} (ij|G)

"forward" FFT:
    (G|ij) = \sum_r e^{-iGr} i*(r) j(r) = fft[ i*(r) j(r) ]
"inverse" FFT:
    (ij|G) = \sum_r e^{iGr} i*(r) j(r) = N * ifft[ i*(r) j(r) ]
           = conj[ \sum_r e^{-iGr} j*(r) i(r) ]
'''

import time
import numpy
from pyscf import lib
from pyscf import ao2mo
from pyscf.ao2mo import _ao2mo
from pyscf.lib import logger
from pyscf.pbc import dft as pdft
from pyscf.pbc import tools


def get_eri(mydf, kpts=None, compact=False):
    cell = mydf.cell
    if kpts is None:
        kptijkl = numpy.zeros((4,3))
    elif numpy.shape(kpts) == (3,):
        kptijkl = numpy.vstack([kpts]*4)
    else:
        kptijkl = numpy.reshape(kpts, (4,3))

    kpti, kptj, kptk, kptl = kptijkl
    nao = cell.nao_nr()
    nao_pair = nao * (nao+1) // 2
    coulG = tools.get_coulG(cell, kptj-kpti, gs=mydf.gs)
    ngs = len(coulG)

####################
# gamma point, the integral is real and with s4 symmetry
    if abs(kptijkl).sum() < 1e-9:
        ao_pairs_G = get_ao_pairs_G(mydf, kptijkl[:2], compact)
        ao_pairs_G *= numpy.sqrt(coulG).reshape(-1,1)
        aoijR = ao_pairs_G.real.copy()
        aoijI = ao_pairs_G.imag.copy()
        ao_pairs_G = None
        eri = lib.dot(aoijR.T, aoijR, cell.vol/ngs**2)
        eri = lib.dot(aoijI.T, aoijI, cell.vol/ngs**2, eri, 1)
        return eri

####################
# (kpt) i == j == k == l != 0
# (kpt) i == l && j == k && i != j && j != k  =>
#
# complex integrals, N^4 elements
    elif (abs(kpti-kptl).sum() < 1e-9) and (abs(kptj-kptk).sum() < 1e-9):
        ao_pairs_G = get_ao_pairs_G(mydf, kptijkl[:2], False)
        ao_pairs_G *= numpy.sqrt(coulG).reshape(-1,1)
        ao_pairs_invG = ao_pairs_G.T.reshape(nao,nao,-1).transpose(1,0,2).conj()
        ao_pairs_invG = ao_pairs_invG.reshape(-1,ngs)
        return lib.dot(ao_pairs_G.T, ao_pairs_invG.T, cell.vol/ngs**2)

####################
# aosym = s1, complex integrals
#
    else:
        ao_pairs_G = get_ao_pairs_G(mydf, kptijkl[:2], False)
# ao_pairs_invG = rho_rs(-G+k_rs) = conj(rho_sr(G+k_sr)).swap(r,s)
        ao_pairs_invG = get_ao_pairs_G(mydf, -kptijkl[2:], False).conj()
        ao_pairs_G *= coulG.reshape(-1,1)
        return lib.dot(ao_pairs_G.T, ao_pairs_invG, cell.vol/ngs**2)


def general(mydf, mo_coeffs, kpts=None, compact=False):
    cell = mydf.cell
    if kpts is None:
        kptijkl = numpy.zeros((4,3))
    elif numpy.shape(kpts) == (3,):
        kptijkl = numpy.vstack([kpts]*4)
    else:
        kptijkl = numpy.reshape(kpts, (4,3))

    kpti, kptj, kptk, kptl = kptijkl
    if isinstance(mo_coeffs, numpy.ndarray) and mo_coeffs.ndim == 2:
        mo_coeffs = (mo_coeffs,) * 4
    coulG = tools.get_coulG(cell, kptj-kpti, gs=mydf.gs)
    ngs = len(coulG)

####################
# gamma point, the integral is real and with s4 symmetry
    if (abs(kptijkl).sum() < 1e-9 and
        not any((numpy.iscomplexobj(mo) for mo in mo_coeffs))):
        mo_pairs_G = get_mo_pairs_G(mydf, mo_coeffs[:2], kptijkl[:2], compact)
        if ((ao2mo.incore.iden_coeffs(mo_coeffs[0],mo_coeffs[2]) and
             ao2mo.incore.iden_coeffs(mo_coeffs[1],mo_coeffs[3]))):
            mo_pairs_G *= numpy.sqrt(coulG).reshape(-1,1)
            moijR = moklR = mo_pairs_G.real.copy()
            moijI = moklI = mo_pairs_G.imag.copy()
            mo_pairs_G = None
        else:
            mo_pairs_G *= coulG
            moijR = mo_pairs_G.real.copy()
            moijI = mo_pairs_G.imag.copy()
            mo_pairs_G = None
            mo_pairs_G = get_mo_pairs_G(mydf, mo_coeffs[2:], kptijkl[2:],
                                        compact)
            moklR = mo_pairs_G.real.copy()
            moklI = mo_pairs_G.imag.copy()
            mo_pairs_G = None
        eri = lib.dot(moijR.T, moklR, cell.vol/ngs**2)
        eri = lib.dot(moijI.T, moklI, cell.vol/ngs**2, eri, 1)
        return eri

####################
# (kpt) i == j == k == l != 0
# (kpt) i == l && j == k && i != j && j != k  =>
#
# complex integrals, N^4 elements
    elif ((abs(kpti-kptl).sum() < 1e-9) and (abs(kptj-kptk).sum() < 1e-9) and
          ao2mo.incore.iden_coeffs(mo_coeffs[0],mo_coeffs[3]) and
          ao2mo.incore.iden_coeffs(mo_coeffs[1],mo_coeffs[2])):
        nmoi = mo_coeffs[0].shape[1]
        nmoj = mo_coeffs[1].shape[1]
        mo_ij_G = get_mo_pairs_G(mydf, mo_coeffs[:2], kptijkl[:2])
        mo_ij_G *= numpy.sqrt(coulG).reshape(-1,1)
        mo_kl_G = mo_ij_G.T.reshape(nmoi,nmoj,-1).transpose(1,0,2).conj()
        mo_kl_G = mo_kl_G.reshape(-1,ngs)
        return lib.dot(mo_ij_G.T, mo_kl_G.T, cell.vol/ngs**2)

####################
# aosym = s1, complex integrals
#
    else:
        nmok = mo_coeffs[2].shape[1]
        nmol = mo_coeffs[3].shape[1]
        mo_ij_G = get_mo_pairs_G(mydf, mo_coeffs[:2], kptijkl[:2])
        mo_ij_G *= coulG.reshape(-1,1)
# mo_pairs_invG = rho_rs(-G+k_rs) = conj(rho_sr(G+k_sr)).swap(r,s)
        mo_kl_G = get_mo_pairs_G(mydf, (mo_coeffs[3],mo_coeffs[2]),
                                 (kptl,kptk))
        mo_kl_G = mo_kl_G.T.reshape(nmol,nmok,-1).transpose(1,0,2).conj()
        mo_kl_G = mo_kl_G.reshape(-1,ngs)
        return lib.dot(mo_ij_G.T, mo_kl_G.T, cell.vol/ngs**2)


def get_ao_pairs_G(mydf, kpts=numpy.zeros((2,3)), compact=False):
    '''Calculate forward (G|ij) FFT of all AO pairs.

    Returns:
        ao_pairs_G : 2D complex array
            For gamma point, the shape is (ngs, nao*(nao+1)/2); otherwise the
            shape is (ngs, nao*nao)
    '''
    if kpts is None: kpts = mydf.kpts
    cell = mydf.cell
    kpts = numpy.asarray(kpts)
    coords = pdft.gen_grid.gen_uniform_grids(cell, mydf.gs)
    nao = cell.nao_nr()
    ngs = len(coords)

    def trans(aoiR, aojR, fac=1):
        if id(aoiR) == id(aojR):
            aoiR = aojR = numpy.asarray(aoiR.T, order='C')
        else:
            aoiR = numpy.asarray(aoiR.T, order='C')
            aojR = numpy.asarray(aojR.T, order='C')
        ao_pairs_G = numpy.empty((nao,nao,ngs), dtype=numpy.complex128)
        for i in range(nao):
            ao_pairs_G[i] = tools.fft(fac * aoiR[i].conj() * aojR, mydf.gs)
        ao_pairs_G = ao_pairs_G.reshape(-1,ngs).T
        return ao_pairs_G

    if compact and abs(kpts).sum() < 1e-9:  # gamma point
        aoR = mydf._numint.eval_ao(cell, coords, kpts[:1])[0]
        aoR = numpy.asarray(aoR.T, order='C')
        npair = nao*(nao+1)//2
        ao_pairs_G = numpy.empty((npair,ngs), dtype=numpy.complex128)
        ij = 0
        for i in range(nao):
            ao_pairs_G[ij:ij+i+1] = tools.fft(aoR[i] * aoR[:i+1], mydf.gs)
            ij += i + 1
        ao_pairs_G = ao_pairs_G.T

    elif abs(kpts[0]-kpts[1]).sum() < 1e-9:
        aoR = mydf._numint.eval_ao(cell, coords, kpts[:1])[0]
        ao_pairs_G = trans(aoR, aoR)

    else:
        aoiR, aojR = mydf._numint.eval_ao(cell, coords, kpts[:2])
        q = kpts[1] - kpts[0]
        fac = numpy.exp(-1j * numpy.dot(coords, q))
        ao_pairs_G = trans(aoiR, aojR, fac)

    return ao_pairs_G

def get_mo_pairs_G(mydf, mo_coeffs, kpts=numpy.zeros((2,3)), compact=False):
    '''Calculate forward (G|ij) FFT of all MO pairs.

    Args:
        mo_coeff: length-2 list of (nao,nmo) ndarrays
            The two sets of MO coefficients to use in calculating the
            product |ij).

    Returns:
        mo_pairs_G : (ngs, nmoi*nmoj) ndarray
            The FFT of the real-space MO pairs.
    '''
    if kpts is None: kpts = mydf.kpts
    cell = mydf.cell
    kpts = numpy.asarray(kpts)
    coords = pdft.gen_grid.gen_uniform_grids(cell, mydf.gs)
    nmoi = mo_coeffs[0].shape[1]
    nmoj = mo_coeffs[1].shape[1]
    ngs = len(coords)

    def trans(aoiR, aojR, fac=1):
        if id(aoiR) == id(aojR) and ao2mo.incore.iden_coeffs(mo_coeffs[0], mo_coeffs[1]):
            moiR = mojR = numpy.asarray(lib.dot(mo_coeffs[0].T,aoiR.T), order='C')
        else:
            moiR = numpy.asarray(lib.dot(mo_coeffs[0].T, aoiR.T), order='C')
            mojR = numpy.asarray(lib.dot(mo_coeffs[1].T, aojR.T), order='C')
        mo_pairs_G = numpy.empty((nmoi,nmoj,ngs), dtype=numpy.complex128)
        for i in range(nmoi):
            mo_pairs_G[i] = tools.fft(fac * moiR[i].conj() * mojR, mydf.gs)
        mo_pairs_G = mo_pairs_G.reshape(-1,ngs).T
        return mo_pairs_G

    if abs(kpts).sum() < 1e-9:  # gamma point, real
        aoR = mydf._numint.eval_ao(cell, coords, kpts[:1])[0]
        if compact and ao2mo.incore.iden_coeffs(mo_coeffs[0], mo_coeffs[1]):
            moR = numpy.asarray(lib.dot(mo_coeffs[0].T, aoR.T), order='C')
            npair = nmoi*(nmoi+1)//2
            mo_pairs_G = numpy.empty((npair,ngs), dtype=numpy.complex128)
            ij = 0
            for i in range(nmoi):
                mo_pairs_G[ij:ij+i+1] = tools.fft(moR[i].conj() * moR[:i+1], mydf.gs)
                ij += i + 1
            mo_pairs_G = mo_pairs_G.T
        else:
            mo_pairs_G = trans(aoR, aoR)

    elif abs(kpts[0]-kpts[1]).sum() < 1e-9:
        aoR = mydf._numint.eval_ao(cell, coords, kpts[:1])[0]
        mo_pairs_G = trans(aoR, aoR)

    else:
        aoiR, aojR = mydf._numint.eval_ao(cell, coords, kpts)
        q = kpts[1] - kpts[0]
        fac = numpy.exp(-1j * numpy.dot(coords, q))
        mo_pairs_G = trans(aoiR, aojR, fac)

    return mo_pairs_G


if __name__ == '__main__':
    import pyscf.pbc.gto as pgto
    from pyscf.pbc import df

    L = 5.
    n = 5
    cell = pgto.Cell()
    cell.h = numpy.diag([L,L,L])
    cell.gs = numpy.array([n,n,n])

    cell.atom = '''He    3.    2.       3.
                   He    1.    1.       1.'''
    #cell.basis = {'He': [[0, (1.0, 1.0)]]}
    #cell.basis = '631g'
    #cell.basis = {'He': [[0, (2.4, 1)], [1, (1.1, 1)]]}
    cell.basis = 'ccpvdz'
    cell.verbose = 0
    cell.build(0,0)

    nao = cell.nao_nr()
    numpy.random.seed(1)
    kpts = numpy.random.random((4,3))
    kpts[3] = -numpy.einsum('ij->j', kpts[:3])
    with_df = df.DF(cell)
    with_df.kpts = kpts
    mo =(numpy.random.random((nao,nao)) +
         numpy.random.random((nao,nao))*1j)
    eri = with_df.get_eri(kpts).reshape((nao,)*4)
    eri0 = numpy.einsum('pjkl,pi->ijkl', eri , mo.conj())
    eri0 = numpy.einsum('ipkl,pj->ijkl', eri0, mo       )
    eri0 = numpy.einsum('ijpl,pk->ijkl', eri0, mo.conj())
    eri0 = numpy.einsum('ijkp,pl->ijkl', eri0, mo       ).reshape(nao**2,-1)
    eri1 = with_df.ao2mo(mo, kpts)
    print abs(eri1-eri0).sum()
