/* ************************************************************************
 * Copyright (C) 2023 Advanced Micro Devices, Inc. All rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#ifndef ROCALUTION_HIP_HIP_KERNELS_RSAMG_CSR_HPP_
#define ROCALUTION_HIP_HIP_KERNELS_RSAMG_CSR_HPP_

#include "hip_atomics.hpp"
#include "hip_unordered_map.hpp"
#include "hip_unordered_set.hpp"
#include "hip_utils.hpp"

#include <hip/hip_runtime.h>

namespace rocalution
{
    // Determine strong influences
    template <unsigned int BLOCKSIZE, unsigned int WFSIZE, typename T, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_pmis_strong_influences(I nrow,
                                                  const J* __restrict__ csr_row_ptr,
                                                  const I* __restrict__ csr_col_ind,
                                                  const T* __restrict__ csr_val,
                                                  float eps,
                                                  float* __restrict__ omega,
                                                  bool* __restrict__ S)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        // The row this wavefront operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Determine minimum and maximum off-diagonal of the current row
        T min_a_ik = static_cast<T>(0);
        T max_a_ik = static_cast<T>(0);

        // Shared boolean that holds diagonal sign for each wavefront
        // where true means, the diagonal element is negative
        __shared__ bool sign[BLOCKSIZE / WFSIZE];

        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Determine diagonal sign and min/max
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            I col = csr_col_ind[j];
            T val = csr_val[j];

            if(col == row)
            {
                // Get diagonal entry sign
                sign[wid] = val < static_cast<T>(0);
            }
            else
            {
                // Get min / max entries
                min_a_ik = (min_a_ik < val) ? min_a_ik : val;
                max_a_ik = (max_a_ik > val) ? max_a_ik : val;
            }
        }

        __threadfence_block();

        // Maximum or minimum, depending on the diagonal sign
        T cond = sign[wid] ? max_a_ik : min_a_ik;

        // Obtain extrema on all threads of the wavefront
        if(sign[wid])
        {
            wf_reduce_max<WFSIZE>(&cond);
        }
        else
        {
            wf_reduce_min<WFSIZE>(&cond);
        }

        // Threshold to check for strength of connection
        cond *= eps;

        // Fill S
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            I col = csr_col_ind[j];
            T val = csr_val[j];

            if(col != row && val < cond)
            {
                // col is strongly connected to row
                S[j] = true;

                // Increment omega, as it holds all strongly connected edges
                // of vertex col.
                // Additionally, omega holds a random number between 0 and 1 to
                // distinguish neighbor points with equal number of strong
                // connections.
                atomicAdd(&omega[col], 1.0f);
            }
        }
    }

    // Mark all vertices that have not been assigned yet, as coarse
    template <typename I>
    __global__ void kernel_csr_rs_pmis_unassigned_to_coarse(I nrow,
                                                            const float* __restrict__ omega,
                                                            int* __restrict__ cf,
                                                            bool* __restrict__ workspace)
    {
        // Each thread processes a row
        I row = blockIdx.x * blockDim.x + threadIdx.x;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // workspace keeps track, whether a vertex has been marked coarse
        // during the current iteration, or not.
        bool flag = false;

        // Check only undecided vertices
        if(cf[row] == 0)
        {
            // If this vertex has an edge, it might be a coarse one
            if(omega[row] >= 1.0f)
            {
                cf[row] = 1;

                // Keep in mind, that this vertex has been marked coarse in the
                // current iteration
                flag = true;
            }
            else
            {
                // This point does not influence any other points and thus is a
                // fine point
                cf[row] = 2;
            }
        }

        workspace[row] = flag;
    }

    // Correct previously marked vertices with respect to omega
    template <unsigned int BLOCKSIZE, unsigned int WFSIZE, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_pmis_correct_coarse(I nrow,
                                               const J* __restrict__ csr_row_ptr,
                                               const I* __restrict__ csr_col_ind,
                                               const float* __restrict__ omega,
                                               const bool* __restrict__ S,
                                               int* __restrict__ cf,
                                               bool* __restrict__ workspace)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        // The row this wavefront operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // If this vertex has been marked coarse in the current iteration,
        // process it for further checks
        if(workspace[row])
        {
            J row_begin = csr_row_ptr[row];
            J row_end   = csr_row_ptr[row + 1];

            // Get the weight of the current row for comparison
            float omega_row = omega[row];

            // Loop over the full row to compare weights of other vertices that
            // have been marked coarse in the current iteration
            for(J j = row_begin + lid; j < row_end; j += WFSIZE)
            {
                // Process only vertices that are strongly connected
                if(S[j])
                {
                    I col = csr_col_ind[j];

                    // If this vertex has been marked coarse in the current iteration,
                    // we need to check whether it is accepted as a coarse vertex or not.
                    if(workspace[col])
                    {
                        // Get the weight of the current vertex for comparison
                        float omega_col = omega[col];

                        if(omega_row > omega_col)
                        {
                            // The diagonal entry has more edges and will remain
                            // a coarse point, whereas this vertex gets reverted
                            // back to undecided, for further processing.
                            cf[col] = 0;
                        }
                        else if(omega_row < omega_col)
                        {
                            // The diagonal entry has fewer edges and gets
                            // reverted back to undecided for further processing,
                            // whereas this vertex stays
                            // a coarse one.
                            cf[row] = 0;
                        }
                    }
                }
            }
        }
    }

    // Mark remaining edges of a coarse point to fine
    template <unsigned int BLOCKSIZE, unsigned int WFSIZE, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_pmis_coarse_edges_to_fine(I nrow,
                                                     const J* __restrict__ csr_row_ptr,
                                                     const I* __restrict__ csr_col_ind,
                                                     const bool* __restrict__ S,
                                                     int* __restrict__ cf)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        // The row this wavefront operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Process only undecided vertices
        if(cf[row] == 0)
        {
            J row_begin = csr_row_ptr[row];
            J row_end   = csr_row_ptr[row + 1];

            // Loop over all edges of this undecided vertex
            // and check, if there is a coarse point connected
            for(J j = row_begin + lid; j < row_end; j += WFSIZE)
            {
                // Check, whether this edge is strongly connected to the vertex
                if(S[j])
                {
                    I col = csr_col_ind[j];

                    // If this edge is coarse, our vertex must be fine
                    if(cf[col] == 1)
                    {
                        cf[row] = 2;
                        return;
                    }
                }
            }
        }
    }

    // Check for undecided vertices
    template <unsigned int BLOCKSIZE, typename I>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_pmis_check_undecided(I nrow,
                                                const int* __restrict__ cf,
                                                bool* __restrict__ undecided)
    {
        I row = blockIdx.x * blockDim.x + threadIdx.x;

        if(row >= nrow)
        {
            return;
        }

        // Check whether current vertex is undecided
        if(cf[row] == 0)
        {
            *undecided = true;
        }
    }

    template <unsigned int BLOCKSIZE, typename T, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_direct_interp_nnz(I nrow,
                                             const J* __restrict__ csr_row_ptr,
                                             const I* __restrict__ csr_col_ind,
                                             const T* __restrict__ csr_val,
                                             const bool* __restrict__ S,
                                             const int* __restrict__ cf,
                                             T* __restrict__ Amin,
                                             T* __restrict__ Amax,
                                             J* __restrict__ row_nnz,
                                             I* __restrict__ f2c)
    {
        // The row this thread operates on
        I row = blockIdx.x * blockDim.x + threadIdx.x;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Counter
        I nnz = 0;

        // Coarse points generate a single entry
        if(cf[row] == 1)
        {
            // Set coarse flag
            f2c[row] = nnz = 1;
        }
        else
        {
            // Set non-coarse flag
            f2c[row] = 0;

            T amin = static_cast<T>(0);
            T amax = static_cast<T>(0);

            J row_begin = csr_row_ptr[row];
            J row_end   = csr_row_ptr[row + 1];

            // Loop over the full row and determine minimum and maximum
            for(J j = row_begin; j < row_end; ++j)
            {
                // Process only vertices that are strongly connected
                if(S[j])
                {
                    I col = csr_col_ind[j];

                    // Process only coarse points
                    if(cf[col] == 1)
                    {
                        T val = csr_val[j];

                        amin = (amin < val) ? amin : val;
                        amax = (amax > val) ? amax : val;
                    }
                }
            }

            Amin[row] = amin = amin * static_cast<T>(0.2);
            Amax[row] = amax = amax * static_cast<T>(0.2);

            // Loop over the full row to count eligible entries
            for(J j = row_begin; j < row_end; ++j)
            {
                // Process only vertices that are strongly connected
                if(S[j] == true)
                {
                    I col = csr_col_ind[j];

                    // Process only coarse points
                    if(cf[col] == 1)
                    {
                        T val = csr_val[j];

                        // If conditions are fulfilled, count up row nnz
                        if(val <= amin || val >= amax)
                        {
                            ++nnz;
                        }
                    }
                }
            }
        }

        // Write row nnz back to global memory
        row_nnz[row] = nnz;
    }

    template <unsigned int BLOCKSIZE, typename T, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_direct_interp_fill(I nrow,
                                              const J* __restrict__ csr_row_ptr,
                                              const I* __restrict__ csr_col_ind,
                                              const T* __restrict__ csr_val,
                                              const J* __restrict__ prolong_csr_row_ptr,
                                              I* __restrict__ prolong_csr_col_ind,
                                              T* __restrict__ prolong_csr_val,
                                              const bool* __restrict__ S,
                                              const int* __restrict__ cf,
                                              const T* __restrict__ Amin,
                                              const T* __restrict__ Amax,
                                              const I* __restrict__ f2c)
    {
        // The row this thread operates on
        I row = blockIdx.x * blockDim.x + threadIdx.x;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // The row of P this thread operates on
        I row_P = prolong_csr_row_ptr[row];

        // If this is a coarse point, we can fill P and return
        if(cf[row] == 1)
        {
            prolong_csr_col_ind[row_P] = f2c[row];
            prolong_csr_val[row_P]     = static_cast<T>(1);

            return;
        }

        T diag  = static_cast<T>(0);
        T a_num = static_cast<T>(0), a_den = static_cast<T>(0);
        T b_num = static_cast<T>(0), b_den = static_cast<T>(0);
        T d_neg = static_cast<T>(0), d_pos = static_cast<T>(0);

        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Loop over the full row
        for(J j = row_begin; j < row_end; ++j)
        {
            I col = csr_col_ind[j];
            T val = csr_val[j];

            // Do not process the vertex itself
            if(col == row)
            {
                diag = val;
                continue;
            }

            if(val < static_cast<T>(0))
            {
                a_num += val;

                // Only process vertices that are strongly connected and coarse
                if(S[j] && cf[col] == 1)
                {
                    a_den += val;

                    if(val > Amin[row])
                    {
                        d_neg += val;
                    }
                }
            }
            else
            {
                b_num += val;

                // Only process vertices that are strongly connected and coarse
                if(S[j] && cf[col] == 1)
                {
                    b_den += val;

                    if(val < Amax[row])
                    {
                        d_pos += val;
                    }
                }
            }
        }

        T cf_neg = static_cast<T>(1);
        T cf_pos = static_cast<T>(1);

        if(abs(a_den - d_neg) > 1e-32)
        {
            cf_neg = a_den / (a_den - d_neg);
        }

        if(abs(b_den - d_pos) > 1e-32)
        {
            cf_pos = b_den / (b_den - d_pos);
        }

        if(b_num > static_cast<T>(0) && abs(b_den) < 1e-32)
        {
            diag += b_num;
        }

        T alpha = abs(a_den) > 1e-32 ? -cf_neg * a_num / (diag * a_den) : static_cast<T>(0);
        T beta  = abs(b_den) > 1e-32 ? -cf_pos * b_num / (diag * b_den) : static_cast<T>(0);

        // Loop over the full row to fill eligible entries
        for(J j = row_begin; j < row_end; ++j)
        {
            // Process only vertices that are strongly connected
            if(S[j] == true)
            {
                I col = csr_col_ind[j];
                T val = csr_val[j];

                // Process only coarse points
                if(cf[col] == 1)
                {
                    if(val > Amin[row] && val < Amax[row])
                    {
                        continue;
                    }

                    // Fill P
                    prolong_csr_col_ind[row_P] = f2c[col];
                    prolong_csr_val[row_P]     = (val < static_cast<T>(0) ? alpha : beta) * val;
                    ++row_P;
                }
            }
        }
    }

    template <unsigned int BLOCKSIZE, unsigned int WFSIZE, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_extpi_interp_max(I    nrow,
                                            bool FF1,
                                            const J* __restrict__ csr_row_ptr,
                                            const I* __restrict__ csr_col_ind,
                                            const bool* __restrict__ S,
                                            const int* __restrict__ cf,
                                            J* __restrict__ row_max)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        // The row this thread operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Some helpers for readability
        constexpr int COARSE = 1;

        // Coarse points generate a single entry
        if(cf[row] == COARSE)
        {
            if(lid == 0)
            {
                // Set row nnz to one
                row_max[row] = 1;
            }

            return;
        }

        // Counter
        I row_nnz = 0;

        // Row entry and exit points
        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Loop over all columns of the i-th row, whereas each lane processes a column
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            // Skip points that do not influence the current point
            if(S[j] == false)
            {
                continue;
            }

            // Get the column index
            I col_j = csr_col_ind[j];

            // Skip diagonal entries (i does not influence itself)
            if(col_j == row)
            {
                continue;
            }

            // Switch between coarse and fine points that influence the i-th point
            if(cf[col_j] == COARSE)
            {
                // This is a coarse point and thus contributes, count it
                ++row_nnz;
            }
            else
            {
                // This is a fine point, check for strongly connected coarse points

                // Row entry and exit of this fine point
                J row_begin_j = csr_row_ptr[col_j];
                J row_end_j   = csr_row_ptr[col_j + 1];

                // Loop over all columns of the fine point
                for(J k = row_begin_j; k < row_end_j; ++k)
                {
                    // Skip points that do not influence the fine point
                    if(S[k] == false)
                    {
                        continue;
                    }

                    // Get the column index
                    I col_k = csr_col_ind[k];

                    // Skip diagonal entries (the fine point does not influence itself)
                    if(col_k == col_j)
                    {
                        continue;
                    }

                    // Check whether k is a coarse point
                    if(cf[col_k] == COARSE)
                    {
                        // This is a coarse point, it contributes, count it
                        ++row_nnz;

                        // Stop if FF interpolation is limited
                        if(FF1 == true)
                        {
                            break;
                        }
                    }
                }
            }
        }

        // Sum up the row nnz from all lanes
        wf_reduce_sum<WFSIZE>(&row_nnz);

        if(lid == WFSIZE - 1)
        {
            // Write row nnz back to global memory
            row_max[row] = row_nnz;
        }
    }

    template <unsigned int BLOCKSIZE,
              unsigned int WFSIZE,
              unsigned int HASHSIZE,
              typename I,
              typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_extpi_interp_nnz(I    nrow,
                                            bool FF1,
                                            const J* __restrict__ csr_row_ptr,
                                            const I* __restrict__ csr_col_ind,
                                            const bool* __restrict__ S,
                                            const int* __restrict__ cf,
                                            J* __restrict__ row_nnz,
                                            I* __restrict__ state)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        if(WFSIZE == warpSize)
        {
            wid = __builtin_amdgcn_readfirstlane(wid);
        }

        // The row this thread operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Some helpers for readability
        constexpr int COARSE = 1;

        // Coarse points generate a single entry
        if(cf[row] == COARSE)
        {
            if(lid == 0)
            {
                // Set this points state to coarse
                state[row] = 1;

                // Set row nnz
                row_nnz[row] = 1;
            }

            return;
        }

        // Counter
        I nnz = 0;

        // Shared memory for the unordered set
        __shared__ I sdata[BLOCKSIZE / WFSIZE * HASHSIZE];

        // Each wavefront operates on its own set
        unordered_set<I, HASHSIZE, WFSIZE> set(sdata + wid * HASHSIZE);

        // Row entry and exit points
        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Loop over all columns of the i-th row, whereas each lane processes a column
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            // Skip points that do not influence the current point
            if(S[j] == false)
            {
                continue;
            }

            // Get the column index
            I col_j = csr_col_ind[j];

            // Skip diagonal entries (i does not influence itself)
            if(col_j == row)
            {
                continue;
            }

            // Switch between coarse and fine points that influence the i-th point
            if(cf[col_j] == COARSE)
            {
                // This is a coarse point and thus contributes, count it for the row nnz
                // We need to use a set here, to discard duplicates.
                nnz += set.insert(col_j);
            }
            else
            {
                // This is a fine point, check for strongly connected coarse points

                // Row entry and exit of this fine point
                J row_begin_j = csr_row_ptr[col_j];
                J row_end_j   = csr_row_ptr[col_j + 1];

                // Loop over all columns of the fine point
                for(J k = row_begin_j; k < row_end_j; ++k)
                {
                    // Skip points that do not influence the fine point
                    if(S[k] == false)
                    {
                        continue;
                    }

                    // Get the column index
                    I col_k = csr_col_ind[k];

                    // Skip diagonal entries (the fine point does not influence itself)
                    if(col_k == col_j)
                    {
                        continue;
                    }

                    // Check whether k is a coarse point
                    if(cf[col_k] == COARSE)
                    {
                        // This is a coarse point, it contributes, count it for the row nnz
                        // We need to use a set here, to discard duplicates.
                        nnz += set.insert(col_k);

                        // Stop if FF interpolation is limited
                        if(FF1 == true)
                        {
                            break;
                        }
                    }
                }
            }
        }

        // Sum up the row nnz from all lanes
        wf_reduce_sum<WFSIZE>(&nnz);

        if(lid == WFSIZE - 1)
        {
            // Write row nnz back to global memory
            row_nnz[row] = nnz;

            // Set this points state to fine
            state[row] = 0;
        }
    }

    template <unsigned int BLOCKSIZE,
              unsigned int WFSIZE,
              unsigned int HASHSIZE,
              typename T,
              typename I,
              typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_extpi_interp_fill(I    nrow,
                                             bool FF1,
                                             const J* __restrict__ csr_row_ptr,
                                             const I* __restrict__ csr_col_ind,
                                             const T* __restrict__ csr_val,
                                             const T* __restrict__ diag,
                                             const J* __restrict__ csr_row_ptr_P,
                                             I* __restrict__ csr_col_ind_P,
                                             T* __restrict__ csr_val_P,
                                             const bool* __restrict__ S,
                                             const int* __restrict__ cf,
                                             const I* __restrict__ f2c)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        if(WFSIZE == warpSize)
        {
            wid = __builtin_amdgcn_readfirstlane(wid);
        }

        // The row this thread operates on
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Some helpers for readability
        constexpr T zero = static_cast<T>(0);

        constexpr int COARSE = 1;
        constexpr int FINE   = 2;

        // Coarse points generate a single entry
        if(cf[row] == COARSE)
        {
            if(lid == 0)
            {
                // Get index into P
                J idx = csr_row_ptr_P[row];

                // Single entry in this row (coarse point)
                csr_col_ind_P[idx] = f2c[row];
                csr_val_P[idx]     = static_cast<T>(1);
            }

            return;
        }

        // Shared memory for the unordered map
        extern __shared__ char smem[];

        I* stable = reinterpret_cast<I*>(smem);
        T* sdata  = reinterpret_cast<T*>(stable + BLOCKSIZE / WFSIZE * HASHSIZE);

        // Unordered map
        unordered_map<I, T, HASHSIZE, WFSIZE> map(&stable[wid * HASHSIZE], &sdata[wid * HASHSIZE]);

        // Fill the map according to the nnz pattern of P
        // This is identical to the nnz per row kernel

        // Row entry and exit points
        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Loop over all columns of the i-th row, whereas each lane processes a column
        for(J k = row_begin + lid; k < row_end; k += WFSIZE)
        {
            // Skip points that do not influence the current point
            if(S[k] == false)
            {
                continue;
            }

            // Get the column index
            I col_ik = csr_col_ind[k];

            // Skip diagonal entries (i does not influence itself)
            if(col_ik == row)
            {
                continue;
            }

            // Switch between coarse and fine points that influence the i-th point
            if(cf[col_ik] == COARSE)
            {
                // This is a coarse point and thus contributes
                map.insert(col_ik);
            }
            else
            {
                // This is a fine point, check for strongly connected coarse points

                // Row entry and exit of this fine point
                J row_begin_k = csr_row_ptr[col_ik];
                J row_end_k   = csr_row_ptr[col_ik + 1];

                // Loop over all columns of the fine point
                for(J l = row_begin_k; l < row_end_k; ++l)
                {
                    // Skip points that do not influence the fine point
                    if(S[l] == false)
                    {
                        continue;
                    }

                    // Get the column index
                    I col_kl = csr_col_ind[l];

                    // Skip diagonal entries (the fine point does not influence itself)
                    if(col_kl == col_ik)
                    {
                        continue;
                    }

                    // Check whether l is a coarse point
                    if(cf[col_kl] == COARSE)
                    {
                        // This is a coarse point, it contributes
                        map.insert(col_kl);

                        // Stop if FF interpolation is limited
                        if(FF1 == true)
                        {
                            break;
                        }
                    }
                }
            }
        }

        // Now, we need to do the numerical part

        // Diagonal entry of i-th row
        T val_ii = diag[row];

        // Sign of diagonal entry of i-th row
        bool pos_ii = val_ii >= zero;

        // Accumulators
        T sum_k = zero;
        T sum_n = zero;

        // Loop over all columns of the i-th row, whereas each lane processes a column
        for(J k = row_begin + lid; k < row_end; k += WFSIZE)
        {
            // Get the column index
            I col_ik = csr_col_ind[k];

            // Skip diagonal entries (i does not influence itself)
            if(col_ik == row)
            {
                continue;
            }

            // Get the column value
            T val_ik = csr_val[k];

            // Check, whether the k-th entry of the row is a fine point and strongly
            // connected to the i-th point (e.g. k \in F^S_i)
            if(S[k] == true && cf[col_ik] == FINE)
            {
                // Accumulator for the sum over l
                T sum_l = zero;

                // Diagonal entry of k-th row
                T val_kk = diag[col_ik];

                // Store a_ki, if present
                T val_ki = zero;

                // Row entry and exit of this fine point
                J row_begin_k = csr_row_ptr[col_ik];
                J row_end_k   = csr_row_ptr[col_ik + 1];

                // Loop over all columns of the fine point
                for(J l = row_begin_k; l < row_end_k; ++l)
                {
                    // Get the column index
                    I col_kl = csr_col_ind[l];

                    // Get the column value
                    T val_kl = csr_val[l];

                    // Sign of a_kl
                    bool pos_kl = val_kl >= zero;

                    // Differentiate between diagonal and off-diagonal
                    if(col_kl == row)
                    {
                        // Column that matches the i-th row
                        // Since we sum up all l in C^hat_i and i, the diagonal need to
                        // be added to the sum over l, e.g. a^bar_kl
                        // a^bar contributes only, if the sign is different to the
                        // i-th row diagonal sign.
                        if(pos_ii != pos_kl)
                        {
                            sum_l += val_kl;
                        }

                        // If a_ki exists, keep it for later
                        val_ki = val_kl;
                    }
                    else if(cf[col_kl] == COARSE)
                    {
                        // Check if sign is different from i-th row diagonal
                        if(pos_ii != pos_kl)
                        {
                            // Entry contributes only, if it is a coarse point
                            // and part of C^hat (e.g. we need to check the map)
                            if(map.contains(col_kl))
                            {
                                sum_l += val_kl;
                            }
                        }
                    }
                }

                // Update sum over l with a_ik
                sum_l = val_ik / sum_l;

                // Compute the sign of a_kk and a_ki, we need this for a_bar
                bool pos_kk = val_kk >= zero;
                bool pos_ki = val_ki >= zero;

                // Additionally, for eq19 we need to add all coarse points in row k,
                // if they have different sign than the diagonal a_kk
                for(J l = row_begin_k; l < row_end_k; ++l)
                {
                    // Get the column index
                    I col_kl = csr_col_ind[l];

                    // Only coarse points contribute
                    if(cf[col_kl] != COARSE)
                    {
                        continue;
                    }

                    // Get the column value
                    T val_kl = csr_val[l];

                    // Compute the sign of a_kl
                    bool pos_kl = val_kl >= zero;

                    // Check for different sign
                    if(pos_kk != pos_kl)
                    {
                        // Add to map, only if the element exists already
                        map.add(col_kl, val_kl * sum_l);
                    }
                }

                // If sign of a_ki and a_kk are different, a_ki contributes to the
                // sum over k in F^S_i
                if(pos_kk != pos_ki)
                {
                    sum_k += val_ki * sum_l;
                }
            }

            // Boolean, to flag whether a_ik is in C hat or not
            // (we can query the map for it)
            bool in_C_hat = false;

            // a_ik can only be in C^hat if it is coarse
            if(cf[col_ik] == COARSE)
            {
                // Append a_ik to the sum of eq19
                in_C_hat = map.add(col_ik, val_ik);
            }

            // If a_ik is not in C^hat and does not strongly influence i, it contributes
            // to sum_n
            if(in_C_hat == false && S[k] == false)
            {
                sum_n += val_ik;
            }
        }

        // Each lane accumulates the sums (over n and l)
        T a_ii_tilde = sum_n + sum_k;

        // Now, each lane of the wavefront should hold the global row sum
        for(unsigned int i = WFSIZE >> 1; i > 0; i >>= 1)
        {
            a_ii_tilde += hip_shfl_xor(a_ii_tilde, i);
        }

        // Precompute -1 / (a_ii_tilde + a_ii)
        a_ii_tilde = static_cast<T>(-1) / (a_ii_tilde + val_ii);

        // Access into P
        J aj = csr_row_ptr_P[row];

        // Finally, extract the numerical values from the map and fill P such
        // that the resulting matrix is sorted by columns
        for(unsigned int i = lid; i < HASHSIZE; i += WFSIZE)
        {
            // Get column from map to fill into C hat
            I col = map.get_key(i);

            // Skip, if table is empty
            if(col == map.empty_key())
            {
                continue;
            }

            // Get index into P
            J idx = 0;

            // Hash table index counter
            unsigned int cnt = 0;

            // Go through the hash table, until we reach its end
            while(cnt < HASHSIZE)
            {
                // We are searching for the right place in P to
                // insert the i-th hash table entry.
                // If the i-th hash table column entry is greater then the current one,
                // we need to leave a slot to its left.
                if(col > map.get_key(cnt))
                {
                    ++idx;
                }

                // Process next hash table entry
                ++cnt;
            }

            // Add hash table entry into P
            csr_col_ind_P[aj + idx] = f2c[col];
            csr_val_P[aj + idx]     = a_ii_tilde * map.get_val(i);
        }
    }

    // Compress prolongation matrix
    template <unsigned int BLOCKSIZE, unsigned int WFSIZE, typename T, typename I, typename J>
    __launch_bounds__(BLOCKSIZE) __global__
        void kernel_csr_rs_extpi_interp_compress_nnz(I nrow,
                                                     const J* __restrict__ csr_row_ptr,
                                                     const I* __restrict__ csr_col_ind,
                                                     const T* __restrict__ csr_val,
                                                     float trunc,
                                                     J* __restrict__ row_nnz)
    {
        unsigned int lid = threadIdx.x & (WFSIZE - 1);
        unsigned int wid = threadIdx.x / WFSIZE;

        // Current row
        I row = blockIdx.x * BLOCKSIZE / WFSIZE + wid;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Row nnz counter
        I nnz = 0;

        double row_max = 0.0;

        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        // Obtain numbers for processing the row
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            // Compute absolute row maximum
            row_max = max(row_max, hip_abs(csr_val[j]));
        }

        // Gather from other lanes
        wf_reduce_max<WFSIZE>(&row_max);

        // Threshold
        double threshold = row_max * trunc;

        // Count the row nnz
        for(J j = row_begin + lid; j < row_end; j += WFSIZE)
        {
            // Check whether we keep this entry or not
            if(hip_abs(csr_val[j]) >= threshold)
            {
                // Count nnz
                ++nnz;
            }
        }

        // Gather from other lanes
        for(unsigned int i = WFSIZE >> 1; i > 0; i >>= 1)
        {
            nnz += __shfl_xor(nnz, i);
        }

        if(lid == 0)
        {
            // Write back to global memory
            row_nnz[row] = nnz;
        }
    }

    // Compress
    template <typename T, typename I, typename J>
    __global__ void kernel_csr_rs_extpi_interp_compress_fill(I nrow,
                                                             const J* __restrict__ csr_row_ptr,
                                                             const I* __restrict__ csr_col_ind,
                                                             const T* __restrict__ csr_val,
                                                             float trunc,
                                                             const J* __restrict__ comp_csr_row_ptr,
                                                             I* __restrict__ comp_csr_col_ind,
                                                             T* __restrict__ comp_csr_val)
    {
        // Current row
        I row = blockIdx.x * blockDim.x + threadIdx.x;

        // Do not run out of bounds
        if(row >= nrow)
        {
            return;
        }

        // Row entry and exit point
        J row_begin = csr_row_ptr[row];
        J row_end   = csr_row_ptr[row + 1];

        double row_max = 0.0;
        T      row_sum = static_cast<T>(0);

        // Obtain numbers for processing the row
        for(J j = row_begin; j < row_end; ++j)
        {
            // Get column value
            T val = csr_val[j];

            // Compute absolute row maximum
            row_max = max(row_max, hip_abs(val));

            // Compute row sum
            row_sum += val;
        }

        // Threshold
        double threshold = row_max * trunc;

        // Row entry point for the compressed matrix
        J comp_row_begin = comp_csr_row_ptr[row];
        J comp_row_end   = comp_csr_row_ptr[row + 1];

        // Row nnz counter
        I nnz = 0;

        // Row sum of not-dropped entries
        T comp_row_sum = static_cast<T>(0);

        for(J j = row_begin; j < row_end; ++j)
        {
            // Get column value
            T val = csr_val[j];

            // Check whether we keep this entry or not
            if(hip_abs(val) >= threshold)
            {
                // Compute compressed row sum
                comp_row_sum += val;

                // Fill compressed structures
                comp_csr_col_ind[comp_row_begin + nnz] = csr_col_ind[j];
                comp_csr_val[comp_row_begin + nnz]     = csr_val[j];

                ++nnz;
            }
        }

        // Row scaling factor
        T scale = row_sum / comp_row_sum;

        // Scale row entries
        for(J j = comp_row_begin; j < comp_row_end; ++j)
        {
            comp_csr_val[j] *= scale;
        }
    }
} // namespace rocalution

#endif // ROCALUTION_HIP_HIP_KERNELS_RSAMG_CSR_HPP_
