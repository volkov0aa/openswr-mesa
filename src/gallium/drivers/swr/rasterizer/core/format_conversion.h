/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* @file format_conversion.h
*
* @brief API implementation
*
******************************************************************************/
#include "format_types.h"
#include "format_traits.h"

//////////////////////////////////////////////////////////////////////////
/// @brief Load SIMD packed pixels in SOA format and converts to
///        SOA RGBA32_FLOAT format.
/// @param pSrc - source data in SOA form
/// @param dst - output data in SOA form
template<SWR_FORMAT SrcFormat>
INLINE void LoadSOA(const BYTE *pSrc, simdvector &dst)
{
    // fast path for float32
    if ((FormatTraits<SrcFormat>::GetType(0) == SWR_TYPE_FLOAT) && (FormatTraits<SrcFormat>::GetBPC(0) == 32))
    {
        auto lambda = [&](int comp)
        {
            simdscalar vComp = _simd_load_ps((const float*)(pSrc + comp*sizeof(simdscalar)));

            dst.v[FormatTraits<SrcFormat>::swizzle(comp)] = vComp;
        };

        UnrollerL<0, FormatTraits<SrcFormat>::numComps, 1>::step(lambda);
        return;
    }

    auto lambda = [&](int comp)
    {
        // load SIMD components
        simdscalar vComp = FormatTraits<SrcFormat>::loadSOA(comp, pSrc);

        // unpack
        vComp = FormatTraits<SrcFormat>::unpack(comp, vComp);

        // convert
        if (FormatTraits<SrcFormat>::isNormalized(comp))
        {
            vComp = _simd_cvtepi32_ps(_simd_castps_si(vComp));
            vComp = _simd_mul_ps(vComp, _simd_set1_ps(FormatTraits<SrcFormat>::toFloat(comp)));
        }

        dst.v[FormatTraits<SrcFormat>::swizzle(comp)] = vComp;

        pSrc += (FormatTraits<SrcFormat>::GetBPC(comp) * KNOB_SIMD_WIDTH) / 8;
    };

    UnrollerL<0, FormatTraits<SrcFormat>::numComps, 1>::step(lambda);
}

//////////////////////////////////////////////////////////////////////////
/// @brief Convert and store simdvector of pixels in SOA
///        RGBA32_FLOAT to SOA format
/// @param src - source data in SOA form
/// @param dst - output data in SOA form
template<SWR_FORMAT DstFormat>
INLINE void StoreSOA(const simdvector &src, BYTE *pDst)
{
    // fast path for float32
    if ((FormatTraits<DstFormat>::GetType(0) == SWR_TYPE_FLOAT) && (FormatTraits<DstFormat>::GetBPC(0) == 32))
    {
        for (uint32_t comp = 0; comp < FormatTraits<DstFormat>::numComps; ++comp)
        {
            simdscalar vComp = src.v[FormatTraits<DstFormat>::swizzle(comp)];

            // Gamma-correct
            if (FormatTraits<DstFormat>::isSRGB)
            {
                if (comp < 3)  // Input format is always RGBA32_FLOAT.
                {
                    vComp = FormatTraits<R32G32B32A32_FLOAT>::convertSrgb(comp, vComp);
                }
            }

            _simd_store_ps((float*)(pDst + comp*sizeof(simdscalar)), vComp);
        }
        return;
    }

    auto lambda = [&](int comp)
    {
        simdscalar vComp = src.v[FormatTraits<DstFormat>::swizzle(comp)];

        // Gamma-correct
        if (FormatTraits<DstFormat>::isSRGB)
        {
            if (comp < 3)  // Input format is always RGBA32_FLOAT.
            {
                vComp = FormatTraits<R32G32B32A32_FLOAT>::convertSrgb(comp, vComp);
            }
        }

        // convert
        if (FormatTraits<DstFormat>::isNormalized(comp))
        {
            if (FormatTraits<DstFormat>::GetType(comp) == SWR_TYPE_UNORM)
            {
                vComp = _simd_max_ps(vComp, _simd_setzero_ps());
            }

            if (FormatTraits<DstFormat>::GetType(comp) == SWR_TYPE_SNORM)
            {
                vComp = _simd_max_ps(vComp, _simd_set1_ps(-1.0f));
            }
            vComp = _simd_min_ps(vComp, _simd_set1_ps(1.0f));

            vComp = _simd_mul_ps(vComp, _simd_set1_ps(FormatTraits<DstFormat>::fromFloat(comp)));
            vComp = _simd_castsi_ps(_simd_cvtps_epi32(vComp));
        }
        else if (FormatTraits<DstFormat>::GetBPC(comp) < 32)
        {
            if (FormatTraits<DstFormat>::GetType(comp) == SWR_TYPE_UINT)
            {
                int iMax = (1 << FormatTraits<DstFormat>::GetBPC(comp)) - 1;
                int iMin = 0;
                simdscalari vCompi = _simd_castps_si(vComp);
                vCompi = _simd_max_epu32(vCompi, _simd_set1_epi32(iMin));
                vCompi = _simd_min_epu32(vCompi, _simd_set1_epi32(iMax));
                vComp = _simd_castsi_ps(vCompi);
            }
            else if (FormatTraits<DstFormat>::GetType(comp) == SWR_TYPE_SINT)
            {
                int iMax = (1 << (FormatTraits<DstFormat>::GetBPC(comp) - 1)) - 1;
                int iMin = -1 - iMax;
                simdscalari vCompi = _simd_castps_si(vComp);
                vCompi = _simd_max_epi32(vCompi, _simd_set1_epi32(iMin));
                vCompi = _simd_min_epi32(vCompi, _simd_set1_epi32(iMax));
                vComp = _simd_castsi_ps(vCompi);
            }
        }

        // pack
        vComp = FormatTraits<DstFormat>::pack(comp, vComp);

        // store
        FormatTraits<DstFormat>::storeSOA(comp, pDst, vComp);

        pDst += (FormatTraits<DstFormat>::GetBPC(comp) * KNOB_SIMD_WIDTH) / 8;
    };

    UnrollerL<0, FormatTraits<DstFormat>::numComps, 1>::step(lambda);
}
