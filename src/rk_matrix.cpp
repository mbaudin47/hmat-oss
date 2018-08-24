/*
  HMat-OSS (HMatrix library, open source software)

  Copyright (C) 2014-2015 Airbus Group SAS

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

  http://github.com/jeromerobert/hmat-oss
*/

#include "rk_matrix.hpp"
#include "h_matrix.hpp"
#include "cluster_tree.hpp"
#include <cstring> // memcpy
#include <cfloat> // DBL_MAX
#include "data_types.hpp"
#include "lapack_operations.hpp"
#include "blas_overloads.hpp"
#include "lapack_overloads.hpp"
#include "common/context.hpp"
#include "common/my_assert.h"
#include "common/timeline.hpp"
#include "lapack_exception.hpp"

namespace hmat {

/** RkApproximationControl */
template<typename T> RkApproximationControl RkMatrix<T>::approx;
int RkApproximationControl::findK(Vector<double> &sigma, double epsilon) {
  // Control of approximation for fixed approx.k != 0
  int newK = k;
  if (newK != 0) {
    newK = std::min(newK, sigma.rows);
  } else {
    assert(epsilon >= 0.);
    static char *useL2Criterion = getenv("HMAT_L2_CRITERION");
    double threshold_eigenvalue = 0.0;
    if (useL2Criterion == NULL) {
      for (int i = 0; i < sigma.rows; i++) {
        threshold_eigenvalue += sigma[i];
      }
    } else {
      threshold_eigenvalue = sigma[0];
    }
    threshold_eigenvalue *= epsilon;
    int i = 0;
    for (i = 0; i < sigma.rows; i++) {
      if (sigma[i] <= threshold_eigenvalue){
        break;
      }
    }
    newK = i;
  }
  return newK;
}


/** RkMatrix */
template<typename T> RkMatrix<T>::RkMatrix(ScalarArray<T>* _a, const IndexSet* _rows,
                                           ScalarArray<T>* _b, const IndexSet* _cols,
                                           CompressionMethod _method)
  : rows(_rows),
    cols(_cols),
    a(_a),
    b(_b),
    method(_method)
{

  // We make a special case for empty matrices.
  if ((!a) && (!b)) {
    return;
  }
  assert(a->rows == rows->size());
  assert(b->rows == cols->size());
}

template<typename T> RkMatrix<T>::~RkMatrix() {
  clear();
}


template<typename T> ScalarArray<T>* RkMatrix<T>::evalArray(ScalarArray<T>* result) const {
  if(result==NULL)
    result = new ScalarArray<T>(rows->size(), cols->size());
  if (rank())
    result->gemm('N', 'T', Constants<T>::pone, a, b, Constants<T>::zero);
  else
    result->clear();
  return result;
}

template<typename T> FullMatrix<T>* RkMatrix<T>::eval() const {
  FullMatrix<T>* result = new FullMatrix<T>(rows, cols);
  evalArray(&result->data);
  return result;
}

// Compute squared Frobenius norm
template<typename T> double RkMatrix<T>::normSqr() const {
  return a->norm_abt_Sqr(*b);
}

template<typename T> void RkMatrix<T>::scale(T alpha) {
  // We need just to scale the first matrix, A.
  if (a) {
    a->scale(alpha);
  }
}

template<typename T> void RkMatrix<T>::transpose() {
  std::swap(a, b);
  std::swap(rows, cols);
}

template<typename T> void RkMatrix<T>::clear() {
  delete a;
  delete b;
  a = NULL;
  b = NULL;
}

template<typename T>
void RkMatrix<T>::gemv(char trans, T alpha, const ScalarArray<T>* x, T beta, ScalarArray<T>* y) const {
  if (rank() == 0) {
    if (beta != Constants<T>::pone) {
      y->scale(beta);
    }
    return;
  }
  if (trans == 'N') {
    // Compute Y <- Y + alpha * A * B^T * X
    ScalarArray<T> z(b->cols, x->cols);
    z.gemm('T', 'N', Constants<T>::pone, b, x, Constants<T>::zero);
    y->gemm('N', 'N', alpha, a, &z, beta);
  } else if (trans == 'T') {
    // Compute Y <- Y + alpha * (A*B^T)^T * X = Y + alpha * B * A^T * X
    ScalarArray<T> z(a->cols, x->cols);
    z.gemm('T', 'N', Constants<T>::pone, a, x, Constants<T>::zero);
    y->gemm('N', 'N', alpha, b, &z, beta);
  } else {
    assert(trans == 'C');
    // Compute Y <- Y + alpha * (A*B^T)^H * X = Y + alpha * conj(B) * A^H * X
    ScalarArray<T> z(a->cols, x->cols);
    z.gemm('C', 'N', Constants<T>::pone, a, x, Constants<T>::zero);
    ScalarArray<T> * newB = b->copy();
    newB->conjugate();
    y->gemm('N', 'N', alpha, newB, &z, beta);
    delete newB;
  }
}

template<typename T> const RkMatrix<T>* RkMatrix<T>::subset(const IndexSet* subRows,
                                                            const IndexSet* subCols) const {
  assert(subRows->isSubset(*rows));
  assert(subCols->isSubset(*cols));
  ScalarArray<T>* subA = NULL;
  ScalarArray<T>* subB = NULL;
  if(rank() > 0) {
    // The offset in the matrix, and not in all the indices
    int rowsOffset = subRows->offset() - rows->offset();
    int colsOffset = subCols->offset() - cols->offset();
    subA = new ScalarArray<T>(*a, rowsOffset, subRows->size(), 0, rank());
    subB = new ScalarArray<T>(*b, colsOffset, subCols->size(), 0, rank());
  }
  return new RkMatrix<T>(subA, subRows, subB, subCols, method);
}

template<typename T> size_t RkMatrix<T>::compressedSize() {
    return ((size_t)rows->size()) * rank() + ((size_t)cols->size()) * rank();
}

template<typename T> size_t RkMatrix<T>::uncompressedSize() {
    return ((size_t)rows->size()) * cols->size();
}

template<typename T> void RkMatrix<T>::addRand(double epsilon) {
  DECLARE_CONTEXT;
  a->addRand(epsilon);
  b->addRand(epsilon);
  return;
}

template<typename T> void RkMatrix<T>::truncate(double epsilon, int initialPivotA, int initialPivotB) {
  DECLARE_CONTEXT;
  static char *useInitPivot = getenv("HMAT_TRUNC_INITPIV");
  if (!useInitPivot) {
    initialPivotA=0;
    initialPivotB=0;
  }
  assert(initialPivotA>=0 && initialPivotA<=rank());
  assert(initialPivotB>=0 && initialPivotB<=rank());

  if (rank() == 0) {
    assert(!(a || b));
    return;
  }

  assert(rows->size() >= rank());
  // Case: more columns than one dimension of the matrix.
  // In this case, the calculation of the SVD of the matrix "R_a R_b^t" is more
  // expensive than computing the full SVD matrix. We make then a full matrix conversion,
  // and compress it with RkMatrix::fromMatrix().
  // TODO: in this case, the epsilon of recompression is not respected
  if (rank() > std::min(rows->size(), cols->size())) {
    FullMatrix<T>* tmp = eval();
    RkMatrix<T>* rk = truncatedSvd(tmp, epsilon); // TODO compress with something else than SVD (rank() can still be quite large) ?
    delete tmp;
    // "Move" rk into this, and delete the old "this".
    swap(*rk);
    delete rk;
    return;
  }

  static bool usedRecomp = getenv("HMAT_RECOMPRESS") && strcmp(getenv("HMAT_RECOMPRESS"), "MGS") == 0 ;
  if (usedRecomp){
    mGSTruncate(epsilon, initialPivotA, initialPivotB);
    return;
  }

  /* To recompress an Rk-matrix to Rk-matrix, we need :
      - A = Q_a R_A (QR decomposition)
      - B = Q_b R_b (QR decomposition)
      - Calculate the SVD of R_a R_b^t  = U S V^t
      - Make truncation U, S, and V in the same way as for
      compression of a full rank matrix, ie:
      - Restrict U to its newK first columns U_tilde
      - Restrict S to its newK first values (diagonal matrix) S_tilde
      - Restrict V to its newK first columns V_tilde
      - Put A = Q_a U_tilde SQRT (S_tilde)
      B = Q_b V_tilde SQRT(S_tilde)

     The sizes of the matrices are:
      - Qa : rows x k
      - Ra : k x k
      - Qb : cols x k
      - Rb : k x k
     So:
      - Ra Rb^t: k x k
      - U  : k * k
      - S  : k (diagonal)
      - V^t: k * k
     Hence:
      - newA: rows x newK
      - newB: cols x newK

  */
  // Matrices created by the SVD
  ScalarArray<T> *u = NULL, *v = NULL;
  Vector<double> *sigma = NULL;
  {
    // QR decomposition of A and B
    ScalarArray<T> ra(rank(), rank());
    a->qrDecomposition(&ra, initialPivotA); // A contains Qa and tau_a
    ScalarArray<T> rb(rank(), rank());
    b->qrDecomposition(&rb, initialPivotB); // B contains Qb and tau_b

    // R <- Ra Rb^t
    ScalarArray<T> r(rank(), rank());
    r.gemm('N','T', Constants<T>::pone, &ra, &rb , Constants<T>::zero);

    // SVD of Ra Rb^t (allows failure)
    r.svdDecomposition(&u, &sigma, &v, true); // TODO use something else than SVD ?
  }

  // Control of approximation
  int newK = approx.findK(*sigma, epsilon);
  if (newK == 0)
  {
    delete u;
    delete v;
    delete sigma;
    clear();
    return;
  }

  // Resize u, sigma, v (not very clean...)
  u->cols =newK;
  sigma->rows = newK;
  v->cols =newK;

  // We put the square root of singular values in sigma
  for (int i = 0; i < newK; i++) {
    (*sigma)[i] = sqrt((*sigma)[i]);
  }
  // TODO why not rather apply SigmaTilde to only a or b, and avoid computing square roots ?
  // we first calculate Utilde * SQRT (SigmaTilde) and VTilde * SQRT(SigmaTilde)
  u->multiplyWithDiag(sigma);
  v->multiplyWithDiag(sigma);
  delete sigma;

  // We need to calculate Qa * Utilde * SQRT (SigmaTilde)
  ScalarArray<T>* newA = new ScalarArray<T>(rows->size(), newK);

  if (initialPivotA) {
    // If there is an initial pivot, we must compute the product by Q in two parts
    // first the column >= initialPivotA, obtained from lapack GETRF, will overwrite newA when calling UNMQR
    // then the first initialPivotA columns, with a classical GEMM, will add the result in newA

    // create subset of a (columns>=initialPivotA) and u (rows>=initialPivotA)
    ScalarArray<T> sub_a(*a, 0, a->rows, initialPivotA, a->cols-initialPivotA);
    ScalarArray<T> sub_u(*u, initialPivotA, u->rows-initialPivotA, 0, u->cols);
    newA->copyMatrixAtOffset(&sub_u, 0, 0);
    // newA <- Qa * newA (et newA = Utilde * SQRT(SigmaTilde))
    sub_a.productQ('L', 'N', newA);

    // then add the regular part of the product by Q
    ScalarArray<T> sub_a2(*a, 0, a->rows, 0, initialPivotA);
    ScalarArray<T> sub_u2(*u, 0, initialPivotA, 0, u->cols);
    newA->gemm('N', 'N', Constants<T>::pone, &sub_a2, &sub_u2, Constants<T>::pone);
  } else {
    // If no initialPivotA, then no gemm, just a productQ()
    newA->copyMatrixAtOffset(u, 0, 0);
    // newA <- Qa * newA
    a->productQ('L', 'N', newA);
  }

  newA->setOrtho(u->getOrtho());
  delete u;
  delete a;
  a = newA;

  // newB = Qb * VTilde * SQRT(SigmaTilde)
  ScalarArray<T>* newB = new ScalarArray<T>(cols->size(), newK);

  if (initialPivotB) {
    // create subset of b (columns>=initialPivotB) and v (rows>=initialPivotB)
    ScalarArray<T> sub_b(*b, 0, b->rows, initialPivotB, b->cols-initialPivotB);
    ScalarArray<T> sub_v(*v, initialPivotB, v->rows-initialPivotB, 0, v->cols);
    newB->copyMatrixAtOffset(&sub_v, 0, 0);
    // newB <- Qb * newB (et newB = Vtilde * SQRT(SigmaTilde))
    sub_b.productQ('L', 'N', newB);

    // then add the regular part of the product by Q
    ScalarArray<T> sub_b2(*b, 0, b->rows, 0, initialPivotB);
    ScalarArray<T> sub_v2(*v, 0, initialPivotB, 0, v->cols);
    newB->gemm('N', 'N', Constants<T>::pone, &sub_b2, &sub_v2, Constants<T>::pone);
  } else {
    // If no initialPivotB, then no gemm, just a productQ()
    newB->copyMatrixAtOffset(v, 0, 0);
    // newB <- Qb * newB
    b->productQ('L', 'N', newB);
  }

  newB->setOrtho(v->getOrtho());
  delete v;
  delete b;
  b = newB;
}

template<typename T> void RkMatrix<T>::mGSTruncate(double epsilon, int initialPivotA, int initialPivotB) {
  DECLARE_CONTEXT;
  if (rank() == 0) {
    assert(!(a || b));
    return;
  }

  ScalarArray<T>* ur = NULL;
  Vector<double>* sr = NULL;
  ScalarArray<T>* vr = NULL;
  int kA, kB, newK;

  int krank = rank();

  // Limit scope to automatically destroy ra, rb and matR
  {
    // Gram-Schmidt on a
    ScalarArray<T> ra(krank, krank);
    kA = a->modifiedGramSchmidt( &ra, epsilon, initialPivotA);
    if (kA==0) {
      clear();
      return;
    }
    // On input, a0(m,k)
    // On output, a(m,kA), ra(kA,k) such that a0 = a * ra

    // Gram-Schmidt on b
    ScalarArray<T> rb(krank, krank);
    kB = b->modifiedGramSchmidt( &rb, epsilon, initialPivotB);
    if (kB==0) {
      clear();
      return;
    }
    // On input, b0(p,k)
    // On output, b(p,kB), rb(kB,k) such that b0 = b * rb

    // M = a0*b0^T = a*(ra*rb^T)*b^T
    // We perform an SVD on ra*rb^T:
    //  (ra*rb^T) = U*S*S*Vt
    // and M = (a*U*S)*(S*Vt*b^T) = (a*U*S)*(b*(S*Vt)^T)^T
    ScalarArray<T> matR(kA, kB);
    matR.gemm('N','T', Constants<T>::pone, &ra, &rb , Constants<T>::zero);

    // SVD (allows failure)
    matR.svdDecomposition(&ur, &sr, &vr, true);
    // On output, ur->rows = kA, vr->rows = kB
  }

  // Remove small singular values and compute square root of sr
  newK = approx.findK(*sr, epsilon);
  if (newK == 0)
  {
    delete ur;
    delete vr;
    delete sr;
    clear();
    return;
  }

  assert(newK>0);
  for(int i = 0; i < newK; ++i) {
    (*sr)[i] = sqrt((*sr)[i]);
  }
  ur->cols = newK;
  vr->cols = newK;
  /* Scaling of ur and vr */
  ur->multiplyWithDiag(sr);
  vr->multiplyWithDiag(sr);

  delete sr;

  /* Multiplication by orthogonal matrix Q: no or/un-mqr as
    this comes from Gram-Schmidt procedure not Householder
  */
  ScalarArray<T> *newA = new ScalarArray<T>(a->rows, newK);
  newA->gemm('N', 'N', Constants<T>::pone, a, ur, Constants<T>::zero);

  ScalarArray<T> *newB = new ScalarArray<T>(b->rows, newK);
  newB->gemm('N', 'N', Constants<T>::pone, b, vr, Constants<T>::zero);

  newA->setOrtho(ur->getOrtho());
  newB->setOrtho(vr->getOrtho());
  delete ur;
  delete vr;

  delete a;
  a = newA;
  delete b;
  b = newB;
}

// Swap members with members from another instance.
template<typename T> void RkMatrix<T>::swap(RkMatrix<T>& other)
{
  assert(rows == other.rows);
  assert(cols == other.cols);
  std::swap(a, other.a);
  std::swap(b, other.b);
  std::swap(method, other.method);
}

template<typename T> void RkMatrix<T>::axpy(T alpha, const FullMatrix<T>* mat) {
  RkMatrix<T>* tmp = formattedAddParts(&alpha, &mat, 1);
  swap(*tmp);
  delete tmp;
}

template<typename T> void RkMatrix<T>::axpy(T alpha, const RkMatrix<T>* mat) {
  RkMatrix<T>* tmp = formattedAddParts(&alpha, &mat, 1);
  swap(*tmp);
  delete tmp;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::formattedAddParts(const T* alpha, const RkMatrix<T>* const * parts,
                                            const int n, const bool dotruncate) const {
  // TODO check if formattedAddParts() actually uses sometimes this 'alpha' parameter (or is it always 1 ?)
  DECLARE_CONTEXT;

  /* List of non-null and non-empty Rk matrices to coalesce, and the corresponding scaling coefficients */
  const RkMatrix<T>* usedParts[n+1];
  T usedAlpha[n+1];
  /* Number of elements in usedParts[] */
  int notNullParts = 0;
  /* Sum of the ranks */
  int rankTotal = 0;

  // If needed, put 'this' in first position in usedParts[]
  if (rank()) {
    usedAlpha[0] = Constants<T>::pone ;
    usedParts[notNullParts++] = this ;
    rankTotal += rank();
  }

  CompressionMethod minMethod = method;
  for (int i = 0; i < n; i++) {
    // exclude the NULL and 0-rank matrices
    if (!parts[i] || parts[i]->rank() == 0 || parts[i]->rows->size() == 0 || parts[i]->cols->size() == 0 || alpha[i]==Constants<T>::zero)
      continue;
    // Check that partial RkMatrix indices are subsets of their global indices set.
    assert(parts[i]->rows->isSubset(*rows));
    assert(parts[i]->cols->isSubset(*cols));
    // Add this Rk to the list
    rankTotal += parts[i]->rank();
    minMethod = std::min(minMethod, parts[i]->method);
    usedAlpha[notNullParts] = alpha[i] ;
    usedParts[notNullParts] = parts[i] ;
    notNullParts++;
  }

  if(notNullParts == 0)
    return new RkMatrix<T>(NULL, rows, NULL, cols, minMethod);

  // In case the sum of the ranks of the sub-matrices is greater than
  // the matrix size, it is more efficient to put everything in a
  // full matrix.
  if (rankTotal >= std::min(rows->size(), cols->size())) {
    const FullMatrix<T>** fullParts = new const FullMatrix<T>*[notNullParts];
    fullParts[0] = NULL ;
    for (int i = rank() ? 1 : 0 ; i < notNullParts; i++) // exclude usedParts[0] if it is 'this'
      fullParts[i] = usedParts[i]->eval();
    RkMatrix<T>* result = formattedAddParts(usedAlpha, fullParts, notNullParts);
    for (int i = 0; i < notNullParts; i++)
      delete fullParts[i];
    delete[] fullParts;
    return result;
  }

  // Find if the QR factorisation can be accelerated using orthogonality information
  int initialPivotA = usedParts[0]->a->getOrtho() ? usedParts[0]->rank() : 0;
  int initialPivotB = usedParts[0]->b->getOrtho() ? usedParts[0]->rank() : 0;

  // Try to optimize the order of the Rk matrix to maximize initialPivot
  static char *useBestRk = getenv("HMAT_MGS_BESTRK");
  if (useBestRk){
    // 1st optim: Put in first position the Rk matrix with orthogonal panels AND maximum rank
    int bestRk=-1, bestGain=-1;
    for (int i=0 ; i<notNullParts ; i++) {
      // Roughly, the gain from an initial pivot 'p' in a QR factorisation 'm x n' is to reduce the flops
      // from 2mn^2 to 2m(n^2-p^2), so the gain grows like p^2 for each panel
      // hence the gain formula : number of orthogonal panels x rank^2
      int gain = (usedParts[i]->a->getOrtho() + usedParts[i]->b->getOrtho())*usedParts[i]->rank()*usedParts[i]->rank();
      if (gain > bestGain) {
        bestGain = gain;
        bestRk = i;
      }
    }
    if (bestRk > 0) {
      std::swap(usedParts[0], usedParts[bestRk]) ;
      std::swap(usedAlpha[0], usedAlpha[bestRk]) ;
    }
    initialPivotA = usedParts[0]->a->getOrtho() ? usedParts[0]->rank() : 0;
    initialPivotB = usedParts[0]->b->getOrtho() ? usedParts[0]->rank() : 0;


    // 2nd optim:
    // When coallescing Rk from childs toward parent, it is possible to "merge" Rk from (extra-)diagonal
    // childs because with non-intersecting rows and cols we will extend orthogonality between separate Rk.
    int best_i1=-1, best_i2=-1, best_rkA=-1, best_rkB=-1;
    for (int i1=0 ; i1<notNullParts ; i1++)
      for (int i2=0 ; i2<notNullParts ; i2++)
        if (i1 != i2) {
          const RkMatrix<T>* Rk1 = usedParts[i1];
          const RkMatrix<T>* Rk2 = usedParts[i2];
          // compute the gain expected from puting Rk1-Rk2 in first position
          // Orthogonality of Rk2->a is useful only if Rk1->a is ortho AND rows dont intersect (cols for panel b)
          int rkA = Rk1->a->getOrtho() ? Rk1->rank() + (Rk2->a->getOrtho() && !Rk1->rows->intersects(*Rk2->rows) ? Rk2->rank() : 0) : 0;
          int rkB = Rk1->b->getOrtho() ? Rk1->rank() + (Rk2->b->getOrtho() && !Rk1->cols->intersects(*Rk2->cols) ? Rk2->rank() : 0) : 0;
          int gain = rkA*rkA + rkB*rkB ;
          if (gain > bestGain) {
            bestGain = gain;
            best_i1 = i1;
            best_i2 = i2;
            best_rkA = rkA;
            best_rkB = rkB;
          }
        }

    if (best_i1 >= 0) {
      // put i1 in first position, i2 in second
      std::swap(usedParts[0], usedParts[best_i1]) ;
      std::swap(usedAlpha[0], usedAlpha[best_i1]) ;
      if (best_i2==0) best_i2 = best_i1; // handles the case where best_i2 was usedParts[0] which has just been moved
      std::swap(usedParts[1], usedParts[best_i2]) ;
      std::swap(usedAlpha[1], usedAlpha[best_i2]) ;
      initialPivotA = best_rkA;
      initialPivotB = best_rkB;
    }

  }

  ScalarArray<T>* resultA = new ScalarArray<T>(rows->size(), rankTotal);
  ScalarArray<T>* resultB = new ScalarArray<T>(cols->size(), rankTotal);
  // According to the indices organization, the sub-matrices are
  // contiguous blocks in the "big" matrix whose columns offset is
  //      kOffset = usedParts[0]->k + ... + usedParts[i-1]->k
  // rows offset is
  //      usedParts[i]->rows->offset - rows->offset
  // rows size
  //      usedParts[i]->rows->size x usedParts[i]->k (rows x columns)
  // Same for columns.
  int rankOffset = 0;
  for (int i = 0; i < notNullParts; i++) {

    // Copy 'a' at position rowOffset, kOffset
    int rowOffset = usedParts[i]->rows->offset() - rows->offset();
    resultA->copyMatrixAtOffset(usedParts[i]->a, rowOffset, rankOffset);

    // Scaling the matrix already in place inside resultA
    if (usedAlpha[i] != Constants<T>::pone) {
      ScalarArray<T> tmp(*resultA, rowOffset, usedParts[i]->a->rows, rankOffset, usedParts[i]->a->cols);
      tmp.scale(usedAlpha[i]);
    }

    // Copy 'b' at position colOffset, kOffset
    int colOffset = usedParts[i]->cols->offset() - cols->offset();
    resultB->copyMatrixAtOffset(usedParts[i]->b, colOffset, rankOffset);

    // Update the rank offset
    rankOffset += usedParts[i]->rank();
  }
  assert(rankOffset==rankTotal);
  RkMatrix<T>* rk = new RkMatrix<T>(resultA, rows, resultB, cols, minMethod);
  // If only one of the parts is non-zero, then the recompression is not necessary
  if (notNullParts > 1 && dotruncate)
    rk->truncate(approx.recompressionEpsilon, initialPivotA, initialPivotB);

  return rk;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::formattedAddParts(const T* alpha, const FullMatrix<T>* const * parts, int n) const {
  DECLARE_CONTEXT;
  FullMatrix<T>* me = eval();
  HMAT_ASSERT(me);

  // TODO: here, we convert Rk->Full, Update the Full with parts[], and Full->Rk. We could also
  // create a new empty Full, update, convert to Rk and add it to 'this'.
  // If the parts[] are smaller than 'this', convert them to Rk and add them could be less expensive
  for (int i = 0; i < n; i++) {
    if (!parts[i])
      continue;
    const IndexSet *rows_full = parts[i]->rows_;
    const IndexSet *cols_full = parts[i]->cols_;
    assert(rows_full->isSubset(*rows));
    assert(cols_full->isSubset(*cols));
    int rowOffset = rows_full->offset() - rows->offset();
    int colOffset = cols_full->offset() - cols->offset();
    int maxCol = cols_full->size();
    int maxRow = rows_full->size();
    ScalarArray<T> sub(me->data, rowOffset, maxRow, colOffset, maxCol);
    sub.axpy(alpha[i], &parts[i]->data);
  }
  RkMatrix<T>* result = truncatedSvd(me, RkMatrix<T>::approx.recompressionEpsilon); // TODO compress with something else than SVD
  delete me;
  return result;
}


template<typename T> RkMatrix<T>* RkMatrix<T>::multiplyRkFull(char transR, char transM,
                                                              const RkMatrix<T>* rk,
                                                              const FullMatrix<T>* m) {
  DECLARE_CONTEXT;

  assert(((transR == 'N') ? rk->cols->size() : rk->rows->size()) == ((transM == 'N') ? m->rows() : m->cols()));
  const IndexSet *rkRows = ((transR == 'N')? rk->rows : rk->cols);
  const IndexSet *mCols = ((transM == 'N')? m->cols_ : m->rows_);

  if(rk->rank() == 0) {
      return new RkMatrix<T>(NULL, rkRows, NULL, mCols, NoCompression);
  }
  // If transM is 'N' and transR is 'N', we compute
  //  A * B^T * M ==> newA = A, newB = M^T * B
  // We can deduce all other cases from this one:
  //   * if transR is 'T', all we have to do is to swap A and B
  //   * if transR is 'C', we swap A and B, but they must also
  //     be conjugate; let us look at the different cases:
  //     + if transM is 'N', newB = M^T * conj(B) = conj(M^H * B)
  //     + if transM is 'T', newB = M * conj(B)
  //     + if transM is 'C', newB = conj(M) * conj(B) = conj(M * B)

  ScalarArray<T> *newA, *newB;
  ScalarArray<T>* a = rk->a;
  ScalarArray<T>* b = rk->b;
  if (transR != 'N') {
    // if transR == 'T', we permute a and b; if transR == 'C', they will
    // also have to be conjugated, but this cannot be done here because rk
    // is const, this will be performed below.
    std::swap(a, b);
  }
  newA = a->copy();
  newB = new ScalarArray<T>(transM == 'N' ? m->cols() : m->rows(), b->cols);
  if (transR == 'C') {
    newA->conjugate();
    if (transM == 'N') {
      newB->gemm('C', 'N', Constants<T>::pone, &m->data, b, Constants<T>::zero);
      newB->conjugate();
    } else if (transM == 'T') {
      ScalarArray<T> *conjB = b->copy();
      conjB->conjugate();
      newB->gemm('N', 'N', Constants<T>::pone, &m->data, conjB, Constants<T>::zero);
      delete conjB;
    } else {
      assert(transM == 'C');
      newB->gemm('N', 'N', Constants<T>::pone, &m->data, b, Constants<T>::zero);
      newB->conjugate();
    }
  } else {
    if (transM == 'N') {
      newB->gemm('T', 'N', Constants<T>::pone, &m->data, b, Constants<T>::zero);
    } else if (transM == 'T') {
      newB->gemm('N', 'N', Constants<T>::pone, &m->data, b, Constants<T>::zero);
    } else {
      assert(transM == 'C');
      ScalarArray<T> *conjB = b->copy();
      conjB->conjugate();
      newB->gemm('N', 'N', Constants<T>::pone, &m->data, conjB, Constants<T>::zero);
      newB->conjugate();
      delete conjB;
    }
  }
  RkMatrix<T>* result = new RkMatrix<T>(newA, rkRows, newB, mCols, rk->method);
  return result;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::multiplyFullRk(char transM, char transR,
                                         const FullMatrix<T>* m,
                                         const RkMatrix<T>* rk) {
  DECLARE_CONTEXT;
  // If transM is 'N' and transR is 'N', we compute
  //  M * A * B^T  ==> newA = M * A, newB = B
  // We can deduce all other cases from this one:
  //   * if transR is 'T', all we have to do is to swap A and B
  //   * if transR is 'C', we swap A and B, but they must also
  //     be conjugate; let us look at the different cases:
  //     + if transM is 'N', newA = M * conj(A)
  //     + if transM is 'T', newA = M^T * conj(A) = conj(M^H * A)
  //     + if transM is 'C', newA = M^H * conj(A) = conj(M^T * A)
  ScalarArray<T> *newA, *newB;
  ScalarArray<T>* a = rk->a;
  ScalarArray<T>* b = rk->b;
  if (transR != 'N') { // permutation to transpose the matrix Rk
    std::swap(a, b);
  }
  const IndexSet *rkCols = ((transR == 'N')? rk->cols : rk->rows);
  const IndexSet *mRows = ((transM == 'N')? m->rows_ : m->cols_);

  newA = new ScalarArray<T>(mRows->size(), b->cols);
  newB = b->copy();
  if (transR == 'C') {
    newB->conjugate();
    if (transM == 'N') {
      ScalarArray<T> *conjA = a->copy();
      conjA->conjugate();
      newA->gemm('N', 'N', Constants<T>::pone, &m->data, conjA, Constants<T>::zero);
      delete conjA;
    } else if (transM == 'T') {
      newA->gemm('C', 'N', Constants<T>::pone, &m->data, a, Constants<T>::zero);
      newA->conjugate();
    } else {
      assert(transM == 'C');
      newA->gemm('T', 'N', Constants<T>::pone, &m->data, a, Constants<T>::zero);
      newA->conjugate();
    }
  } else {
    newA->gemm(transM, 'N', Constants<T>::pone, &m->data, a, Constants<T>::zero);
  }
  RkMatrix<T>* result = new RkMatrix<T>(newA, mRows, newB, rkCols, rk->method);
  return result;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::multiplyRkH(char transR, char transH,
                                      const RkMatrix<T>* rk, const HMatrix<T>* h) {
  DECLARE_CONTEXT;
  assert(((transR == 'N') ? *rk->cols : *rk->rows) == ((transH == 'N')? *h->rows() : *h->cols()));

  const IndexSet* rkRows = ((transR == 'N')? rk->rows : rk->cols);

  // If transR == 'N'
  //    transM == 'N': (A*B^T)*M = A*(M^T*B)^T
  //    transM == 'T': (A*B^T)*M^T = A*(M*B)^T
  //    transM == 'C': (A*B^T)*M^H = A*(conj(M)*B)^T = A*conj(M*conj(B))^T
  // If transR == 'T', we only have to swap A and B
  // If transR == 'C', we swap A and B, then
  //    transM == 'N': R^H*M = conj(A)*(M^T*conj(B))^T = conj(A)*conj(M^H*B)^T
  //    transM == 'T': R^H*M^T = conj(A)*(M*conj(B))^T
  //    transM == 'C': R^H*M^H = conj(A)*conj(M*B)^T
  //
  // Size of the HMatrix is n x m,
  // So H^t size is m x n and the product is m x cols(B)
  // and the number of columns of B is k.
  ScalarArray<T> *newA, *newB;
  ScalarArray<T>* a = rk->a;
  ScalarArray<T>* b = rk->b;
  if (transR != 'N') { // permutation to transpose the matrix Rk
    std::swap(a, b);
  }

  const IndexSet *newCols = ((transH == 'N' )? h->cols() : h->rows());

  newA = a->copy();
  newB = new ScalarArray<T>(transH == 'N' ? h->cols()->size() : h->rows()->size(), b->cols);
  if (transR == 'C') {
    newA->conjugate();
    if (transH == 'N') {
      h->gemv('C', Constants<T>::pone, b, Constants<T>::zero, newB);
      newB->conjugate();
    } else if (transH == 'T') {
      ScalarArray<T> *conjB = b->copy();
      conjB->conjugate();
      h->gemv('N', Constants<T>::pone, conjB, Constants<T>::zero, newB);
      delete conjB;
    } else {
      assert(transH == 'C');
      h->gemv('N', Constants<T>::pone, b, Constants<T>::zero, newB);
      newB->conjugate();
    }
  } else {
    if (transH == 'N') {
      h->gemv('T', Constants<T>::pone, b, Constants<T>::zero, newB);
    } else if (transH == 'T') {
      h->gemv('N', Constants<T>::pone, b, Constants<T>::zero, newB);
    } else {
      assert(transH == 'C');
      ScalarArray<T> *conjB = b->copy();
      conjB->conjugate();
      h->gemv('N', Constants<T>::pone, conjB, Constants<T>::zero, newB);
      delete conjB;
      newB->conjugate();
    }
  }
  RkMatrix<T>* result = new RkMatrix<T>(newA, rkRows, newB, newCols, rk->method);
  return result;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::multiplyHRk(char transH, char transR,
                                      const HMatrix<T>* h, const RkMatrix<T>* rk) {

  DECLARE_CONTEXT;
  if (rk->rank() == 0) {
    const IndexSet* newRows = ((transH == 'N') ? h-> rows() : h->cols());
    const IndexSet* newCols = ((transR == 'N') ? rk->cols : rk->rows);
    return new RkMatrix<T>(NULL, newRows, NULL, newCols, rk->method);
  }

  // If transH is 'N' and transR is 'N', we compute
  //  M * A * B^T  ==> newA = M * A, newB = B
  // We can deduce all other cases from this one:
  //   * if transR is 'T', all we have to do is to swap A and B
  //   * if transR is 'C', we swap A and B, but they must also
  //     be conjugate; let us look at the different cases:
  //     + if transH is 'N', newA = M * conj(A)
  //     + if transH is 'T', newA = M^T * conj(A) = conj(M^H * A)
  //     + if transH is 'C', newA = M^H * conj(A) = conj(M^T * A)
  ScalarArray<T> *newA, *newB;
  ScalarArray<T>* a = rk->a;
  ScalarArray<T>* b = rk->b;
  if (transR != 'N') { // permutation to transpose the matrix Rk
    std::swap(a, b);
  }
  const IndexSet *rkCols = ((transR == 'N')? rk->cols : rk->rows);
  const IndexSet* newRows = ((transH == 'N')? h-> rows() : h->cols());

  newA = new ScalarArray<T>(transH == 'N' ? h->rows()->size() : h->cols()->size(), b->cols);
  newB = b->copy();
  if (transR == 'C') {
    newB->conjugate();
    if (transH == 'N') {
      ScalarArray<T> *conjA = a->copy();
      conjA->conjugate();
      h->gemv('N', Constants<T>::pone, conjA, Constants<T>::zero, newA);
      delete conjA;
    } else if (transH == 'T') {
      h->gemv('C', Constants<T>::pone, a, Constants<T>::zero, newA);
      newA->conjugate();
    } else {
      assert(transH == 'C');
      h->gemv('T', Constants<T>::pone, a, Constants<T>::zero, newA);
      newA->conjugate();
    }
  } else {
    h->gemv(transH, Constants<T>::pone, a, Constants<T>::zero, newA);
  }
  RkMatrix<T>* result = new RkMatrix<T>(newA, newRows, newB, rkCols, rk->method);
  return result;
}

template<typename T>
RkMatrix<T>* RkMatrix<T>::multiplyRkRk(char trans1, char trans2,
                                       const RkMatrix<T>* r1, const RkMatrix<T>* r2) {
  DECLARE_CONTEXT;
  assert(((trans1 == 'N') ? *r1->cols : *r1->rows) == ((trans2 == 'N') ? *r2->rows : *r2->cols));
  // It is possible to do the computation differently, yielding a
  // different rank and a different amount of computation.
  // TODO: choose the best order.
  ScalarArray<T>* a1 = (trans1 == 'N' ? r1->a : r1->b);
  ScalarArray<T>* b1 = (trans1 == 'N' ? r1->b : r1->a);
  ScalarArray<T>* a2 = (trans2 == 'N' ? r2->a : r2->b);
  ScalarArray<T>* b2 = (trans2 == 'N' ? r2->b : r2->a);

  assert(b1->rows == a2->rows); // compatibility of the multiplication

  // We want to compute the matrix a1.t^b1.a2.t^b2 and return an Rk matrix
  // Usually, the best way is to start with tmp=t^b1.a2 which produces a 'small' matrix rank1 x rank2
  //
  // OLD version (default):
  // Then we can either :
  // - compute a1.tmp : the cost is rank1.rank2.row_a, the resulting Rk has rank rank2
  // - compute tmp.t^b2 : the cost is rank1.rank2.col_b, the resulting Rk has rank rank1
  // We use the solution which gives the lowest resulting rank.
  // With this version, orthogonality is lost on one panel, it is preserved on the other.
  //
  // NEW version :
  // Other solution: once we have the small matrix tmp=t^b1.a2, we can do a recompression on it for low cost
  // using SVD + truncation. This also removes the choice above, since tmp=U.S.V is then applied on both sides
  // This version isn't default, it can be activated by setting env. var. HMAT_NEW_RKRK
  // With this version, orthogonality is lost on both panel.

  ScalarArray<T>* tmp = new ScalarArray<T>(r1->rank(), r2->rank());
  if (trans1 == 'C' && trans2 == 'C') {
    tmp->gemm('T', 'N', Constants<T>::pone, b1, a2, Constants<T>::zero);
    tmp->conjugate();
  } else if (trans1 == 'C') {
    tmp->gemm('C', 'N', Constants<T>::pone, b1, a2, Constants<T>::zero);
  } else if (trans2 == 'C') {
    tmp->gemm('C', 'N', Constants<T>::pone, b1, a2, Constants<T>::zero);
    tmp->conjugate();
  } else {
    tmp->gemm('T', 'N', Constants<T>::pone, b1, a2, Constants<T>::zero);
  }

  ScalarArray<T> *newA=NULL, *newB=NULL;
  static char *newRKRK = getenv("HMAT_NEW_RKRK"); // Option to use the NEW version, with SVD-in-the-middle
  if (newRKRK) {
    // NEW version
    ScalarArray<T>* ur = NULL;
    Vector<double>* sr = NULL;
    ScalarArray<T>* vr = NULL;
    int newK;
    // SVD tmp = ur.sr.t^vr
    tmp->svdDecomposition(&ur, &sr, &vr);
    // Remove small singular values and compute square root of sr
    newK = approx.findK(*sr, RkMatrix<T>::approx.recompressionEpsilon);
    //    printf("oldK1=%d oldK2=%d newK=%d\n", r1->rank(), r2->rank(), newK);
    if (newK > 0) {
      for(int i = 0; i < newK; ++i)
        (*sr)[i] = sqrt((*sr)[i]);
      ur->cols = newK;
      vr->cols = newK;
      /* Scaling of ur and vr */
      ur->multiplyWithDiag(sr);
      vr->multiplyWithDiag(sr);
      /* Now compute newA = a1.ur and newB = b2.vr */
      newA = new ScalarArray<T>(a1->rows, newK);
      if (trans1 == 'C') ur->conjugate();
      newA->gemm('N', 'N', Constants<T>::pone, a1, ur, Constants<T>::zero);
      if (trans1 == 'C') newA->conjugate();
      newB = new ScalarArray<T>(b2->rows, newK);
      if (trans2 == 'C') vr->conjugate();
      newB->gemm('N', 'N', Constants<T>::pone, b2, vr, Constants<T>::zero);
      if (trans2 == 'C') newB->conjugate();
    }
    delete ur;
    delete vr;
    delete sr;
  } else {
    // OLD version
    if (r1->rank() < r2->rank()) {
      // newA = a1, newB = b2.t^tmp
      newA = a1->copy();
      if (trans1 == 'C') newA->conjugate();
      newB = new ScalarArray<T>(b2->rows, r1->rank());
      if (trans2 == 'C') {
        newB->gemm('N', 'C', Constants<T>::pone, b2, tmp, Constants<T>::zero);
        newB->conjugate();
      } else {
        newB->gemm('N', 'T', Constants<T>::pone, b2, tmp, Constants<T>::zero);
      }
    } else { // newA = a1.tmp, newB = b2
      newA = new ScalarArray<T>(a1->rows, r2->rank());
      if (trans1 == 'C') tmp->conjugate(); // be careful if you re-use tmp after this...
      newA->gemm('N', 'N', Constants<T>::pone, a1, tmp, Constants<T>::zero);
      if (trans1 == 'C') newA->conjugate();
      newB = b2->copy();
      if (trans2 == 'C') newB->conjugate();
    }
  }
  delete tmp;

  CompressionMethod combined = std::min(r1->method, r2->method);
  return new RkMatrix<T>(newA, ((trans1 == 'N') ? r1->rows : r1->cols), newB, ((trans2 == 'N') ? r2->cols : r2->rows), combined);
}

template<typename T>
size_t RkMatrix<T>::computeRkRkMemorySize(char trans1, char trans2,
                                                const RkMatrix<T>* r1, const RkMatrix<T>* r2)
{
    ScalarArray<T>* b2 = (trans2 == 'N' ? r2->b : r2->a);
    ScalarArray<T>* a1 = (trans1 == 'N' ? r1->a : r1->b);
    return b2 == NULL ? 0 : b2->memorySize() +
           a1 == NULL ? 0 : a1->rows * r2->rank() * sizeof(T);
}

template<typename T>
void RkMatrix<T>::multiplyWithDiagOrDiagInv(const HMatrix<T> * d, bool inverse, bool left) {
  assert(*d->rows() == *d->cols());
  assert(!left || (*rows == *d->cols()));
  assert(left  || (*cols == *d->rows()));

  // extracting the diagonal
  Vector<T>* diag = new Vector<T>(d->cols()->size());
  d->extractDiagonal(diag->ptr());

  // left multiplication by d of b (if M<-M*D : left = false) or a (if M<-D*M : left = true)
  ScalarArray<T>* aOrB = (left ? a : b);
  aOrB->multiplyWithDiagOrDiagInv(diag, inverse, true);

  delete diag;
}

template<typename T> void RkMatrix<T>::gemmRk(char transHA, char transHB,
                                              T alpha, const HMatrix<T>* ha, const HMatrix<T>* hb, T beta) {
  DECLARE_CONTEXT;
  // TODO: remove this limitation, if needed.
  assert(beta == Constants<T>::pone);

  // This is ugly!  When ha node is void, we replace ha by its non-void child
  // so that further computations are similar to the non-void case.
  // Indeed, this is ugly...
  while (!ha->isLeaf())
  {
    if (ha->nrChildRow() >= 2 && ha->nrChildCol() >= 2) {
      if (ha->get(0, 0)->rows()->size() == 0 && ha->get(0, 0)->cols()->size() == 0)
      {
        ha = ha->get(1, 1);
        continue;
      }
      if (ha->get(1, 1) && ha->get(1, 1)->rows()->size() == 0 && ha->get(1, 1)->cols()->size() == 0)
      {
        ha = ha->get(0, 0);
        continue;
      }
    }
    break;
  }
  while (!hb->isLeaf())
  {
    if (hb->nrChildRow() >= 2 && hb->nrChildCol() >= 2) {
      if (hb->get(0, 0)->rows()->size() == 0 && hb->get(0, 0)->cols()->size() == 0)
      {
        hb = hb->get(1, 1);
        continue;
      }
      if (hb->get(1, 1) && hb->get(1, 1)->rows()->size() == 0 && hb->get(1, 1)->cols()->size() == 0)
      {
        hb = hb->get(0, 0);
        continue;
      }
    }
    break;
  }
  // void matrix
  if (ha->rows()->size() == 0 || ha->cols()->size() == 0 || hb->rows()->size() == 0 || hb->cols()->size() == 0) return;

  if (!(ha->isLeaf() || hb->isLeaf())) {
    // Recursion case
    int nbRows = transHA == 'N' ? ha->nrChildRow() : ha->nrChildCol() ; /* Row blocks of the product */
    int nbCols = transHB == 'N' ? hb->nrChildCol() : hb->nrChildRow() ; /* Col blocks of the product */
    int nbCom  = transHA == 'N' ? ha->nrChildCol() : ha->nrChildRow() ; /* Common dimension between A and B */
    RkMatrix<T>* subRks[nbRows * nbCols];
    for (int i = 0; i < nbRows; i++) {
      for (int j = 0; j < nbCols; j++) {
        subRks[i + j * nbRows]=(RkMatrix<T>*)NULL;
        for (int k = 0; k < nbCom; k++) {
          // C_ij = A_ik * B_kj
          const HMatrix<T>* a_ik = (transHA == 'N' ? ha->get(i, k) : ha->get(k, i));
          const HMatrix<T>* b_kj = (transHB == 'N' ? hb->get(k, j) : hb->get(j, k));
          if (a_ik && b_kj) {
            if (subRks[i + j * nbRows]==NULL) {
              const IndexSet* subRows = (transHA == 'N' ? a_ik->rows() : a_ik->cols());
              const IndexSet* subCols = (transHB == 'N' ? b_kj->cols() : b_kj->rows());
              subRks[i + j * nbRows] = new RkMatrix<T>(NULL, subRows, NULL, subCols, NoCompression);
            }
            subRks[i + j * nbRows]->gemmRk(transHA, transHB, alpha, a_ik, b_kj, beta);
          }
        } // k loop
      } // j loop
    } // i loop
    // Reconstruction of C by adding the parts
    std::vector<T> alphaV(nbRows * nbCols, Constants<T>::pone);
    RkMatrix<T>* rk = formattedAddParts(&alphaV[0], (const RkMatrix<T>**) subRks, nbRows * nbCols);
    swap(*rk);
    for (int i = 0; i < nbRows * nbCols; i++) {
      delete subRks[i];
    }
    delete rk;
  } else {
    RkMatrix<T>* rk = NULL;
    // One of the product matrix is a leaf
    if ((ha->isLeaf() && ha->isNull()) || (hb->isLeaf() && hb->isNull())) {
      // Nothing to do
    } else if (ha->isRkMatrix() || hb->isRkMatrix()) {
      rk = HMatrix<T>::multiplyRkMatrix(transHA, transHB, ha, hb);
    } else {
      assert(ha->isFullMatrix() || hb->isFullMatrix());
      FullMatrix<T>* fullMat = HMatrix<T>::multiplyFullMatrix(transHA, transHB, ha, hb);
      if(fullMat) {
        rk = truncatedSvd(fullMat, RkMatrix<T>::approx.recompressionEpsilon); // TODO compress with something else than SVD
        delete fullMat;
      }
    }
    if(rk) {
      axpy(alpha, rk);
      delete rk;
    }
  }
}

template<typename T> void RkMatrix<T>::copy(const RkMatrix<T>* o) {
  delete a;
  delete b;
  rows = o->rows;
  cols = o->cols;
  a = (o->a ? o->a->copy() : NULL);
  b = (o->b ? o->b->copy() : NULL);
}

template<typename T> RkMatrix<T>* RkMatrix<T>::copy() const {
  RkMatrix<T> *result = new RkMatrix<T>(NULL, rows, NULL, cols, this->method);
  result->copy(this);
  return result;
}


template<typename T> void RkMatrix<T>::checkNan() const {
  if (rank() == 0) {
    return;
  }
  a->checkNan();
  b->checkNan();
}

template<typename T> void RkMatrix<T>::conjugate() {
  if (a) a->conjugate();
  if (b) b->conjugate();
}

template<typename T> T RkMatrix<T>::get(int i, int j) const {
  return a->dot_aibj(i, *b, j);
}

template<typename T> void RkMatrix<T>::writeArray(hmat_iostream writeFunc, void * userData) const{
  a->writeArray(writeFunc, userData);
  b->writeArray(writeFunc, userData);
}

// Templates declaration
template class RkMatrix<S_t>;
template class RkMatrix<D_t>;
template class RkMatrix<C_t>;
template class RkMatrix<Z_t>;

}  // end namespace hmat
