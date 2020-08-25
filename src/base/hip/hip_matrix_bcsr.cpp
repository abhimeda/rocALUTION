/* ************************************************************************
 * Copyright (c) 2018-2020 Advanced Micro Devices, Inc.
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

#include "hip_matrix_bcsr.hpp"
#include "../../utils/def.hpp"
#include "../../utils/log.hpp"
#include "../backend_manager.hpp"
#include "../base_matrix.hpp"
#include "../base_vector.hpp"
#include "../host/host_matrix_bcsr.hpp"
#include "../matrix_formats_ind.hpp"
#include "hip_allocate_free.hpp"
#include "hip_conversion.hpp"
#include "hip_matrix_csr.hpp"
#include "hip_sparse.hpp"
#include "hip_utils.hpp"
#include "hip_vector.hpp"

#include <hip/hip_runtime.h>

namespace rocalution
{

    template <typename ValueType>
    HIPAcceleratorMatrixBCSR<ValueType>::HIPAcceleratorMatrixBCSR()
    {
        // no default constructors
        LOG_INFO("no default constructor");
        FATAL_ERROR(__FILE__, __LINE__);
    }

    template <typename ValueType>
    HIPAcceleratorMatrixBCSR<ValueType>::HIPAcceleratorMatrixBCSR(
        const Rocalution_Backend_Descriptor local_backend)
    {
        log_debug(this,
                  "HIPAcceleratorMatrixBCSR::HIPAcceleratorMatrixBCSR()",
                  "constructor with local_backend");

        this->mat_.row_offset = NULL;
        this->mat_.col        = NULL;
        this->mat_.val        = NULL;
        this->set_backend(local_backend);

        this->mat_descr_ = 0;

        CHECK_HIP_ERROR(__FILE__, __LINE__);

        rocsparse_status status;

        status = rocsparse_create_mat_descr(&this->mat_descr_);
        CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);

        status = rocsparse_set_mat_index_base(this->mat_descr_, rocsparse_index_base_zero);
        CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);

        status = rocsparse_set_mat_type(this->mat_descr_, rocsparse_matrix_type_general);
        CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);
    }

    template <typename ValueType>
    HIPAcceleratorMatrixBCSR<ValueType>::~HIPAcceleratorMatrixBCSR()
    {
        log_debug(this, "HIPAcceleratorMatrixBCSR::~HIPAcceleratorMatrixBCSR()", "destructor");

        this->Clear();

        rocsparse_status status;

        status = rocsparse_destroy_mat_descr(this->mat_descr_);
        CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::Info(void) const
    {
        LOG_INFO("HIPAcceleratorMatrixBCSR<ValueType>");
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::AllocateBCSR(int nnzb,
                                                           int nrowb,
                                                           int ncolb,
                                                           int blockdim)
    {
        assert(nnzb >= 0);
        assert(ncolb >= 0);
        assert(nrowb >= 0);
        assert(blockdim > 1);

        if(this->nnz_ > 0)
        {
            this->Clear();
        }

        if(nnzb > 0)
        {
            allocate_hip(nrowb + 1, &this->mat_.row_offset);
            allocate_hip(nnzb, &this->mat_.col);
            allocate_hip(nnzb * blockdim * blockdim, &this->mat_.val);

            set_to_zero_hip(this->local_backend_.HIP_block_size, nrowb + 1, this->mat_.row_offset);
            set_to_zero_hip(this->local_backend_.HIP_block_size, nnzb, this->mat_.col);
            set_to_zero_hip(
                this->local_backend_.HIP_block_size, nnzb * blockdim * blockdim, this->mat_.val);

            this->nrow_ = nrowb * blockdim;
            this->ncol_ = ncolb * blockdim;
            this->nnz_  = nnzb * blockdim * blockdim;

            this->mat_.nrowb    = nrowb;
            this->mat_.ncolb    = ncolb;
            this->mat_.nnzb     = nnzb;
            this->mat_.blockdim = blockdim;
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::Clear()
    {
        if(this->nnz_ > 0)
        {
            free_hip(&this->mat_.row_offset);
            free_hip(&this->mat_.col);
            free_hip(&this->mat_.val);

            this->nrow_ = 0;
            this->ncol_ = 0;
            this->nnz_  = 0;
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::SetDataPtrBCSR(
        int** row_offset, int** col, ValueType** val, int nnzb, int nrowb, int ncolb, int blockdim)
    {
        assert(*row_offset != NULL);
        assert(*col != NULL);
        assert(*val != NULL);
        assert(nnzb > 0);
        assert(nrowb > 0);
        assert(ncolb > 0);
        assert(blockdim > 1);

        this->Clear();

        hipDeviceSynchronize();

        this->blockdim_ = blockdim;
        this->nrow_     = nrowb * blockdim;
        this->ncol_     = ncolb * blockdim;
        this->nnz_      = nnzb * blockdim * blockdim;

        this->mat_.nrowb    = nrowb;
        this->mat_.ncolb    = ncolb;
        this->mat_.nnzb     = nnzb;
        this->mat_.blockdim = blockdim;

        this->mat_.row_offset = *row_offset;
        this->mat_.col        = *col;
        this->mat_.val        = *val;
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::LeaveDataPtrBCSR(int**       row_offset,
                                                               int**       col,
                                                               ValueType** val,
                                                               int&        blockdim)
    {
        assert(this->nrow_ > 0);
        assert(this->ncol_ > 0);
        assert(this->nnz_ > 0);
        assert(this->mat_.blockdim > 1);

        hipDeviceSynchronize();

        *row_offset = this->mat_.row_offset;
        *col        = this->mat_.col;
        *val        = this->mat_.val;

        this->mat_.row_offset = NULL;
        this->mat_.col        = NULL;
        this->mat_.val        = NULL;

        blockdim = this->mat_.blockdim;

        this->mat_.blockdim = 0;

        this->nrow_ = 0;
        this->ncol_ = 0;
        this->nnz_  = 0;
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyFromHost(const HostMatrix<ValueType>& src)
    {
        const HostMatrixBCSR<ValueType>* cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == src.GetMatFormat());

        // CPU to HIP copy
        if((cast_mat = dynamic_cast<const HostMatrixBCSR<ValueType>*>(&src)) != NULL)
        {
            if(this->nnz_ == 0)
            {
                this->AllocateBCSR(cast_mat->mat_.nnzb,
                                   cast_mat->mat_.nrowb,
                                   cast_mat->mat_.ncolb,
                                   cast_mat->mat_.blockdim);
            }

            assert(this->nnz_ == cast_mat->nnz_);
            assert(this->nrow_ == cast_mat->nrow_);
            assert(this->ncol_ == cast_mat->ncol_);
            assert(this->mat_.nrowb == cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == cast_mat->mat_.blockdim);

            hipMemcpy(this->mat_.row_offset,
                      cast_mat->mat_.row_offset,
                      (this->mat_.nrowb + 1) * sizeof(int),
                      hipMemcpyHostToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(this->mat_.col,
                      cast_mat->mat_.col,
                      this->mat_.nnzb * sizeof(int),
                      hipMemcpyHostToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(this->mat_.val,
                      cast_mat->mat_.val,
                      this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                          * sizeof(ValueType),
                      hipMemcpyHostToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);
        }
        else
        {
            LOG_INFO("Error unsupported HIP matrix type");
            this->Info();
            src.Info();
            FATAL_ERROR(__FILE__, __LINE__);
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyToHost(HostMatrix<ValueType>* dst) const
    {
        HostMatrixBCSR<ValueType>* cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == dst->GetMatFormat());

        // HIP to CPU copy
        if((cast_mat = dynamic_cast<HostMatrixBCSR<ValueType>*>(dst)) != NULL)
        {
            cast_mat->set_backend(this->local_backend_);

            if(cast_mat->nnz_ == 0)
            {
                cast_mat->AllocateBCSR(
                    this->mat_.nnzb, this->mat_.nrowb, this->mat_.ncolb, this->mat_.blockdim);
            }

            assert(this->nnz_ == cast_mat->nnz_);
            assert(this->nrow_ == cast_mat->nrow_);
            assert(this->ncol_ == cast_mat->ncol_);
            assert(this->mat_.nrowb == cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == cast_mat->mat_.blockdim);

            hipMemcpy(cast_mat->mat_.row_offset,
                      this->mat_.row_offset,
                      (this->mat_.nrowb + 1) * sizeof(int),
                      hipMemcpyDeviceToHost);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(cast_mat->mat_.col,
                      this->mat_.col,
                      this->mat_.nnzb * sizeof(int),
                      hipMemcpyDeviceToHost);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(cast_mat->mat_.val,
                      this->mat_.val,
                      this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                          * sizeof(ValueType),
                      hipMemcpyDeviceToHost);
            CHECK_HIP_ERROR(__FILE__, __LINE__);
        }
        else
        {
            LOG_INFO("Error unsupported HIP matrix type");
            this->Info();
            dst->Info();
            FATAL_ERROR(__FILE__, __LINE__);
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyFrom(const BaseMatrix<ValueType>& src)
    {
        const HIPAcceleratorMatrixBCSR<ValueType>* hip_cast_mat;
        const HostMatrix<ValueType>*               host_cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == src.GetMatFormat());

        // HIP to HIP copy
        if((hip_cast_mat = dynamic_cast<const HIPAcceleratorMatrixBCSR<ValueType>*>(&src)) != NULL)
        {
            if(this->nnz_ == 0)
            {
                this->AllocateBCSR(hip_cast_mat->mat_.nnzb,
                                   hip_cast_mat->mat_.nrowb,
                                   hip_cast_mat->mat_.ncolb,
                                   hip_cast_mat->mat_.blockdim);
            }

            assert(this->nnz_ == hip_cast_mat->nnz_);
            assert(this->nrow_ == hip_cast_mat->nrow_);
            assert(this->ncol_ == hip_cast_mat->ncol_);
            assert(this->mat_.nrowb == hip_cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == hip_cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == hip_cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == hip_cast_mat->mat_.blockdim);

            hipMemcpy(this->mat_.row_offset,
                      hip_cast_mat->mat_.row_offset,
                      (this->mat_.nrowb + 1) * sizeof(int),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(this->mat_.col,
                      hip_cast_mat->mat_.col,
                      this->mat_.nnzb * sizeof(int),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(this->mat_.val,
                      hip_cast_mat->mat_.val,
                      this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                          * sizeof(ValueType),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);
        }
        else
        {
            // CPU to HIP
            if((host_cast_mat = dynamic_cast<const HostMatrix<ValueType>*>(&src)) != NULL)
            {
                this->CopyFromHost(*host_cast_mat);
            }
            else
            {
                LOG_INFO("Error unsupported HIP matrix type");
                this->Info();
                src.Info();
                FATAL_ERROR(__FILE__, __LINE__);
            }
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyTo(BaseMatrix<ValueType>* dst) const
    {
        HIPAcceleratorMatrixBCSR<ValueType>* hip_cast_mat;
        HostMatrix<ValueType>*               host_cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == dst->GetMatFormat());

        // HIP to HIP copy
        if((hip_cast_mat = dynamic_cast<HIPAcceleratorMatrixBCSR<ValueType>*>(dst)) != NULL)
        {
            hip_cast_mat->set_backend(this->local_backend_);

            if(hip_cast_mat->nnz_ == 0)
            {
                hip_cast_mat->AllocateBCSR(
                    this->mat_.nnzb, this->mat_.nrowb, this->mat_.ncolb, this->mat_.blockdim);
            }

            assert(this->nnz_ == hip_cast_mat->nnz_);
            assert(this->nrow_ == hip_cast_mat->nrow_);
            assert(this->ncol_ == hip_cast_mat->ncol_);
            assert(this->mat_.nrowb == hip_cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == hip_cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == hip_cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == hip_cast_mat->mat_.blockdim);

            hipMemcpy(hip_cast_mat->mat_.row_offset,
                      this->mat_.row_offset,
                      (this->mat_.nrowb + 1) * sizeof(int),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(hip_cast_mat->mat_.col,
                      this->mat_.col,
                      this->mat_.nnzb * sizeof(int),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);

            hipMemcpy(hip_cast_mat->mat_.val,
                      this->mat_.val,
                      this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                          * sizeof(ValueType),
                      hipMemcpyDeviceToDevice);
            CHECK_HIP_ERROR(__FILE__, __LINE__);
        }
        else
        {
            // HIP to CPU
            if((host_cast_mat = dynamic_cast<HostMatrix<ValueType>*>(dst)) != NULL)
            {
                this->CopyToHost(host_cast_mat);
            }
            else
            {
                LOG_INFO("Error unsupported HIP matrix type");
                this->Info();
                dst->Info();
                FATAL_ERROR(__FILE__, __LINE__);
            }
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyFromHostAsync(const HostMatrix<ValueType>& src)
    {
        const HostMatrixBCSR<ValueType>* cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == src.GetMatFormat());

        // CPU to HIP copy
        if((cast_mat = dynamic_cast<const HostMatrixBCSR<ValueType>*>(&src)) != NULL)
        {
            if(this->nnz_ == 0)
            {
                this->AllocateBCSR(cast_mat->mat_.nnzb,
                                   cast_mat->mat_.nrowb,
                                   cast_mat->mat_.ncolb,
                                   cast_mat->mat_.blockdim);
            }

            assert(this->nnz_ == cast_mat->nnz_);
            assert(this->nrow_ == cast_mat->nrow_);
            assert(this->ncol_ == cast_mat->ncol_);
            assert(this->mat_.nrowb == cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == cast_mat->mat_.blockdim);

            if(this->nnz_ > 0)
            {
                hipMemcpyAsync(this->mat_.row_offset,
                               cast_mat->mat_.row_offset,
                               (this->mat_.nrowb + 1) * sizeof(int),
                               hipMemcpyHostToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(this->mat_.col,
                               cast_mat->mat_.col,
                               this->mat_.nnzb * sizeof(int),
                               hipMemcpyHostToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(this->mat_.val,
                               cast_mat->mat_.val,
                               this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                                   * sizeof(int),
                               hipMemcpyHostToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);
            }
        }
        else
        {
            LOG_INFO("Error unsupported HIP matrix type");
            this->Info();
            src.Info();
            FATAL_ERROR(__FILE__, __LINE__);
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyToHostAsync(HostMatrix<ValueType>* dst) const
    {
        HostMatrixBCSR<ValueType>* cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == dst->GetMatFormat());

        // HIP to CPU copy
        if((cast_mat = dynamic_cast<HostMatrixBCSR<ValueType>*>(dst)) != NULL)
        {
            cast_mat->set_backend(this->local_backend_);

            if(cast_mat->nnz_ == 0)
            {
                cast_mat->AllocateBCSR(
                    this->mat_.nnzb, this->mat_.nrowb, this->mat_.ncolb, this->mat_.blockdim);
            }

            assert(this->nnz_ == cast_mat->nnz_);
            assert(this->nrow_ == cast_mat->nrow_);
            assert(this->ncol_ == cast_mat->ncol_);
            assert(this->mat_.nrowb == cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == cast_mat->mat_.blockdim);

            if(this->nnz_ > 0)
            {
                hipMemcpyAsync(cast_mat->mat_.row_offset,
                               this->mat_.row_offset,
                               (this->mat_.nrowb + 1) * sizeof(int),
                               hipMemcpyDeviceToHost);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(cast_mat->mat_.col,
                               this->mat_.col,
                               this->mat_.nnzb * sizeof(int),
                               hipMemcpyDeviceToHost);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(cast_mat->mat_.val,
                               this->mat_.val,
                               this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                                   * sizeof(int),
                               hipMemcpyDeviceToHost);
                CHECK_HIP_ERROR(__FILE__, __LINE__);
            }
        }
        else
        {
            LOG_INFO("Error unsupported HIP matrix type");
            this->Info();
            dst->Info();
            FATAL_ERROR(__FILE__, __LINE__);
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyFromAsync(const BaseMatrix<ValueType>& src)
    {
        const HIPAcceleratorMatrixBCSR<ValueType>* hip_cast_mat;
        const HostMatrix<ValueType>*               host_cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == src.GetMatFormat());

        // HIP to HIP copy
        if((hip_cast_mat = dynamic_cast<const HIPAcceleratorMatrixBCSR<ValueType>*>(&src)) != NULL)
        {
            if(this->nnz_ == 0)
            {
                this->AllocateBCSR(hip_cast_mat->mat_.nnzb,
                                   hip_cast_mat->mat_.nrowb,
                                   hip_cast_mat->mat_.ncolb,
                                   hip_cast_mat->mat_.blockdim);
            }

            assert(this->nnz_ == hip_cast_mat->nnz_);
            assert(this->nrow_ == hip_cast_mat->nrow_);
            assert(this->ncol_ == hip_cast_mat->ncol_);
            assert(this->mat_.nrowb == hip_cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == hip_cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == hip_cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == hip_cast_mat->mat_.blockdim);

            if(this->nnz_ > 0)
            {
                hipMemcpyAsync(this->mat_.row_offset,
                               hip_cast_mat->mat_.row_offset,
                               (this->mat_.nrowb + 1) * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(this->mat_.col,
                               hip_cast_mat->mat_.col,
                               this->mat_.nnzb * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(this->mat_.val,
                               hip_cast_mat->mat_.val,
                               this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                                   * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);
            }
        }
        else
        {
            // CPU to HIP
            if((host_cast_mat = dynamic_cast<const HostMatrix<ValueType>*>(&src)) != NULL)
            {
                this->CopyFromHostAsync(*host_cast_mat);
            }
            else
            {
                LOG_INFO("Error unsupported HIP matrix type");
                this->Info();
                src.Info();
                FATAL_ERROR(__FILE__, __LINE__);
            }
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::CopyToAsync(BaseMatrix<ValueType>* dst) const
    {
        HIPAcceleratorMatrixBCSR<ValueType>* hip_cast_mat;
        HostMatrix<ValueType>*               host_cast_mat;

        // copy only in the same format
        assert(this->GetMatFormat() == dst->GetMatFormat());

        // HIP to HIP copy
        if((hip_cast_mat = dynamic_cast<HIPAcceleratorMatrixBCSR<ValueType>*>(dst)) != NULL)
        {
            hip_cast_mat->set_backend(this->local_backend_);

            if(this->nnz_ == 0)
            {
                hip_cast_mat->AllocateBCSR(
                    this->mat_.nnzb, this->mat_.nrowb, this->mat_.ncolb, this->mat_.blockdim);
            }

            assert(this->nnz_ == hip_cast_mat->nnz_);
            assert(this->nrow_ == hip_cast_mat->nrow_);
            assert(this->ncol_ == hip_cast_mat->ncol_);
            assert(this->mat_.nrowb == hip_cast_mat->mat_.nrowb);
            assert(this->mat_.ncolb == hip_cast_mat->mat_.ncolb);
            assert(this->mat_.nnzb == hip_cast_mat->mat_.nnzb);
            assert(this->mat_.blockdim == hip_cast_mat->mat_.blockdim);

            if(this->nnz_ > 0)
            {
                hipMemcpyAsync(hip_cast_mat->mat_.row_offset,
                               this->mat_.row_offset,
                               (this->mat_.nrowb + 1) * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(hip_cast_mat->mat_.col,
                               this->mat_.col,
                               this->mat_.nnzb * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);

                hipMemcpyAsync(hip_cast_mat->mat_.val,
                               this->mat_.val,
                               this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim
                                   * sizeof(int),
                               hipMemcpyDeviceToDevice);
                CHECK_HIP_ERROR(__FILE__, __LINE__);
            }
        }
        else
        {
            // HIP to CPU
            if((host_cast_mat = dynamic_cast<HostMatrix<ValueType>*>(dst)) != NULL)
            {
                this->CopyToHostAsync(host_cast_mat);
            }
            else
            {
                LOG_INFO("Error unsupported HIP matrix type");
                this->Info();
                dst->Info();
                FATAL_ERROR(__FILE__, __LINE__);
            }
        }
    }

    template <typename ValueType>
    bool HIPAcceleratorMatrixBCSR<ValueType>::ConvertFrom(const BaseMatrix<ValueType>& mat)
    {
        this->Clear();

        // empty matrix is empty matrix
        if(mat.GetNnz() == 0)
        {
            return true;
        }

        const HIPAcceleratorMatrixBCSR<ValueType>* cast_mat_bcsr;

        if((cast_mat_bcsr = dynamic_cast<const HIPAcceleratorMatrixBCSR<ValueType>*>(&mat)) != NULL)
        {
            this->CopyFrom(*cast_mat_bcsr);
            return true;
        }

        const HIPAcceleratorMatrixCSR<ValueType>* cast_mat_csr;
        if((cast_mat_csr = dynamic_cast<const HIPAcceleratorMatrixCSR<ValueType>*>(&mat)) != NULL)
        {
            this->Clear();

            this->mat_.blockdim = this->blockdim_;

            if(csr_to_bcsr_hip(ROCSPARSE_HANDLE(this->local_backend_.ROC_sparse_handle),
                               cast_mat_csr->nnz_,
                               cast_mat_csr->nrow_,
                               cast_mat_csr->ncol_,
                               cast_mat_csr->mat_,
                               cast_mat_csr->mat_descr_,
                               &this->mat_,
                               this->mat_descr_)
               == true)
            {
                this->nrow_ = this->mat_.nrowb * this->mat_.blockdim;
                this->ncol_ = this->mat_.ncolb * this->mat_.blockdim;
                this->nnz_  = this->mat_.nnzb * this->mat_.blockdim * this->mat_.blockdim;

                return true;
            }
        }

        return false;
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::Apply(const BaseVector<ValueType>& in,
                                                    BaseVector<ValueType>*       out) const
    {
        if(this->nnz_ > 0)
        {
            assert(in.GetSize() >= 0);
            assert(out->GetSize() >= 0);
            assert(in.GetSize() == this->ncol_);
            assert(out->GetSize() == this->nrow_);

            const HIPAcceleratorVector<ValueType>* cast_in
                = dynamic_cast<const HIPAcceleratorVector<ValueType>*>(&in);
            HIPAcceleratorVector<ValueType>* cast_out
                = dynamic_cast<HIPAcceleratorVector<ValueType>*>(out);

            assert(cast_in != NULL);
            assert(cast_out != NULL);

            ValueType alpha = 1.0;
            ValueType beta  = 0.0;

            // Determine whether we are using row or column major for the blocks
            rocsparse_direction dir
                = BCSR_IND_BASE ? rocsparse_direction_row : rocsparse_direction_column;

            rocsparse_status status;
            status = rocsparseTbsrmv(ROCSPARSE_HANDLE(this->local_backend_.ROC_sparse_handle),
                                     dir,
                                     rocsparse_operation_none,
                                     this->mat_.nrowb,
                                     this->mat_.ncolb,
                                     this->mat_.nnzb,
                                     &alpha,
                                     this->mat_descr_,
                                     this->mat_.val,
                                     this->mat_.row_offset,
                                     this->mat_.col,
                                     this->mat_.blockdim,
                                     cast_in->vec_,
                                     &beta,
                                     cast_out->vec_);
            CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);
        }
    }

    template <typename ValueType>
    void HIPAcceleratorMatrixBCSR<ValueType>::ApplyAdd(const BaseVector<ValueType>& in,
                                                       ValueType                    scalar,
                                                       BaseVector<ValueType>*       out) const
    {
        if(this->nnz_ > 0)
        {
            assert(in.GetSize() >= 0);
            assert(out->GetSize() >= 0);
            assert(in.GetSize() == this->ncol_);
            assert(out->GetSize() == this->nrow_);

            const HIPAcceleratorVector<ValueType>* cast_in
                = dynamic_cast<const HIPAcceleratorVector<ValueType>*>(&in);
            HIPAcceleratorVector<ValueType>* cast_out
                = dynamic_cast<HIPAcceleratorVector<ValueType>*>(out);

            assert(cast_in != NULL);
            assert(cast_out != NULL);

            ValueType beta = 1.0;

            // Determine whether we are using row or column major for the blocks
            rocsparse_direction dir
                = BCSR_IND_BASE ? rocsparse_direction_row : rocsparse_direction_column;

            rocsparse_status status;
            status = rocsparseTbsrmv(ROCSPARSE_HANDLE(this->local_backend_.ROC_sparse_handle),
                                     dir,
                                     rocsparse_operation_none,
                                     this->mat_.nrowb,
                                     this->mat_.ncolb,
                                     this->mat_.nnzb,
                                     &scalar,
                                     this->mat_descr_,
                                     this->mat_.val,
                                     this->mat_.row_offset,
                                     this->mat_.col,
                                     this->mat_.blockdim,
                                     cast_in->vec_,
                                     &beta,
                                     cast_out->vec_);
            CHECK_ROCSPARSE_ERROR(status, __FILE__, __LINE__);
        }
    }

    template class HIPAcceleratorMatrixBCSR<double>;
    template class HIPAcceleratorMatrixBCSR<float>;
#ifdef SUPPORT_COMPLEX
    template class HIPAcceleratorMatrixBCSR<std::complex<double>>;
    template class HIPAcceleratorMatrixBCSR<std::complex<float>>;
#endif

} // namespace rocalution
